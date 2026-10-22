// The catalog of an open project (architecture 6): both scan stages on a real folder, the
// statuses they find, saves of this program, changes by other programs, and rebuilding
// after the catalog file is deleted.

#include "application/ProjectCatalog.h"
#include "application/ProjectSession.h"
#include "common/FolderBlocker.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace liteanno;
using namespace liteanno::application;

namespace {

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

QByteArray annotation(const char* imagePath, const char* extra, int width = 200)
{
    return QByteArray(R"({"version": "2.4.0", "flags": {}, "shapes": [{"label": "scratch", "points": [[1.0, 1.0], [50.0, 40.0]], )"
                      R"("group_id": null, "shape_type": "rectangle", "flags": {}}], "imagePath": ")")
           + imagePath + R"(", "imageData": null, "imageHeight": 100, "imageWidth": )" + QByteArray::number(width) + extra
           + "}";
}

// Every image is 200 x 100 for these tests; the files need not be real images.
std::optional<ImageInfo> fakeImageInfo(const QString& path)
{
    ImageInfo info;
    info.fileName = QFileInfo(path).fileName();
    info.width = 200;
    info.height = 100;
    return info;
}

struct Setup {
    QTemporaryDir local;
    QTemporaryDir data;
    std::unique_ptr<ProjectSession> project;

    Setup()
    {
        writeFile(data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", )"
                  R"("shapeTypes": ["rectangle"], "classes": [{"id": 0, "name": "scratch"}]})");
        for (const char* image : {"1.png", "2.png", "3.png", "4.png", "5.png", "5.jpg", "6.png"})
            writeFile(data.filePath(QStringLiteral("line/") + QLatin1String(image)), "image");
        writeFile(data.filePath(QStringLiteral("line/2.json")), annotation("2.png", R"(, "checked": true)"));
        writeFile(data.filePath(QStringLiteral("line/3.json")), "{");
        writeFile(data.filePath(QStringLiteral("line/4.json")),
                  annotation("4.png", R"(, "checked": false, "liteanno": {"formatVersion": "1.0", "status": "skipped", "skipReason": "blurred"})"));
        // Written for an image of another size: confirmed, but the size does not match.
        writeFile(data.filePath(QStringLiteral("line/6.json")), annotation("6.png", R"(, "checked": true)", 300));
        ProjectSession::OpenResult opened = ProjectSession::open(data.path(), LocalDataPaths(local.path()));
        if (!opened.session)
            qFatal("cannot open the project: %s", qPrintable(opened.message));
        project = std::move(opened.session);
    }

    QString path(const char* relative) const { return data.filePath(QLatin1String(relative)); }

    // Opens the project again, e.g. after its project file changed.
    void reopen()
    {
        project.reset();
        ProjectSession::OpenResult opened = ProjectSession::open(data.path(), LocalDataPaths(local.path()));
        if (!opened.session)
            qFatal("cannot open the project: %s", qPrintable(opened.message));
        project = std::move(opened.session);
    }
};

QMap<QString, QString> statuses(const ProjectCatalog& catalog)
{
    QMap<QString, QString> map;
    for (const dataset::CatalogEntry& entry : catalog.reader().images())
        map.insert(entry.path, entry.status && entry.verified ? QLatin1String(annotationStatusName(*entry.status))
                                                              : QStringLiteral("unchecked"));
    return map;
}

const QMap<QString, QString> kExpected = {
    {QStringLiteral("line/1.png"), QStringLiteral("unprocessed")},
    {QStringLiteral("line/2.png"), QStringLiteral("confirmed")},
    {QStringLiteral("line/3.png"), QStringLiteral("corrupt")},
    {QStringLiteral("line/4.png"), QStringLiteral("skipped")},
    {QStringLiteral("line/5.jpg"), QStringLiteral("name-collision")},
    {QStringLiteral("line/5.png"), QStringLiteral("name-collision")},
    {QStringLiteral("line/6.png"), QStringLiteral("confirmed-with-problems")},
};

} // namespace

