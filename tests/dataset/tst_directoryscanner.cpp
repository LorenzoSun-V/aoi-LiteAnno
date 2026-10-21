// Listing the images of a data root (format spec 2, 9.1) and their order.

#include "common/FolderBlocker.h"
#include "dataset/DirectoryScanner.h"
#include "dataset/NaturalSort.h"
#include "formats/AnnotationStore.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace liteanno::dataset;

namespace {

void touch(const QDir& root, const QString& path)
{
    root.mkpath(QFileInfo(root.filePath(path)).path());
    QFile file(root.filePath(path));
    if (!file.open(QIODevice::WriteOnly))
        qFatal("cannot create %s", qPrintable(path));
}

QStringList paths(const ScanResult& result)
{
    QStringList list;
    for (const ScannedImage& image : result.images)
        list.append(image.path);
    return list;
}

} // namespace

class TestDirectoryScanner : public QObject
{
    Q_OBJECT

private slots:
    void naturalOrder()
    {
        QStringList names = {QStringLiteral("img10.jpg"), QStringLiteral("img2.jpg"), QStringLiteral("IMG1.jpg"),
                             QStringLiteral("a/b.jpg"), QStringLiteral("a-b.jpg"), QStringLiteral("img02.jpg")};
        std::sort(names.begin(), names.end(), naturalLess);
        const QStringList expected = {QStringLiteral("a/b.jpg"), QStringLiteral("a-b.jpg"), QStringLiteral("IMG1.jpg"),
                                      QStringLiteral("img02.jpg"), QStringLiteral("img2.jpg"), QStringLiteral("img10.jpg")};
        QCOMPARE(names, expected);
    }

    void scan()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("line1/10.bmp"));
        touch(root, QStringLiteral("line1/9.BMP"));
        touch(root, QStringLiteral("line1/9.json"));
        touch(root, QStringLiteral("line1/notes.txt"));
        touch(root, QStringLiteral("line2/a.jpg"));
        touch(root, QStringLiteral("line2/a.png"));
        touch(root, QStringLiteral("line2/raw.dng"));
        touch(root, QStringLiteral("liteanno/x.png"));
        touch(root, QStringLiteral("sub/liteanno/kept.png"));
        touch(root, QStringLiteral("x-anylabeling-exif-backup/old.jpg"));
        touch(root, QStringLiteral(".hidden/h.jpg"));
        touch(root, QStringLiteral("line1/.xal_1.tmp.jpg"));

        const ScanResult result = scanDirectory(dir.path());
        const QStringList expected = {QStringLiteral("line1/9.BMP"), QStringLiteral("line1/10.bmp"),
                                      QStringLiteral("line2/a.jpg"), QStringLiteral("line2/a.png"),
                                      QStringLiteral("sub/liteanno/kept.png")};
        QCOMPARE(paths(result), expected);
        QVERIFY(!result.images[0].nameCollision);
        // line2/a.jpg and line2/a.png share a.json.
        QVERIFY(result.images[2].nameCollision);
        QVERIFY(result.images[3].nameCollision);
        QCOMPARE(result.unsupported, QStringList{QStringLiteral("line2/raw.dng")});
    }

    // Architecture 6.1: the same listing pairs the annotation files (format spec 2) and
    // gives the sizes and times of both files.
    void annotationsFromTheSameListing()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("d/a.png"));
        touch(root, QStringLiteral("d/a.JSON"));
        touch(root, QStringLiteral("d/b.c.jpg"));
        touch(root, QStringLiteral("d/b.c.json"));
        touch(root, QStringLiteral("d/b.json"));  // stem "b" has no image
        touch(root, QStringLiteral("d/c.png"));
        touch(root, QStringLiteral("e/c.json"));  // another directory
        {
            QFile file(root.filePath(QStringLiteral("d/b.c.json")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{}");
        }
        const ScanResult result = scanDirectory(dir.path());
        QVERIFY(result.complete);
        QCOMPARE(paths(result), QStringList({QStringLiteral("d/a.png"), QStringLiteral("d/b.c.jpg"), QStringLiteral("d/c.png")}));
        QCOMPARE(result.images[0].annotationName, QStringLiteral("a.JSON"));
        QCOMPARE(result.images[1].annotationName, QStringLiteral("b.c.json"));
        QCOMPARE(result.images[1].annotation.size, qint64(2));
        QCOMPARE(result.images[1].annotation.modifiedMs,
                 QFileInfo(root.filePath(QStringLiteral("d/b.c.json"))).lastModified().toMSecsSinceEpoch());
        QCOMPARE(result.images[1].image.size, qint64(0));
        QVERIFY(result.images[2].annotationName.isEmpty());
        QVERIFY(!result.images[2].annotation.exists());
        QCOMPARE(result.orphanAnnotations, QStringList({QStringLiteral("d/b.json"), QStringLiteral("e/c.json")}));
    }

    // Format spec 2: the stem follows the file system, the extension never matters.
    void stemCaseFollowsTheFileSystem()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("a.png"));
        touch(root, QStringLiteral("A.json"));
        const ScanResult result = scanDirectory(dir.path());
        QCOMPARE(result.images.size(), 1);
