// The local catalog (architecture 6.2, 6.3): what scans write, when images count as missing,
// which status observations win, and that the catalog is rebuilt rather than trusted when
// it does not fit.

#include "dataset/CatalogReader.h"
#include "dataset/CatalogWriter.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace liteanno;
using namespace liteanno::dataset;

namespace {

const CatalogIdentity kIdentity{QStringLiteral("3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"), QStringLiteral("binding-a"),
                                QStringLiteral("rules-a")};

ScannedImage scanned(const QString& path, qint64 annotationSize = -1)
{
    ScannedImage image;
    image.path = path;
    image.image = {1000, 1700000000000};
    if (annotationSize >= 0) {
        const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
        image.annotationName = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1).left(dot - path.lastIndexOf(QLatin1Char('/')) - 1)
                               + QLatin1String(".json");
        image.annotation = {annotationSize, 1700000000500};
    }
    return image;
}

void fullScan(CatalogWriter& writer, const QList<ScannedImage>& images)
{
    const qint64 generation = writer.beginScan();
    writer.addScanned(generation, images, 0, writer.nextSequence());
    writer.finishScan(generation, true, {});
    writer.flush();
}

QStringList paths(const QList<CatalogEntry>& entries)
{
    QStringList list;
    for (const CatalogEntry& entry : entries)
        list.append(entry.path);
    return list;
}

StatusObservation observation(const QString& path, qint64 sequence, AnnotationStatus status, qint64 annotationSize)
{
    StatusObservation o;
    o.path = path;
    o.sequence = sequence;
    o.status = status;
    o.image = {1000, 1700000000000};
    o.annotation = annotationSize >= 0 ? FileVersion{annotationSize, 1700000000500} : FileVersion{};
    return o;
}

} // namespace