class TestProjectCatalog : public QObject
{
    Q_OBJECT

private slots:
    void scanAndCheck()
    {
        Setup setup;
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        QVERIFY2(catalog.isOpen(), qPrintable(catalog.error()));
        QSignalSpy scanned(&catalog, &ProjectCatalog::scanFinished);
        QSignalSpy checked(&catalog, &ProjectCatalog::checkFinished);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QVERIFY(scanned.wait(1000) || scanned.size() == 1);
        QCOMPARE(scanned.first().first().toBool(), true);
        QVERIFY(checked.size() == 1 || checked.wait(1000));
        QCOMPARE(statuses(catalog), kExpected);
        QCOMPARE(catalog.reader().needingCheckCount(), 0);
        QCOMPARE(catalog.reader().statusCounts().value(AnnotationStatus::NameCollision), 2);
    }

    // Architecture 6.3: delete the catalog and the next scan builds the same one.
    void deletedCatalogIsRebuilt()
    {
        Setup setup;
        QString database;
        {
            ProjectCatalog catalog(*setup.project, fakeImageInfo);
            catalog.start();
            QVERIFY(catalog.waitUntilIdle(10000));
            database = catalog.databasePath();
        }
        QVERIFY(QDir(QFileInfo(database).absolutePath()).removeRecursively());
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        QVERIFY(catalog.reader().images().isEmpty());
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QCOMPARE(statuses(catalog), kExpected);
    }