#ifdef Q_OS_WIN
        QCOMPARE(result.images[0].annotationName, QStringLiteral("A.json"));
        QVERIFY(result.orphanAnnotations.isEmpty());
#else
        QVERIFY(result.images[0].annotationName.isEmpty());
        QCOMPARE(result.orphanAnnotations, QStringList{QStringLiteral("A.json")});
#endif
    }

    void cancelledScanIsIncomplete()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        for (int i = 0; i < 5; ++i)
            touch(root, QStringLiteral("d%1/a.png").arg(i));
        int directories = 0;
        QList<int> progress;
        ScanOptions options;
        options.cancelled = [&directories] { return ++directories > 3; };
        options.progress = [&progress](int found) { progress.append(found); };
        const ScanResult result = scanDirectory(dir.path(), options);
        QVERIFY(!result.complete);
        QVERIFY(result.images.size() < 5);
        QVERIFY(!progress.isEmpty());
        QVERIFY(scanDirectory(dir.path()).complete);
    }

    // The scan pairs the file the editor opens and saves (formats::existingAnnotationPath),
    // also where several files differ only in the case of the extension.
    void pairingMatchesTheEditor()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        for (const char* name : {"a.png", "a.json", "a.JSON", "b.png", "b.Json", "b.JSON", "c.png", "c.JSON"})
            touch(root, QLatin1String(name));
        if (QDir(dir.path()).entryList(QDir::Files).size() < 8)
            QSKIP("this file system ignores case");
        const ScanResult result = scanDirectory(dir.path());
        QCOMPARE(result.images.size(), 3);
        for (const ScannedImage& image : result.images) {
            const QString stem = image.path.left(image.path.lastIndexOf(QLatin1Char('.')));
            const QString editor = liteanno::formats::existingAnnotationPath(root.filePath(stem + QLatin1String(".json")));
            QCOMPARE(image.annotationName, QFileInfo(editor).fileName());
        }
    }

    // A folder that cannot be listed says nothing about its images: the scan is incomplete
    // and names the folder.
    void unreadableFolderMakesTheScanIncomplete()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("ok/a.png"));
        touch(root, QStringLiteral("locked/b.png"));
        liteanno::test::FolderBlocker blocker(root.filePath(QStringLiteral("locked")));
        if (!blocker.isActive())
            QSKIP("cannot take the right to list a folder away here (running as root?)");
        const ScanResult result = scanDirectory(dir.path());
        blocker.release();
        QVERIFY(!result.complete);
        QVERIFY(!result.cancelled);
        QCOMPARE(paths(result), QStringList{QStringLiteral("ok/a.png")});
        QCOMPARE(result.unreadable, QStringList{QStringLiteral("locked")});
        QVERIFY(scanDirectory(dir.path()).complete);
    }

    // Format spec 2: the annotation file's path counts too; ".json" is longer than ".png".
    void longAnnotationPaths()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        const int rootLength = int(QDir::toNativeSeparators(root.absolutePath()).size());
        // Image 259 characters, its annotation file 260.
        const QString stem = QString(259 - rootLength - 1 - 4, QLatin1Char('a'));
        if (stem.size() + 5 > 255)
            QSKIP("the temporary folder is too short for a name of 255 characters at most");
        touch(root, stem + QLatin1String(".png"));
        ScanResult result = scanDirectory(dir.path());
        QCOMPARE(result.longPaths, QStringList{stem + QLatin1String(".png")});
        touch(root, stem + QLatin1String(".json"));
        result = scanDirectory(dir.path());
        QCOMPARE(result.longPaths, QStringList{stem + QLatin1String(".png")});
    }

    // Format spec 2: absolute paths over 259 characters are reported.
    void longPaths()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        const int rootLength = int(QDir::toNativeSeparators(root.absolutePath()).size());
        // The annotation file to be written ("<stem>.json") has 259 characters.
        const QString fits = QString(259 - rootLength - 1 - 5, QLatin1Char('a')) + QLatin1String(".png");
        const QString over = QString(260 - rootLength - 1 - 4, QLatin1Char('b')) + QLatin1String(".png");
        if (over.size() > 255)
            QSKIP("the temporary folder is too short for a name of 255 characters at most");
        touch(root, fits);
        touch(root, over);
        const ScanResult result = scanDirectory(dir.path());
        QCOMPARE(result.images.size(), 2);
        QCOMPARE(result.longPaths, QStringList{over});
    }

    // Format spec 3, 11: names are kept as they are on disk; Chinese names scan like others,
    // and names that differ only in Unicode normalization collide.
    void unicodeNames()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        const QString chinese = QStringLiteral(u"产线一/划痕_001.png");
        touch(root, chinese);
        const QString nfc = QStringLiteral(u"l\u00e9.png");   // é as one code point
        const QString nfd = QStringLiteral(u"le\u0301.png");  // e + combining acute
        touch(root, QStringLiteral(u"u/") + nfc);
        touch(root, QStringLiteral(u"u/") + nfd);
        touch(root, QStringLiteral(u"u/plain.png"));

        const ScanResult result = scanDirectory(dir.path());
        QStringList found = paths(result);
        QVERIFY2(found.contains(chinese), qPrintable(found.join(QLatin1Char(' '))));
        const QStringList u = found.filter(QStringLiteral("u/"));
        // Some file systems store one normalization form only; then both names are one file.
        if (QDir(root.filePath(QStringLiteral("u"))).entryList(QDir::Files).size() < 3)
            QSKIP("this file system normalizes names");
        QCOMPARE(u.size(), 3);
        for (const ScannedImage& image : result.images) {
            const bool accented = image.path != QStringLiteral("u/plain.png") && image.path.startsWith(QLatin1String("u/"));
            QCOMPARE(image.nameCollision, accented);
            // The characters on disk, not a normalized form.
            if (accented)
                QVERIFY(image.path == QStringLiteral(u"u/") + nfc || image.path == QStringLiteral(u"u/") + nfd);
        }
    }

    // Windows attributes: system alone, hidden alone, on files and on directories.
    void windowsAttributes()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows file attributes");