class TestCatalog : public QObject
{
    Q_OBJECT

private slots:
    void scanThenRead()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("cache/catalog.sqlite"));
        CatalogWriter writer(db, kIdentity);
        QVERIFY2(writer.isOpen(), qPrintable(writer.error()));
        QVERIFY(writer.startedEmpty());
        const qint64 generation = writer.beginScan();
        QCOMPARE(generation, qint64(1));
        writer.addScanned(generation, {scanned(QStringLiteral("a/1.png")), scanned(QStringLiteral("a/2.png"), 120)}, 0,
                          writer.nextSequence());
        writer.addScanned(generation, {scanned(QStringLiteral(u"中文/10.png"))}, 2, writer.nextSequence());
        writer.finishScan(generation, true,
                          {{QStringLiteral("x.heic")}, {QStringLiteral("lonely.json")}, {QStringLiteral("long.png")}, {}});
        writer.flush();

        const CatalogReader reader(db);
        QVERIFY(reader.isOpen());
        QCOMPARE(paths(reader.images()),
                 QStringList({QStringLiteral("a/1.png"), QStringLiteral("a/2.png"), QStringLiteral(u"中文/10.png")}));
        const CatalogEntry second = *reader.image(QStringLiteral("a/2.png"));
        QCOMPARE(second.annotationName, QStringLiteral("2.json"));
        QCOMPARE(second.annotation.size, qint64(120));
        QVERIFY(!second.status);
        QVERIFY(!second.verified);
        QCOMPARE(reader.needingCheckCount(), 3);
        QCOMPARE(reader.completeGeneration(), qint64(1));
        QCOMPARE(reader.reports().unsupported, QStringList{QStringLiteral("x.heic")});
        QCOMPARE(reader.reports().orphanAnnotations, QStringList{QStringLiteral("lonely.json")});
        QCOMPARE(reader.reports().longPaths, QStringList{QStringLiteral("long.png")});
    }

    // SQLite takes UTF-8 file names; Windows code pages cannot spell every folder name.
    void chineseCachePath()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral(u"本机缓存/目录索引.sqlite"));
        {
            CatalogWriter writer(db, kIdentity);
            QVERIFY2(writer.isOpen(), qPrintable(writer.error()));
            fullScan(writer, {scanned(QStringLiteral(u"产线/1.png"))});
        }
        QVERIFY(QFile::exists(db));
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral(u"本机缓存")});
        const CatalogReader reader(db);
        QVERIFY(reader.isOpen());
        QCOMPARE(paths(reader.images()), QStringList{QStringLiteral(u"产线/1.png")});
    }

    // Architecture 6.2: only a complete scan marks what it did not see.
    void missingOnlyAfterACompleteScan()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("catalog.sqlite"));
        CatalogWriter writer(db, kIdentity);
        fullScan(writer, {scanned(QStringLiteral("1.png")), scanned(QStringLiteral("2.png"))});

        qint64 generation = writer.beginScan();
        writer.addScanned(generation, {scanned(QStringLiteral("1.png"))}, 0, writer.nextSequence());
        writer.finishScan(generation, false, {});
        writer.flush();
        const CatalogReader reader(db);
        QCOMPARE(paths(reader.images()), QStringList({QStringLiteral("1.png"), QStringLiteral("2.png")}));
        QCOMPARE(reader.completeGeneration(), qint64(1));

        generation = writer.beginScan();
        writer.addScanned(generation, {scanned(QStringLiteral("1.png"))}, 0, writer.nextSequence());
        writer.finishScan(generation, true, {});
        writer.flush();
        QCOMPARE(paths(reader.images()), QStringList{QStringLiteral("1.png")});
        const QList<CatalogEntry> all = reader.images(true);
        QCOMPARE(all.size(), 2);
        QVERIFY(all[1].missing);
        QCOMPARE(reader.completeGeneration(), generation);

        // Found again: present again.
        fullScan(writer, {scanned(QStringLiteral("1.png")), scanned(QStringLiteral("2.png"))});
        QCOMPARE(reader.images().size(), 2);
    }

    // Architecture 6.3: a check that read the file before the last save does not replace
    // what the save acknowledged.
    void olderObservationsAreDropped()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("catalog.sqlite"));
        CatalogWriter writer(db, kIdentity);
        fullScan(writer, {scanned(QStringLiteral("1.png"), 100)});

        const qint64 checkStarted = writer.nextSequence();
        StatusObservation saved = observation(QStringLiteral("1.png"), writer.nextSequence(), AnnotationStatus::Confirmed, 140);
        saved.written = true;
        saved.annotationName = QStringLiteral("1.json");
        writer.record(saved);
        writer.record(observation(QStringLiteral("1.png"), checkStarted, AnnotationStatus::PendingConfirmation, 100));
        writer.flush();

        const CatalogReader reader(db);
        CatalogEntry entry = *reader.image(QStringLiteral("1.png"));
        QCOMPARE(entry.status, AnnotationStatus::Confirmed);
        QCOMPARE(entry.annotation.size, qint64(140));
        QVERIFY(entry.verified);
        QCOMPARE(reader.needingCheckCount(), 0);
        QCOMPARE(reader.statusCounts().value(AnnotationStatus::Confirmed), 1);

        // Another program changes the file: the next scan lists another version, and the
        // status no longer holds for it.
        QList<ScannedImage> changed = {scanned(QStringLiteral("1.png"), 150)};
        fullScan(writer, changed);
        entry = *reader.image(QStringLiteral("1.png"));
        QCOMPARE(entry.status, AnnotationStatus::Confirmed);
        QVERIFY(!entry.verified);
        QCOMPARE(reader.needingCheckCount(), 1);
        QVERIFY(reader.statusCounts().value(AnnotationStatus::Confirmed) == 1);

        writer.record(observation(QStringLiteral("1.png"), writer.nextSequence(), AnnotationStatus::PendingConfirmation, 150));
        writer.flush();
        entry = *reader.image(QStringLiteral("1.png"));
        QCOMPARE(entry.status, AnnotationStatus::PendingConfirmation);
        QVERIFY(entry.verified);
    }

    void sequencesGoOnAfterReopening()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("catalog.sqlite"));
        qint64 used = 0;
        {
            CatalogWriter writer(db, kIdentity);
            fullScan(writer, {scanned(QStringLiteral("1.png"))});
            for (int i = 0; i < 5; ++i)
                used = writer.nextSequence();
            writer.record(observation(QStringLiteral("1.png"), used, AnnotationStatus::Unprocessed, -1));
        }
        CatalogWriter writer(db, kIdentity);
        QVERIFY(!writer.startedEmpty());
        QVERIFY(writer.nextSequence() > used);
        QCOMPARE(writer.beginScan(), qint64(2));
    }

    // The catalog is a cache (format spec 8): one that does not fit is started over.
    void rebuiltWhenItDoesNotFit_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("other project") << QStringLiteral("project");
        QTest::newRow("other location") << QStringLiteral("binding");
        QTest::newRow("not a database") << QStringLiteral("garbage");
    }

    void rebuiltWhenItDoesNotFit()
    {
        QFETCH(QString, kind);
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("catalog.sqlite"));
        if (kind == QLatin1String("garbage")) {
            QFile file(db);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(4096, 'x'));
        } else {
            CatalogWriter writer(db, kIdentity);
            fullScan(writer, {scanned(QStringLiteral("1.png"))});
        }
        CatalogIdentity other = kIdentity;
        if (kind == QLatin1String("project"))
            other.projectId = QStringLiteral("00000000-0000-4000-8000-000000000000");
        else if (kind == QLatin1String("binding"))
            other.bindingId = QStringLiteral("binding-b");
        CatalogWriter writer(db, other);
        QVERIFY2(writer.isOpen(), qPrintable(writer.error()));
        QVERIFY(writer.startedEmpty());
        QVERIFY(CatalogReader(db).images(true).isEmpty());
        fullScan(writer, {scanned(QStringLiteral("2.png"))});
        QCOMPARE(paths(CatalogReader(db).images()), QStringList{QStringLiteral("2.png")});
    }
};

QTEST_MAIN(TestCatalog)
#include "tst_catalog.moc"
