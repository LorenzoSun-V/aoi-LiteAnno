// Import (architecture 11.2): the three modes, nothing written while a label file cannot be
// imported unless those images are skipped, images with unwritten edits left alone, COCO
// records matched to images, and masks. The shapes themselves are compared with upstream's
// import in python/import_compat.py.

#include "application/ImportService.h"
#include "application/LocalDataPaths.h"
#include "application/ProjectSession.h"
#include "application/RecoveryService.h"
#include "formats/AnnotationStore.h"
#include "formats/Importers.h"
#include "formats/MaskContours.h"
#include "formats/PoseConfig.h"
#include "formats/RecoveryRecord.h"
#include "imaging/ImageLoader.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

struct Project {
    QTemporaryDir local;
    QTemporaryDir data;
    QTemporaryDir labels;

    Project()
    {
        writeFile(data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", "shapeTypes": ["rectangle", "polygon"], )"
                  R"("classes": [{"id": 0, "name": "scratch"}, {"id": 1, "name": "dent"}]})");
    }

    void image(const QString& path, int width = 200, int height = 100)
    {
        QImage image(width, height, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        const QString file = data.filePath(path);
        QDir().mkpath(QFileInfo(file).absolutePath());
        QVERIFY(image.save(file));
    }

    // An annotation file with one rectangle of `label`.
    void annotation(const QString& imagePath, const char* label)
    {
        const QString name = QFileInfo(imagePath).fileName();
        writeFile(data.filePath(imagePath.left(imagePath.lastIndexOf(QLatin1Char('.'))) + QStringLiteral(".json")),
                  QByteArray(R"({"version": "4.2.0", "flags": {}, "shapes": [{"label": ")") + label
                      + R"(", "points": [[1.0, 1.0], [9.0, 1.0], [9.0, 9.0], [1.0, 9.0]], "group_id": null, "description": "", )"
                        R"("difficult": false, "shape_type": "rectangle", "flags": {}}], "imagePath": ")"
                      + name.toUtf8() + R"(", "imageData": null, "imageHeight": 100, "imageWidth": 200, "checked": true})");
    }

    std::unique_ptr<ProjectSession> open()
    {
        ProjectSession::OpenResult result = ProjectSession::open(data.path(), LocalDataPaths(local.path()));
        if (!result.session)
            qFatal("cannot open the project: %s", qPrintable(result.message));
        return std::move(result.session);
    }

    ImportReport run(ImportRequest request, const ImportService::Progress& progress = {})
    {
        const std::unique_ptr<ProjectSession> session = open();
        if (request.labelDir.isEmpty() && request.labelFile.isEmpty())
            request.labelDir = labels.path();
        return ImportService(*session, imaging::readImageInfo, imaging::readMaskImage).run(request, progress);
    }

    AnnotationDocument document(const QString& imagePath)
    {
        const QString json = data.filePath(imagePath.left(imagePath.lastIndexOf(QLatin1Char('.'))) + QStringLiteral(".json"));
        ImageInfo info;
        info.width = 200;
        info.height = 100;
        info.fileName = QFileInfo(imagePath).fileName();
        return formats::loadAnnotationFile(json, info, {}).file.document();
    }

    QList<Shape> shapes(const QString& imagePath) { return document(imagePath).shapes(); }
    bool checked(const QString& imagePath) { return document(imagePath).review().checked; }
};

const ImportItem* itemOf(const ImportReport& report, const QString& image)
{
    for (const ImportItem& item : report.items) {
        if (item.image == image)
            return &item;
    }
    return nullptr;
}

#ifdef Q_OS_WIN
// Sets an empty folder to tell case apart in names (what fsutil file setCaseSensitiveInfo
// does). False where Windows or the file system cannot.
bool makeCaseSensitive(const QString& folder)
{
    const QString native = QDir::toNativeSeparators(folder);
    const HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    struct {
        ULONG flags;
    } info{0x1};  // FILE_CASE_SENSITIVE_INFO, FILE_CS_FLAG_CASE_SENSITIVE_DIR
    const bool set = SetFileInformationByHandle(handle, static_cast<FILE_INFO_BY_HANDLE_CLASS>(23), &info, sizeof(info));
    CloseHandle(handle);
    if (!set)
        return false;
    // Set and in effect: a name in other case is another file.
    writeFile(folder + QStringLiteral("/probe.txt"), "x");
    const bool sensitive = !QFileInfo::exists(folder + QStringLiteral("/PROBE.TXT"));
    QFile::remove(folder + QStringLiteral("/probe.txt"));
    return sensitive;
}
#endif