#else
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("kept.png"));
        touch(root, QStringLiteral("system.png"));
        touch(root, QStringLiteral("hidden.png"));
        touch(root, QStringLiteral("systemdir/a.png"));
        touch(root, QStringLiteral("hiddendir/b.png"));
        const auto set = [&root](const QString& name, DWORD attribute) {
            const QString path = QDir::toNativeSeparators(root.filePath(name));
            QVERIFY(SetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()), attribute));
        };
        set(QStringLiteral("system.png"), FILE_ATTRIBUTE_SYSTEM);
        set(QStringLiteral("hidden.png"), FILE_ATTRIBUTE_HIDDEN);
        set(QStringLiteral("systemdir"), FILE_ATTRIBUTE_SYSTEM);
        set(QStringLiteral("hiddendir"), FILE_ATTRIBUTE_HIDDEN);

        const ScanResult result = scanDirectory(dir.path());
        QCOMPARE(paths(result), QStringList{QStringLiteral("kept.png")});

        // Removable again by QTemporaryDir.
        set(QStringLiteral("system.png"), FILE_ATTRIBUTE_NORMAL);
        set(QStringLiteral("hidden.png"), FILE_ATTRIBUTE_NORMAL);
        set(QStringLiteral("systemdir"), FILE_ATTRIBUTE_NORMAL);
        set(QStringLiteral("hiddendir"), FILE_ATTRIBUTE_NORMAL);
#endif
    }
};

QTEST_GUILESS_MAIN(TestDirectoryScanner)
#include "tst_directoryscanner.moc"
