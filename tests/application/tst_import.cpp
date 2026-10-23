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
#include "formats/RecoveryRecord.h"
#include "imaging/ImageLoader.h"

#include <QDir>
#include <QFile>
#include <QImage>
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

    ImportReport run(ImportRequest request)
    {
        const std::unique_ptr<ProjectSession> session = open();
        if (request.labelDir.isEmpty() && request.labelFile.isEmpty())
            request.labelDir = labels.path();
        return ImportService(*session, imaging::readImageInfo, imaging::readMaskImage).run(request);
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