QByteArray coco(const QByteArray& images, const QByteArray& annotations)
{
    return R"({"images": [)" + images + R"(], "annotations": [)" + annotations
           + R"(], "categories": [{"id": 1, "name": "scratch"}, {"id": 2, "name": "dent"}]})";
}

} // namespace

class TestImport : public QObject
{
    Q_OBJECT

private slots:
    // OnlyNew leaves annotated images alone; Append adds after their shapes; Replace puts the
    // imported ones in their place. Changed images wait for confirmation.
    void modes()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.image(QStringLiteral("b.png"));
        project.image(QStringLiteral("c.png"));
        project.annotation(QStringLiteral("a.png"), "scratch");
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "1 0.5 0.5 0.1 0.2\n");
        writeFile(project.labels.filePath(QStringLiteral("b.txt")), "1 0.5 0.5 0.1 0.2\n0 0.25 0.25 0.1 0.1\n");

        ImportRequest request;
        ImportReport report = project.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(itemOf(report, QStringLiteral("a.png"))->result, ImportResult::HasAnnotation);
        QCOMPARE(itemOf(report, QStringLiteral("b.png"))->result, ImportResult::Imported);
        QCOMPARE(itemOf(report, QStringLiteral("b.png"))->shapes, 2);
        QCOMPARE(itemOf(report, QStringLiteral("c.png"))->result, ImportResult::NoLabelFile);
        QCOMPARE(project.shapes(QStringLiteral("a.png")).size(), 1);
        QCOMPARE(project.shapes(QStringLiteral("b.png"))[0].label, QStringLiteral("dent"));
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("c.json"))));

        request.mode = ImportMode::Append;
        report = project.run(request);
        QCOMPARE(itemOf(report, QStringLiteral("a.png"))->result, ImportResult::Imported);
        const QList<Shape> appended = project.shapes(QStringLiteral("a.png"));
        QCOMPARE(appended.size(), 2);
        QCOMPARE(appended[0].label, QStringLiteral("scratch"));
        QCOMPARE(appended[1].label, QStringLiteral("dent"));
        QVERIFY(!project.checked(QStringLiteral("a.png")));  // was confirmed
        QCOMPARE(project.shapes(QStringLiteral("b.png")).size(), 4);

        request.mode = ImportMode::Replace;
        report = project.run(request);
        QCOMPARE(project.shapes(QStringLiteral("a.png")).size(), 1);
        QCOMPARE(project.shapes(QStringLiteral("a.png"))[0].label, QStringLiteral("dent"));
        QCOMPARE(project.shapes(QStringLiteral("b.png")).size(), 2);
    }

    // One label file that cannot be imported stops the import; skipping imports the others.
    void unconvertibleStopsTheImport()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.image(QStringLiteral("b.png"));
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "1 0.5 0.5 0.1 0.2\n");
        writeFile(project.labels.filePath(QStringLiteral("b.txt")), "5 0.5 0.5 0.1 0.2\n");  // no class 5

        ImportRequest request;
        ImportReport report = project.run(request);
        QVERIFY(!report.written);
        QVERIFY(!report.error.isEmpty());
        QCOMPARE(report.unconvertible().size(), 1);
        QCOMPARE(report.unconvertible()[0].image, QStringLiteral("b.png"));
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("a.json"))));

        request.skipUnconvertible = true;
        report = project.run(request);
        QVERIFY(report.written);
        QVERIFY(QFile::exists(project.data.filePath(QStringLiteral("a.json"))));
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("b.json"))));
    }

    // Edits of the window that are not in the annotation file (a recovery record, or held in
    // memory after a failed save) are not overwritten.
    void unwrittenEditsAreLeftAlone()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.image(QStringLiteral("b.png"));
        project.image(QStringLiteral("c.png"));
        for (const char* name : {"a.txt", "b.txt", "c.txt"})
            writeFile(project.labels.filePath(QLatin1String(name)), "1 0.5 0.5 0.1 0.2\n");
        {
            const std::unique_ptr<ProjectSession> session = project.open();
            const ProjectLocation& location = session->location();
            formats::RecoveryRecord record;
            record.formatVersion = QStringLiteral("1.0");
            record.projectId = location.projectId;
            record.bindingId = location.bindingId;
            record.rootPathAtWrite = location.rootPath;
            record.imagePath = QStringLiteral("a.png");
            record.documentVersion = 1;
            record.createdAt = QStringLiteral("2026-10-23T10:00:00+08:00");
            record.annotation = R"({"version": "4.2.0", "flags": {}, "shapes": [], "imagePath": "a.png", )"
                                R"("imageData": null, "imageHeight": 100, "imageWidth": 200})";
            const LocalDataPaths paths(project.local.path());
            const QString dir = paths.recoveryDir(location.projectId, location.bindingId);
            QVERIFY(QDir().mkpath(dir));
            writeFile(dir + QStringLiteral("/0001.json"), *formats::recoveryRecordToBytes(record));
            QCOMPARE(RecoveryService(paths, location).scan().candidates.size(), 1);
        }
        ImportRequest request;
        request.heldImages = {QStringLiteral("b.png")};
        const ImportReport report = project.run(request);
        QVERIFY(report.written);
        QCOMPARE(itemOf(report, QStringLiteral("a.png"))->result, ImportResult::NotWritten);
        QCOMPARE(itemOf(report, QStringLiteral("b.png"))->result, ImportResult::NotWritten);
        QCOMPARE(itemOf(report, QStringLiteral("c.png"))->result, ImportResult::Imported);
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("a.json"))));
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("b.json"))));
    }

    // COCO records match by relative path, then by a file name only one image has. Records
    // without an image are reported; a size that differs from the image's is refused.
    void cocoRecords()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.image(QStringLiteral("x/b.png"));
        project.image(QStringLiteral("x/c.png"));
        project.image(QStringLiteral("y/c.png"));
        project.image(QStringLiteral("d.png"), 50, 50);
        const QString file = project.labels.filePath(QStringLiteral("instances.json"));
        writeFile(file, coco(R"({"id": 1, "file_name": "a.png", "width": 200, "height": 100},
                                {"id": 2, "file_name": "images/b.png", "width": 200, "height": 100},
                                {"id": 3, "file_name": "c.png", "width": 200, "height": 100},
                                {"id": 4, "file_name": "y/c.png", "width": 200, "height": 100},
                                {"id": 5, "file_name": "d.png", "width": 200, "height": 100},
                                {"id": 6, "file_name": "gone.png", "width": 200, "height": 100})",
                             R"({"id": 1, "image_id": 1, "category_id": 2, "bbox": [1, 2, 3, 4]},
                                {"id": 2, "image_id": 2, "category_id": 1, "bbox": [1, 2, 3, 4]},
                                {"id": 3, "image_id": 4, "category_id": 1, "bbox": [1, 2, 3, 4]},
                                {"id": 4, "image_id": 5, "category_id": 1, "bbox": [1, 2, 3, 4]})"));
        ImportRequest request;
        request.format = formats::ImportFormat::CocoDetection;
        request.labelFile = file;
        ImportReport report = project.run(request);
        QCOMPARE(report.unconvertible().size(), 1);
        QCOMPARE(report.unconvertible()[0].image, QStringLiteral("d.png"));
        QVERIFY2(report.unconvertible()[0].reason.contains(QStringLiteral("200")), qPrintable(report.unconvertible()[0].reason));
        request.skipUnconvertible = true;
        report = project.run(request);
        QVERIFY(report.written);
        QCOMPARE(itemOf(report, QStringLiteral("a.png"))->result, ImportResult::Imported);
        QCOMPARE(project.shapes(QStringLiteral("a.png"))[0].label, QStringLiteral("dent"));
        QCOMPARE(itemOf(report, QStringLiteral("x/b.png"))->result, ImportResult::Imported);
        QCOMPARE(itemOf(report, QStringLiteral("x/c.png"))->result, ImportResult::NoLabelFile);
        QCOMPARE(itemOf(report, QStringLiteral("y/c.png"))->result, ImportResult::Imported);
        QCOMPARE(report.unmatched.size(), 2);
        QCOMPARE(report.unmatched[0].fileName, QStringLiteral("c.png"));  // two images have the name
        QCOMPARE(report.unmatched[1].fileName, QStringLiteral("gone.png"));

        // An annotation of an image the file does not list: the file is not read.
        writeFile(file, coco(R"({"id": 1, "file_name": "a.png", "width": 200, "height": 100})",
                             R"({"id": 1, "image_id": 7, "category_id": 2, "bbox": [1, 2, 3, 4]})"));
        report = project.run(request);
        QVERIFY(!report.written);
        QVERIFY(!report.error.isEmpty());
    }

    // A label file paired by its path with one image is not taken by name for another.
    void exactPairingTakesTheFile()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.image(QStringLiteral("sub/a.png"));
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "1 0.5 0.5 0.1 0.2\n");
        const ImportReport report = project.run({});
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(itemOf(report, QStringLiteral("a.png"))->result, ImportResult::Imported);
        QCOMPARE(itemOf(report, QStringLiteral("sub/a.png"))->result, ImportResult::NoLabelFile);
        QVERIFY(!itemOf(report, QStringLiteral("sub/a.png"))->reason.isEmpty());
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("sub/a.json"))));
    }

    // An image that changed after its labels were converted is not written with the old
    // coordinates.
    void imageChangedBeforeWriting()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "0 0.5 0.5 0.2 0.2\n");
        bool replaced = false;
        const ImportReport report = project.run({}, [&](int done, int total) {
            if (!replaced && total > 0 && done == total / 2) {  // the writing starts
                project.image(QStringLiteral("a.png"), 400, 200);
                replaced = true;
            }
            return true;
        });
        QVERIFY(replaced);
        QVERIFY(!report.written);
        QCOMPARE(itemOf(report, QStringLiteral("a.png"))->result, ImportResult::NotWritten);
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("a.json"))));
    }

    // An image of the same name that turns up after the check is still found right before
    // the write, for ASCII names, names that others fold to, and Chinese ones: nothing is
    // written to the annotation file the images would share.
    void sameNameImageBeforeWriting()
    {
        Project project;
        const QStringList images = {QStringLiteral("plain.png"), QStringLiteral("ses-k.png"), QStringLiteral(u"划痕.png"),
                                    QStringLiteral("kept.png")};
        for (const QString& image : images) {
            project.image(image);
            writeFile(project.labels.filePath(image.left(image.lastIndexOf(QLatin1Char('.'))) + QStringLiteral(".txt")),
                      "0 0.5 0.5 0.2 0.2\n");
        }
        const QStringList arriving = {QStringLiteral("PLAIN.jpg"), QStringLiteral(u"\u017FE\u017F-\u212A.bmp"),
                                      QStringLiteral(u"划痕.JPG")};
        bool added = false;
        const ImportReport report = project.run({}, [&](int done, int total) {
            if (!added && total > 0 && done == total / 2) {  // the writing starts
                for (const QString& image : arriving)
                    writeFile(project.data.filePath(image), "x");
                added = true;
            }
            return true;
        });
        QVERIFY(added);
        for (int i = 0; i < 3; ++i) {
            const ImportItem* item = itemOf(report, images[i]);
            QVERIFY2(item, qPrintable(images[i]));
            QCOMPARE(item->result, ImportResult::NotWritten);
            QCOMPARE(item->reason, QStringLiteral(u"同一目录下有同名图片"));
            QVERIFY(!QFile::exists(project.data.filePath(images[i].left(images[i].lastIndexOf(QLatin1Char('.'))) + QStringLiteral(".json"))));
        }
        QCOMPARE(itemOf(report, QStringLiteral("kept.png"))->result, ImportResult::Imported);
    }

    // Appended instances get group numbers the image does not use yet; the parts of one
    // instance keep sharing theirs.
    void appendedGroupsDoNotMerge()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        writeFile(project.data.filePath(QStringLiteral("a.json")),
                  R"({"version": "4.2.0", "flags": {}, "shapes": [{"label": "scratch", "points": [[1.0, 1.0], [9.0, 1.0], [9.0, 9.0]], )"
                  R"("group_id": 1, "shape_type": "polygon", "flags": {}}], "imagePath": "a.png", "imageData": null, )"
                  R"("imageHeight": 100, "imageWidth": 200})");
        const QString file = project.labels.filePath(QStringLiteral("instances.json"));
        writeFile(file, coco(R"({"id": 1, "file_name": "a.png", "width": 200, "height": 100})",
                             R"({"id": 1, "image_id": 1, "category_id": 1, "bbox": [0, 0, 60, 60], "iscrowd": 0, "area": 10,
                                 "segmentation": [[10, 10, 20, 10, 20, 20], [40, 40, 50, 40, 50, 50]]})"));
        ImportRequest request;
        request.format = formats::ImportFormat::CocoSegmentation;
        request.labelFile = file;
        request.mode = ImportMode::Append;
        const ImportReport report = project.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        const QList<Shape> shapes = project.shapes(QStringLiteral("a.png"));
        QCOMPARE(shapes.size(), 3);
        QCOMPARE(shapes[0].groupId, std::optional<qint64>(1));
        QVERIFY(shapes[1].groupId);
        QVERIFY(*shapes[1].groupId != 1);
        QCOMPARE(shapes[2].groupId, shapes[1].groupId);
    }

    // When the recovery records cannot be listed, it is not known which images have edits
    // only there: nothing is written.
    void unreadableRecoveryStopsTheImport()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.annotation(QStringLiteral("a.png"), "scratch");
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "1 0.5 0.5 0.1 0.2\n");
        {
            const std::unique_ptr<ProjectSession> session = project.open();
            const LocalDataPaths paths(project.local.path());
            const QString dir = paths.recoveryDir(session->location().projectId, session->location().bindingId);
            writeFile(dir, "not a folder");  // listing it fails, as without permission
            QVERIFY(!RecoveryService(paths, session->location()).scan().error.isEmpty());
        }
        const QByteArray before = [&] {
            QFile f(project.data.filePath(QStringLiteral("a.json")));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }();
        ImportRequest request;
        request.mode = ImportMode::Replace;
        const ImportReport report = project.run(request);
        QVERIFY(!report.written);
        QVERIFY(!report.error.isEmpty());
        QFile f(project.data.filePath(QStringLiteral("a.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), before);
    }

    // Only written images count as imported: a cancel leaves the rest "not written", and an
    // empty label file for an image without annotations changes nothing.
    void countsOnlyWrittenImages()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.image(QStringLiteral("b.png"));
        project.image(QStringLiteral("c.png"));
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "1 0.5 0.5 0.1 0.2\n");
        writeFile(project.labels.filePath(QStringLiteral("b.txt")), "1 0.5 0.5 0.1 0.2\n");
        writeFile(project.labels.filePath(QStringLiteral("c.txt")), "");
        int calls = 0;
        ImportReport report = project.run({}, [&](int done, int total) {
            ++calls;
            return !(total > 0 && done == total / 2 + 1);  // cancel after the first write
        });
        QVERIFY(report.cancelled);
        QVERIFY(report.written);
        QCOMPARE(report.count(ImportResult::Imported), 1);
        QCOMPARE(itemOf(report, QStringLiteral("a.png"))->result, ImportResult::Imported);
        QCOMPARE(itemOf(report, QStringLiteral("b.png"))->result, ImportResult::NotWritten);
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("b.json"))));

        report = project.run({});
        QCOMPARE(itemOf(report, QStringLiteral("c.png"))->result, ImportResult::Unchanged);
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("c.json"))));
        QCOMPARE(report.count(ImportResult::Imported), 1);  // b.png

        // A cancel before writing: nothing counts as imported.
        report = project.run({}, [](int, int total) { return total == 0; });
        QVERIFY(report.cancelled);
        QCOMPARE(report.count(ImportResult::Imported), 0);
    }

    // Class numbers are looked up, not used as list positions.
    void sparseClassNumbers()
    {
        Project project;
        writeFile(project.data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", "shapeTypes": ["rectangle"], )"
                  R"("classes": [{"id": 2147483647, "name": "scratch"}]})");
        project.image(QStringLiteral("a.png"));
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "2147483647 0.5 0.5 0.1 0.2\n");
        const ImportReport report = project.run({});
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(project.shapes(QStringLiteral("a.png"))[0].label, QStringLiteral("scratch"));
    }

    // Tabs and runs of spaces separate fields as a single space does: ultralytics trains on
    // such files (str.split()), so they must import. \v and \f are accepted too, which
    // ultralytics does not (it breaks the line there). Field report: 702 tab-separated YOLO
    // files were refused with the whole line taken as the class number.
    void whitespaceSeparatedFields()
    {
        using namespace formats;
        const ClassNames classes{{0, QStringLiteral("dent")}, {1, QStringLiteral("scratch")}};
        const ImportedLabels spaced =
            parseYoloLabels("1 0.5 0.25 0.1 0.2\n0 0.25 0.5 0.5 0.5\n", ImportFormat::YoloDetection, classes, 200, 100);
        QVERIFY2(spaced.error.isEmpty(), qPrintable(spaced.error));
        for (const QByteArray& bytes : {QByteArray("1\t0.5\t0.25\t0.1\t0.2\n0\t0.25\t0.5\t0.5\t0.5\n"),
                                        QByteArray("1   0.5\t\t0.25 \t0.1  0.2\t\r\n\t0  0.25 0.5\v0.5\f0.5  \r\n"),
                                        QByteArray("1\t0.5 0.25 0.1 0.2\n\t \n0 0.25 0.5 0.5 0.5")}) {
            const ImportedLabels tabbed = parseYoloLabels(bytes, ImportFormat::YoloDetection, classes, 200, 100);
            QVERIFY2(tabbed.error.isEmpty(), qPrintable(tabbed.error));
            QCOMPARE(tabbed.shapes.size(), 2);
            for (qsizetype i = 0; i < 2; ++i) {
                QCOMPARE(tabbed.shapes[i].label, spaced.shapes[i].label);
                QCOMPARE(tabbed.shapes[i].points, spaced.shapes[i].points);
            }
        }

        const ImportedLabels seg =
            parseYoloLabels("0\t0.1\t0.1\t0.9\t0.1\t0.5\t0.9\n", ImportFormat::YoloSegmentation, classes, 200, 100);
        QVERIFY2(seg.error.isEmpty(), qPrintable(seg.error));
        QCOMPARE(seg.shapes.size(), 1);
        QCOMPARE(seg.shapes[0].points.size(), 3);

        const ImportedLabels obb = parseYoloLabels("1\t0.1\t0.1\t0.9\t0.1\t0.9\t0.9\t0.1\t0.9\n",
                                                   ImportFormat::YoloObb, classes, 200, 100);
        QVERIFY2(obb.error.isEmpty(), qPrintable(obb.error));
        QCOMPARE(obb.shapes.size(), 1);

        const PoseConfig pose = parsePoseConfig("classes:\n  person: [nose, eye]\n");
        const ImportedLabels keypoints =
            parseYoloPoseLabels("0\t0.5\t0.5\t0.2\t0.2\t0.45\t0.45\t2\t0.55\t0.45\t1\n", pose, 100, 100);
        QVERIFY2(keypoints.error.isEmpty(), qPrintable(keypoints.error));
        QCOMPARE(keypoints.shapes.size(), 3);

        const ImportedLabels dota = parseDotaLabels("imagesource:GoogleEarth\ngsd:0.5\n10\t10\t90\t10\t90\t60\t10\t60\tdent\t0\n");
        QVERIFY2(dota.error.isEmpty(), qPrintable(dota.error));
        QCOMPARE(dota.shapes.size(), 1);
        QCOMPARE(dota.shapes[0].label, QStringLiteral("dent"));

        // Still one class number per line: a malformed number is reported as before.
        const ImportedLabels bad = parseYoloLabels("1x\t0.5\t0.25\t0.1\t0.2\n", ImportFormat::YoloDetection, classes, 200, 100);
        QVERIFY(bad.error.contains(QStringLiteral(u"类别编号“1x”")));
    }

    // Review of #30: in a Windows folder set to tell case apart, "a.JSON" is still the
    // annotation of "a.png" (as the scan pairs it): it is read, the default import leaves the
    // image alone, and no second "a.json" is made.
    void caseSensitiveWindowsFolder()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows folders set to tell case apart");
