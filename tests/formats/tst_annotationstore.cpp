// Loading and saving files on disk: "unedited files are not written" (format spec section 1)
// and read-only files are never touched.

#include "common/TestData.h"
#include "formats/AnnotationStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace liteanno;
using namespace liteanno::formats;

namespace {

const QDateTime kOldTime(QDate(2020, 1, 2), QTime(3, 4, 5), QTimeZone::utc());

struct FileState {
    QByteArray bytes;
    QDateTime modified;
};

FileState fileState(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return {file.readAll(), QFileInfo(path).lastModified()};
}

// Copies a fixture into the temporary directory and gives it an old modification time, so
// any write is visible in both the content and the time stamp.
QString placeFixture(const QTemporaryDir& dir, const char* fixture, const char* targetName)
{
    const QString target = dir.filePath(QLatin1String(targetName));
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly))
        qFatal("cannot create %s", qPrintable(target));
    file.write(test::readTestData(QStringLiteral("annotations/") + QLatin1String(fixture)));
    file.close();
    if (!file.open(QIODevice::ReadWrite) || !file.setFileTime(kOldTime, QFileDevice::FileModificationTime))
        qFatal("cannot set the modification time of %s", qPrintable(target));
    return target;
}

} // namespace

class TestAnnotationStore : public QObject
{
    Q_OBJECT

private slots:
    void uneditedFilesAreNotWritten_data()
    {
        QTest::addColumn<QByteArray>("fixture");
        // Files a save would normalize (two-point rectangle, missing keys, BOM and CRLF) are
        // still left alone when nothing was edited.
        QTest::newRow("upstream 2.x") << QByteArray("upstream_v2_legacy.json");
        QTest::newRow("missing fields") << QByteArray("missing_fields.json");
        QTest::newRow("BOM and CRLF") << QByteArray("bom_crlf.json");
        QTest::newRow("unknown fields") << QByteArray("unknown_fields.json");
    }

    void uneditedFilesAreNotWritten()
    {
        QFETCH(QByteArray, fixture);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, fixture.constData(), "image.json");
        const FileState before = fileState(path);

        AnnotationFile file = loadAnnotationFile(path, ImageInfo{QStringLiteral("image.png"), 0, 0, 1});
        QVERIFY(!file.isReadOnly());
        const SaveResult result = saveAnnotationFile(path, file);
        QCOMPARE(result.status, SaveStatus::NotModified);

        const FileState after = fileState(path);
        QCOMPARE(after.bytes, before.bytes);
        QCOMPARE(after.modified, before.modified);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("image.json")});
    }

    void newDocumentsAreWrittenOnlyAfterAnEdit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("0001.json"));
        const ImageInfo image{QStringLiteral("0001.bmp"), 768, 768, 1};

        AnnotationFile file = loadAnnotationFile(path, image, {QStringLiteral("OK"), QStringLiteral("NG")});
        QVERIFY(file.isNew());
        QCOMPARE(saveAnnotationFile(path, file).status, SaveStatus::NotModified);
        QVERIFY(!QFile::exists(path));

        Shape rectangle;
        rectangle.type = ShapeType::Rectangle;
        rectangle.label = QStringLiteral("scratch");
        rectangle.points = {QPointF(120.5, 80.0), QPointF(260.0, 150.25)};
        QVERIFY(file.document().addShape(rectangle) >= 0);
        QVERIFY(file.document().setImageLabel(QStringLiteral("NG"), true));
        const SaveResult result = saveAnnotationFile(path, file);
        QCOMPARE(result.status, SaveStatus::Saved);
        QCOMPARE(fileState(path).bytes, test::readTestData(QStringLiteral("annotations/golden_new_file.json")));

        // Saved content is the new base: saving again writes nothing.
        QVERIFY(!file.needsWrite());
        QCOMPARE(saveAnnotationFile(path, file).status, SaveStatus::NotModified);
    }

    void editedFilesAreWritten()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        const ImageInfo image{QStringLiteral("0002.jpg"), 640, 480, 1};

        AnnotationFile file = loadAnnotationFile(path, image);
        QVERIFY(file.document().confirm());
        QCOMPARE(saveAnnotationFile(path, file).status, SaveStatus::Saved);

        const FileState after = fileState(path);
        QVERIFY(after.modified != kOldTime);
        QCOMPARE(after.bytes, file.toBytes());
        AnnotationFile reread = loadAnnotationFile(path, image);
        QVERIFY(reread.document().review().checked);
        QVERIFY(reread.diagnostics().isEmpty());
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("0002.json")});
    }

    void readOnlyFilesAreNeverWritten_data()
    {
        QTest::addColumn<QByteArray>("fixture");
        QTest::addColumn<int>("exifOrientation");
        QTest::addColumn<int>("expectedReason");
        QTest::newRow("JSON error") << QByteArray("broken/nan_coordinate.json") << 1
                                    << int(ReadOnlyReason::JsonError);
        QTest::newRow("future version") << QByteArray("future_version.json") << 1
                                        << int(ReadOnlyReason::UnsupportedFormatVersion);
        QTest::newRow("EXIF orientation") << QByteArray("upstream_v4.json") << 6
                                          << int(ReadOnlyReason::ExifOrientationUnconfirmed);
    }

    void readOnlyFilesAreNeverWritten()
    {
        QFETCH(QByteArray, fixture);
        QFETCH(int, exifOrientation);
        QFETCH(int, expectedReason);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, fixture.constData(), "image.json");
        const FileState before = fileState(path);

        AnnotationFile file = loadAnnotationFile(path, ImageInfo{QStringLiteral("image.jpg"), 0, 0, exifOrientation});
        QCOMPARE(int(file.readOnlyReason()), expectedReason);
        file.document().confirm();
        QCOMPARE(saveAnnotationFile(path, file).status, SaveStatus::ReadOnly);

        const FileState after = fileState(path);
        QCOMPARE(after.bytes, before.bytes);
        QCOMPARE(after.modified, before.modified);
    }

    void unreadableFileIsNotTreatedAsNew()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A directory where the annotation file should be cannot be opened as a file.
        const QString path = dir.filePath(QStringLiteral("image.json"));
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("image.json")));

        AnnotationFile file = loadAnnotationFile(path, ImageInfo{QStringLiteral("image.png"), 1, 1, 1});
        QCOMPARE(file.readOnlyReason(), ReadOnlyReason::Unreadable);
        QVERIFY(!file.isNew());
        QCOMPARE(saveAnnotationFile(path, file).status, SaveStatus::ReadOnly);
    }
};

QTEST_GUILESS_MAIN(TestAnnotationStore)
#include "tst_annotationstore.moc"