    // Only files whose size or time changed are read again; a change of another program is
    // found by the next scan; images that are gone are missing after a complete scan.
    void changesFoundByTheNextScan()
    {
        Setup setup;
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));

        QSignalSpy progress(&catalog, &ProjectCatalog::checkProgress);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(1).toInt(), 0);  // nothing to read again

        writeFile(setup.path("line/2.json"), annotation("2.png", R"(, "checked": false)"));
        QVERIFY(QFile::remove(setup.path("line/1.png")));
        progress.clear();
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QCOMPARE(progress.last().at(1).toInt(), 1);
        QMap<QString, QString> expected = kExpected;
        expected.remove(QStringLiteral("line/1.png"));
        expected[QStringLiteral("line/2.png")] = QStringLiteral("pending-confirmation");
        QCOMPARE(statuses(catalog), expected);
        QVERIFY(catalog.reader().image(QStringLiteral("line/1.png"))->missing);
    }

    // A save of this program counts without reading the file again (format spec 5.4).
    void savesUpdateTheCatalog()
    {
        Setup setup;
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));

        writeFile(setup.path("line/1.json"), annotation("1.png", R"(, "checked": true)"));
        catalog.recordSaved(QStringLiteral("line/1.png"), AnnotationStatus::Confirmed,
                            formats::readFileStamp(setup.path("line/1.json")), false);
        QVERIFY(catalog.waitUntilIdle(10000));
        dataset::CatalogEntry entry = *catalog.reader().image(QStringLiteral("line/1.png"));
        QCOMPARE(entry.status, AnnotationStatus::Confirmed);
        QCOMPARE(entry.annotationName, QStringLiteral("1.json"));

        // The next scan lists the same file: nothing to read again, still confirmed.
        QSignalSpy progress(&catalog, &ProjectCatalog::checkProgress);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QCOMPARE(progress.last().at(1).toInt(), 0);
        entry = *catalog.reader().image(QStringLiteral("line/1.png"));
        QCOMPARE(entry.status, AnnotationStatus::Confirmed);
        QVERIFY(entry.verified);
    }

    // Architecture 6.3: a save acknowledged while the folder was being listed is newer than
    // the listing; the listing must not take its annotation file away again.
    void saveDuringTheListingIsKept()
    {
        Setup setup;
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        catalog.setAfterListing([&] {
            writeFile(setup.path("line/1.json"), annotation("1.png", R"(, "checked": true)"));
            catalog.recordSaved(QStringLiteral("line/1.png"), AnnotationStatus::Confirmed,
                                formats::readFileStamp(setup.path("line/1.json")), false);
        });
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        const dataset::CatalogEntry entry = *catalog.reader().image(QStringLiteral("line/1.png"));
        QCOMPARE(entry.annotationName, QStringLiteral("1.json"));
        QCOMPARE(entry.status, AnnotationStatus::Confirmed);
        QVERIFY(entry.verified);

        // The next listing sees the file and agrees.
        catalog.setAfterListing({});
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QCOMPARE(statuses(catalog).value(QStringLiteral("line/1.png")), QStringLiteral("confirmed"));
    }

    // A status holds for the project's rules it was derived under (format spec 5.1): new
    // rules mean reading the files again.
    void changedRulesAreCheckedAgain()
    {
        Setup setup;
        {
            ProjectCatalog catalog(*setup.project, fakeImageInfo);
            catalog.start();
            QVERIFY(catalog.waitUntilIdle(10000));
        }
        writeFile(setup.path("liteanno/project.json"),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", )"
                  R"("shapeTypes": ["rectangle"], "classes": [{"id": 0, "name": "scratch"}], )"
                  R"("attributes": {"scratch": [{"name": "severity", "required": true}]}})");
        setup.reopen();
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        QVERIFY(!catalog.reader().image(QStringLiteral("line/2.png"))->verified);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QCOMPARE(statuses(catalog).value(QStringLiteral("line/2.png")), QStringLiteral("confirmed-with-problems"));
    }

    // A name collision that comes or goes changes the status without either file changing.
    void nameCollisionsAreCheckedAgain()
    {
        Setup setup;
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        writeFile(setup.path("line/1.jpg"), "image");
        QVERIFY(QFile::remove(setup.path("line/5.jpg")));
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QMap<QString, QString> expected = kExpected;
        expected.remove(QStringLiteral("line/5.jpg"));
        expected[QStringLiteral("line/1.jpg")] = QStringLiteral("name-collision");
        expected[QStringLiteral("line/1.png")] = QStringLiteral("name-collision");
        expected[QStringLiteral("line/5.png")] = QStringLiteral("unprocessed");
        QCOMPARE(statuses(catalog), expected);
    }

    // A folder that cannot be listed is not evidence that its images are gone.
    void unreadableFolderIsNotMissing()
    {
        Setup setup;
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        const qint64 complete = catalog.reader().completeGeneration();
        test::FolderBlocker blocker(setup.path("line"));
        if (!blocker.isActive())
            QSKIP("cannot take the right to list a folder away here (running as root?)");
        QSignalSpy finished(&catalog, &ProjectCatalog::scanFinished);
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        blocker.release();
        QCOMPARE(catalog.reader().images().size(), kExpected.size());
        QCOMPARE(catalog.reader().completeGeneration(), complete);
        QCOMPARE(catalog.reader().reports().unreadable, QStringList{QStringLiteral("line")});
        QVERIFY(finished.size() == 1 || finished.wait(1000));
        QCOMPARE(finished.first().first().toBool(), false);
    }

    // The catalog is only a cache: when its folder cannot be created the project still opens
    // and is scanned, with the catalog in a temporary place.
    void blockedCacheFolderStillScans()
    {
        Setup setup;
        writeFile(setup.local.filePath(QStringLiteral("cache")), "not a folder");
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        QVERIFY2(catalog.isOpen(), qPrintable(catalog.error()));
        QVERIFY(catalog.isTemporary());
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QCOMPARE(statuses(catalog), kExpected);
    }

    void cancelling()
    {
        Setup setup;
        ProjectCatalog catalog(*setup.project, fakeImageInfo);
        catalog.start();
        catalog.cancel();
        QVERIFY(!catalog.isBusy());
        catalog.start();
        QVERIFY(catalog.waitUntilIdle(10000));
        QCOMPARE(statuses(catalog), kExpected);
    }
};

QTEST_GUILESS_MAIN(TestProjectCatalog)
#include "tst_projectcatalog.moc"