#else
        Project project;
        QVERIFY(QDir(project.data.path()).mkpath(QStringLiteral("cs")));
        if (!makeCaseSensitive(project.data.filePath(QStringLiteral("cs"))))
            QSKIP("this Windows cannot set a folder to tell case apart");
        project.image(QStringLiteral("cs/a.png"));
        project.annotation(QStringLiteral("cs/a.png"), "scratch");
        const QString lower = project.data.filePath(QStringLiteral("cs/a.json"));
        const QString upper = project.data.filePath(QStringLiteral("cs/a.JSON"));
        QVERIFY(QFile::rename(lower, upper));
        QVERIFY(!QFileInfo::exists(lower));
        QCOMPARE(formats::existingAnnotationPath(lower), upper);
        QCOMPARE(project.open()->annotationFilePath(QStringLiteral("cs/a.png")), upper);
        // A folder named "a.json" beside it is no annotation file.
        QVERIFY(QDir(project.data.path()).mkdir(QStringLiteral("cs/a.json")));
        QCOMPARE(formats::existingAnnotationPath(lower), upper);
        QVERIFY(QDir(project.data.path()).rmdir(QStringLiteral("cs/a.json")));

        writeFile(project.labels.filePath(QStringLiteral("cs/a.txt")), "1 0.5 0.5 0.1 0.2\n");
        const ImportReport report = project.run({});
        QCOMPARE(itemOf(report, QStringLiteral("cs/a.png"))->result, ImportResult::HasAnnotation);
        QVERIFY(!QFileInfo::exists(lower));
        QCOMPARE(QDir(project.data.filePath(QStringLiteral("cs"))).entryList(QDir::Files),
                 (QStringList{QStringLiteral("a.JSON"), QStringLiteral("a.png")}));
#endif
    }

    // A tab-separated file imports through the whole service as well.
    void tabSeparatedYoloFile()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "1\t0.5\t0.5\t0.1\t0.2\r\n");
        const ImportReport report = project.run({});
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(report.count(ImportResult::Imported), 1);
        QCOMPARE(project.shapes(QStringLiteral("a.png")).size(), 1);
    }

    // Coordinates that overflow when scaled to the image are refused before anything is
    // written.
    void overflowIsRefusedBeforeWriting()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.image(QStringLiteral("b.png"));
        writeFile(project.labels.filePath(QStringLiteral("a.txt")), "1 0.5 0.5 0.1 0.2\n");
        writeFile(project.labels.filePath(QStringLiteral("b.txt")), "0 1e308 0.5 0.2 0.2\n");
        const ImportReport report = project.run({});
        QVERIFY(!report.written);
        QCOMPARE(itemOf(report, QStringLiteral("b.png"))->result, ImportResult::Unconvertible);
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("a.json"))));
        QCOMPARE(report.count(ImportResult::Imported), 0);
    }

    void maskMapping()
    {
        using formats::parseMaskMapping;
        QVERIFY(parseMaskMapping(R"({"type": "grayscale", "colors": {"a": 1, "b": 255}})").error.isEmpty());
        QVERIFY(parseMaskMapping(R"({"type": "rgb", "colors": {"a": [255, 0, 0]}})").error.isEmpty());
        QVERIFY(!parseMaskMapping(R"({"type": "grayscale", "colors": {"a": 256}})").error.isEmpty());
        QVERIFY(!parseMaskMapping(R"({"type": "grayscale", "colors": {"a": 1.5}})").error.isEmpty());
        QVERIFY(!parseMaskMapping(R"({"type": "rgb", "colors": {"a": [255, 0]}})").error.isEmpty());
        QVERIFY(!parseMaskMapping(R"({"type": "palette", "colors": {}})").error.isEmpty());
        QVERIFY(!parseMaskMapping(R"({"colors": {}})").error.isEmpty());
        QVERIFY(!parseMaskMapping("not json").error.isEmpty());
    }

    // External contours only (a hole is part of its region), corners of straight edges only.
    void maskContours()
    {
        const int w = 12, h = 10;
        std::vector<std::uint8_t> mask(std::size_t(w * h), 0);
        for (int y = 2; y <= 7; ++y) {
            for (int x = 1; x <= 8; ++x)
                mask[std::size_t(y * w + x)] = 1;
        }
        mask[std::size_t(4 * w + 4)] = 0;  // a hole
        const QList<formats::IntPolygon> polygons = formats::maskPolygons(mask, w, h);
        QCOMPARE(polygons.size(), 1);
        QCOMPARE(polygons[0], (formats::IntPolygon{QPoint(1, 2), QPoint(1, 7), QPoint(8, 7), QPoint(8, 2)}));

        // A single pixel and a line have fewer than three points and are passed over.
        std::fill(mask.begin(), mask.end(), 0);
        mask[std::size_t(5 * w + 5)] = 1;
        for (int x = 0; x < 6; ++x)
            mask[std::size_t(x)] = 1;
        QVERIFY(formats::maskPolygons(mask, w, h).isEmpty());
    }

    // Masks: PNG only, the image's size, grayscale values with R = G = B. A mask folder in
    // the data folder: a mask is never its own label.
    void masks()
    {
        Project project;
        project.image(QStringLiteral("a.png"));
        project.image(QStringLiteral("b.png"));
        project.image(QStringLiteral("c.png"));
        project.image(QStringLiteral("e.png"));
        QImage mask(200, 100, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 10; y < 30; ++y) {
            for (int x = 20; x < 60; ++x)
                mask.setPixel(x, y, qRgb(2, 2, 2));
        }
        const QString masks = project.data.filePath(QStringLiteral("masks"));
        QDir().mkpath(masks);
        QVERIFY(mask.save(masks + QStringLiteral("/a.png")));
        QVERIFY(mask.save(masks + QStringLiteral("/b.jpg"), "JPG"));
        QVERIFY(mask.scaled(100, 50).save(masks + QStringLiteral("/c.png")));
        QImage colored = mask.convertToFormat(QImage::Format_RGB32);
        colored.setPixel(30, 20, qRgb(2, 3, 2));
        QVERIFY(colored.save(masks + QStringLiteral("/e.png")));
        writeFile(project.data.filePath(QStringLiteral("mask_colors.json")),
                  R"({"type": "grayscale", "colors": {"scratch": 1, "dent": 2}})");

        ImportRequest request;
        request.format = formats::ImportFormat::Mask;
        request.labelDir = masks;
        request.skipUnconvertible = true;
        const ImportReport report = project.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(itemOf(report, QStringLiteral("a.png"))->result, ImportResult::Imported);
        const QList<Shape> shapes = project.shapes(QStringLiteral("a.png"));
        QCOMPARE(shapes.size(), 1);
        QCOMPARE(shapes[0].label, QStringLiteral("dent"));
        QCOMPARE(shapes[0].points,
                 (PointList{QPointF(20, 10), QPointF(20, 29), QPointF(59, 29), QPointF(59, 10)}));
        QCOMPARE(itemOf(report, QStringLiteral("b.png"))->result, ImportResult::Unconvertible);
        QVERIFY(itemOf(report, QStringLiteral("b.png"))->reason.contains(QStringLiteral("PNG")));
        QCOMPARE(itemOf(report, QStringLiteral("c.png"))->result, ImportResult::Unconvertible);
        QCOMPARE(itemOf(report, QStringLiteral("e.png"))->result, ImportResult::Unconvertible);
        QCOMPARE(itemOf(report, QStringLiteral("masks/a.png"))->result, ImportResult::NoLabelFile);
        QVERIFY(!QFile::exists(project.data.filePath(QStringLiteral("masks/a.json"))));
    }
};

QTEST_GUILESS_MAIN(TestImport)
#include "tst_import.moc"
