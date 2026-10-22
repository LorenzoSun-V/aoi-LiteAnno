// Export (architecture 11): only confirmed images, nothing written when an object cannot be
// converted unless those images are skipped, the output folder rules, batches as the scope,
// and liteanno-cli's init, status and confirm. Byte-for-byte comparison with upstream's
// converter is in python/export_compat.py.

#include "application/ExportService.h"
#include "application/LocalDataPaths.h"
#include "application/ProjectSession.h"
#include "application/WorksetStore.h"
#include "imaging/ImageLoader.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#ifndef Q_OS_WIN
#include <csignal>
#include <sys/resource.h>
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

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList filesUnder(const QString& folder)
{
    QStringList files;
    QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(QDir(folder).relativeFilePath(it.next()));
    files.sort();
    return files;
}

QByteArray rectangle(const char* label, double x1, double y1, double x2, double y2)
{
    return QStringLiteral(R"({"label": "%1", "points": [[%2, %3], [%4, %3], [%4, %5], [%2, %5]], "group_id": null, )"
                          R"("description": "", "difficult": false, "shape_type": "rectangle", "flags": {}})")
        .arg(QLatin1String(label))
        .arg(x1, 0, 'f', 1)
        .arg(y1, 0, 'f', 1)
        .arg(x2, 0, 'f', 1)
        .arg(y2, 0, 'f', 1)
        .toUtf8();
}

QByteArray polygon(const char* label)
{
    return QByteArray(R"({"label": ")") + label
           + R"(", "points": [[10.0, 10.0], [50.0, 10.0], [30.0, 40.0]], "group_id": null, "description": "", )"
             R"("difficult": false, "shape_type": "polygon", "flags": {}})";
}

struct Project {
    QTemporaryDir local;
    QTemporaryDir data;
    QTemporaryDir out;

    Project()
    {
        writeFile(data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", "shapeTypes": ["rectangle", "polygon"], )"
                  R"("classes": [{"id": 0, "name": "scratch"}, {"id": 1, "name": "dent"}], "batchSize": 2})");
    }

    // An image and its annotation file; `shapes` is a JSON array body.
    void image(const QString& path, const QByteArray& shapes, bool checked)
    {
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        const QString file = data.filePath(path);
        QDir().mkpath(QFileInfo(file).absolutePath());
        QVERIFY(image.save(file));
        const QString name = QFileInfo(path).fileName();
        writeFile(data.filePath(path.left(path.lastIndexOf(QLatin1Char('.'))) + QStringLiteral(".json")),
                  QByteArray(R"({"version": "4.2.0", "flags": {}, "shapes": [)") + shapes + R"(], "imagePath": ")"
                      + name.toUtf8() + R"(", "imageData": null, "imageHeight": 100, "imageWidth": 200, "checked": )"
                      + (checked ? "true" : "false") + "}");
    }

    std::unique_ptr<ProjectSession> open()
    {
        ProjectSession::OpenResult result = ProjectSession::open(data.path(), LocalDataPaths(local.path()));
        if (!result.session)
            qFatal("cannot open the project: %s", qPrintable(result.message));
        return std::move(result.session);
    }

    QString output(const char* name = "export") const { return out.filePath(QLatin1String(name)); }
};

QJsonObject manifestOf(const QString& folder)
{
    return QJsonDocument::fromJson(readFile(folder + QStringLiteral("/liteanno-export.json"))).object();
}

} // namespace

class TestExport : public QObject
{
    Q_OBJECT

private slots:
    void onlyConfirmedImagesAreExported()
    {
        Project project;
        project.image(QStringLiteral("line1/0001.png"), rectangle("scratch", 10, 10, 60, 50), true);
        project.image(QStringLiteral("line1/0002.png"), QByteArray(), true);  // negative sample
        project.image(QStringLiteral("line1/0003.png"), rectangle("dent", 1, 1, 9, 9), false);
        project.image(QStringLiteral("line2/0001.png"), rectangle("dent", 0, 0, 200, 100), true);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.outputDir = project.output();
        const ExportReport report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(report.count(ExportResult::Exported), 2);
        QCOMPARE(report.count(ExportResult::Negative), 1);
        QCOMPARE(report.count(ExportResult::NotDone), 1);
        // Same-named images in two folders keep their folders.
        QCOMPARE(filesUnder(project.output()),
                 (QStringList{QStringLiteral("classes.txt"), QStringLiteral("line1/0001.txt"), QStringLiteral("line1/0002.txt"),
                              QStringLiteral("line2/0001.txt"), QStringLiteral("liteanno-export.json")}));
        QCOMPARE(readFile(project.output() + QStringLiteral("/line1/0002.txt")), QByteArray());
        QCOMPARE(readFile(project.output() + QStringLiteral("/line1/0001.txt")),
                 QByteArray("0 0.175000 0.300000 0.250000 0.400000\n"));
        QCOMPARE(readFile(project.output() + QStringLiteral("/classes.txt")), QByteArray("scratch\ndent\n"));

        const QJsonObject manifest = manifestOf(project.output());
        QCOMPARE(manifest.value(QStringLiteral("projectId")).toString(), QStringLiteral("3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"));
        QCOMPARE(manifest.value(QStringLiteral("format")).toString(), QStringLiteral("yolo-det"));
        QCOMPARE(manifest.value(QStringLiteral("scope")).toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("project"));
        const QJsonObject counts = manifest.value(QStringLiteral("counts")).toObject();
        QCOMPARE(counts.value(QStringLiteral("exported")).toInt(), 2);
        QCOMPARE(counts.value(QStringLiteral("negative")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("not-done")).toInt(), 1);
        QCOMPARE(counts.value(QStringLiteral("clampedObjects")).toInt(), 0);
        const QJsonArray items = manifest.value(QStringLiteral("items")).toArray();
        QCOMPARE(items.size(), 4);
        QCOMPARE(items[2].toObject().value(QStringLiteral("result")).toString(), QStringLiteral("not-done"));
        QCOMPARE(items[2].toObject().value(QStringLiteral("status")).toString(), QStringLiteral("pending-confirmation"));
    }

    // Architecture 11: no object is dropped; the user decides to leave such images out.
    void unconvertibleObjectsStopTheExport()
    {
        Project project;
        project.image(QStringLiteral("0001.png"), rectangle("scratch", 10, 10, 60, 50), true);
        project.image(QStringLiteral("0002.png"), rectangle("dent", 10, 10, 60, 50) + ", " + polygon("dent"), true);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.format = formats::ExportFormat::CocoDetection;
        request.outputDir = project.output();
        ExportReport report = service.run(request);
        QVERIFY(!report.written);
        QVERIFY(!QFileInfo::exists(project.output()));
        QCOMPARE(report.unconvertible().size(), 1);
        QCOMPARE(report.unconvertible().first().image, QStringLiteral("0002.png"));
        QVERIFY2(report.unconvertible().first().reason.contains(QStringLiteral(u"第 2 个对象")),
                 qPrintable(report.unconvertible().first().reason));

        request.skipUnconvertible = true;
        report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        const QJsonObject coco = QJsonDocument::fromJson(readFile(project.output() + QStringLiteral("/coco_detection.json"))).object();
        QCOMPARE(coco.value(QStringLiteral("images")).toArray().size(), 1);
        const QJsonArray items = manifestOf(project.output()).value(QStringLiteral("items")).toArray();
        QCOMPARE(items[1].toObject().value(QStringLiteral("result")).toString(), QStringLiteral("unconvertible"));
    }

    void outputFolderRules()
    {
        Project project;
        project.image(QStringLiteral("0001.png"), rectangle("scratch", 10, 10, 60, 50), true);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.outputDir = project.data.filePath(QStringLiteral("export"));
        QVERIFY(service.run(request).error.contains(QStringLiteral(u"数据文件夹")));
        request.outputDir = QFileInfo(project.data.path()).absolutePath();
        QVERIFY(service.run(request).error.contains(QStringLiteral(u"数据文件夹")));
        writeFile(project.output() + QStringLiteral("/keep.txt"), "mine");
        request.outputDir = project.output();
        QVERIFY(service.run(request).error.contains(QStringLiteral(u"不是空的")));
        QCOMPARE(readFile(project.output() + QStringLiteral("/keep.txt")), QByteArray("mine"));
        // An empty folder is fine.
        QVERIFY(QFile::remove(project.output() + QStringLiteral("/keep.txt")));
        QVERIFY(service.run(request).written);
    }

    void cancellingRemovesWhatWasWritten()
    {
        Project project;
        for (int i = 1; i <= 4; ++i)
            project.image(QStringLiteral("%1.png").arg(i), rectangle("scratch", 10, 10, 60, 50), true);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.outputDir = project.output();
        request.copyImages = true;
        const ExportReport report = service.run(request, [](int done, int total) { return total == 0 || done < total - 1; });
        QVERIFY(report.cancelled);
        QVERIFY(!report.written);
        QVERIFY(filesUnder(project.output()).isEmpty());
        QVERIFY(!QFileInfo::exists(project.output()));
    }

    void batchesAreTheScope()
    {
        Project project;
        for (int i = 1; i <= 3; ++i)
            project.image(QStringLiteral("%1.png").arg(i), rectangle("scratch", 10, 10, 60, 50), true);
        const auto session = project.open();
        WorksetStore worksets(project.data.path(), QStringLiteral("3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"));
        QVERIFY(worksets.createNext({QStringLiteral("1.png"), QStringLiteral("2.png"), QStringLiteral("3.png")}, 2).batch);
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.format = formats::ExportFormat::VocRectangle;
        request.outputDir = project.output();
        request.batches = {QStringLiteral("b000001")};
        request.copyImages = true;
        const ExportReport report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(filesUnder(project.output()),
                 (QStringList{QStringLiteral("1.png"), QStringLiteral("1.xml"), QStringLiteral("2.png"), QStringLiteral("2.xml"),
                              QStringLiteral("liteanno-export.json")}));
        const QJsonObject scope = manifestOf(project.output()).value(QStringLiteral("scope")).toObject();
        QCOMPARE(scope.value(QStringLiteral("kind")).toString(), QStringLiteral("batches"));
        QCOMPARE(scope.value(QStringLiteral("batches")).toArray().first().toObject().value(QStringLiteral("number")).toInt(), 1);

        request.batches = {QStringLiteral("b000009")};
        request.outputDir = project.output("other");
        QVERIFY(service.run(request).error.contains(QStringLiteral("b000009")));
    }

    // Review of PR #8, item 4: a rectangle is its bounding box whatever the order of its four
    // points (format spec 4.2), in YOLO and COCO too.
    void rectanglePointsInAnyOrder()
    {
        Project project;
        project.image(QStringLiteral("0001.png"),
                      R"({"label": "scratch", "points": [[10, 10], [80, 60], [80, 10], [10, 60]], "group_id": null, )"
                      R"("description": "", "difficult": false, "shape_type": "rectangle", "flags": {}})",
                      true);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.outputDir = project.output("yolo");
        ExportReport report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(readFile(project.output("yolo") + QStringLiteral("/0001.txt")),
                 QByteArray("0 0.225000 0.350000 0.350000 0.500000\n"));
        request.format = formats::ExportFormat::CocoDetection;
        request.outputDir = project.output("coco");
        report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        const QJsonObject annotation = QJsonDocument::fromJson(readFile(project.output("coco") + QStringLiteral("/coco_detection.json")))
                                           .object()
                                           .value(QStringLiteral("annotations"))
                                           .toArray()
                                           .first()
                                           .toObject();
        QCOMPARE(annotation.value(QStringLiteral("bbox")).toArray(), (QJsonArray{10, 10, 70, 50}));
        QCOMPARE(annotation.value(QStringLiteral("area")).toInt(), 3500);
    }

    // Review of PR #8, item 5: an "imageWidth" that is not a positive integer reads as
    // missing (format spec 13.3); the image's size is used.
    void invalidSizeInTheFileIsNotUsed()
    {
        Project project;
        project.image(QStringLiteral("0001.png"), rectangle("scratch", 10, 10, 60, 50), true);
        QByteArray json = readFile(project.data.filePath(QStringLiteral("0001.json")));
        json.replace("\"imageWidth\": 200", "\"imageWidth\": 0");
        writeFile(project.data.filePath(QStringLiteral("0001.json")), json);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.outputDir = project.output("yolo");
        ExportReport report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(readFile(project.output("yolo") + QStringLiteral("/0001.txt")),
                 QByteArray("0 0.175000 0.300000 0.250000 0.400000\n"));
        request.format = formats::ExportFormat::CocoDetection;
        request.outputDir = project.output("coco");
        report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        const QJsonObject image = QJsonDocument::fromJson(readFile(project.output("coco") + QStringLiteral("/coco_detection.json")))
                                      .object()
                                      .value(QStringLiteral("images"))
                                      .toArray()
                                      .first()
                                      .toObject();
        QCOMPARE(image.value(QStringLiteral("width")).toInt(), 200);
    }

    // Review of PR #8, item 6: an image replaced by one of another size between the check and
    // the writing is not exported with the old coordinates.
    void imageChangedDuringTheExport()
    {
        Project project;
        project.image(QStringLiteral("0001.png"), rectangle("scratch", 10, 10, 60, 50), true);
        project.image(QStringLiteral("0002.png"), rectangle("dent", 10, 10, 60, 50), true);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.outputDir = project.output();
        request.copyImages = true;
        bool replaced = false;
        const ExportReport report = service.run(request, [&](int done, int total) {
            if (total > 0 && done == total / 2 && !replaced) {  // the check is over
                QImage bigger(400, 200, QImage::Format_RGB32);
                bigger.fill(Qt::red);
                replaced = bigger.save(project.data.filePath(QStringLiteral("0002.png")));
            }
            return true;
        });
        QVERIFY(replaced);
        QVERIFY(!report.written);
        QVERIFY2(report.error.contains(QStringLiteral("0002.png")), qPrintable(report.error));
        QVERIFY(!QFileInfo::exists(project.output()));
    }

    // Review of PR #8, item 7: COCO takes the class ids of the project file, as YOLO and the
    // manifest do (architecture 11).
    void cocoUsesTheProjectsClassIds()
    {
        Project project;
        writeFile(project.data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", "shapeTypes": ["rectangle", "polygon"], )"
                  R"("classes": [{"id": 5, "name": "scratch"}, {"id": 1, "name": "dent"}]})");
        project.image(QStringLiteral("0001.png"), rectangle("scratch", 10, 10, 60, 50), true);
        project.image(QStringLiteral("0002.png"), polygon("scratch"), true);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        for (const auto& [format, file] : {std::pair{formats::ExportFormat::CocoDetection, "coco_detection.json"},
                                           std::pair{formats::ExportFormat::CocoSegmentation, "coco_instance_segmentation.json"}}) {
            ExportRequest request;
            request.format = format;
            request.skipUnconvertible = true;  // rectangles for detection, polygons for segmentation
            request.outputDir = project.output(file);
            const ExportReport report = service.run(request);
            QVERIFY2(report.written, qPrintable(report.error));
            const QJsonObject coco = QJsonDocument::fromJson(readFile(request.outputDir + QLatin1Char('/') + QLatin1String(file))).object();
            QJsonArray ids;
            for (const QJsonValue& category : coco.value(QStringLiteral("categories")).toArray())
                ids.append(category.toObject().value(QStringLiteral("id")));
            QCOMPARE(ids, (QJsonArray{1, 5}));
            QCOMPARE(coco.value(QStringLiteral("annotations")).toArray().first().toObject().value(QStringLiteral("category_id")).toInt(), 5);
            const QJsonArray categories = manifestOf(request.outputDir).value(QStringLiteral("categories")).toArray();
            QCOMPARE(categories.last().toObject().value(QStringLiteral("id")).toInt(), 5);
        }
    }

    // Review of PR #8, item 11: a link in a parent of the output folder cannot lead the export
    // into the data folder.
    void linkIntoTheDataFolderIsRefused()
    {
#ifdef Q_OS_WIN
        QSKIP("creating a link needs extra rights on Windows");
#else
        Project project;
        project.image(QStringLiteral("0001.png"), rectangle("scratch", 10, 10, 60, 50), true);
        const QString link = project.out.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(project.data.path(), link));
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.outputDir = link + QStringLiteral("/export");
        request.copyImages = true;
        const ExportReport report = service.run(request);
        QVERIFY(!report.written);
        QVERIFY2(report.error.contains(QStringLiteral(u"数据文件夹")), qPrintable(report.error));
        QVERIFY(!QFileInfo::exists(project.data.filePath(QStringLiteral("export"))));
#endif
    }

    // Review of PR #8, item 12: when the temporary file of a COCO export cannot be created,
    // the export leaves nothing behind.
    void cocoTemporaryFileFailureLeavesNothing()
    {
#ifdef Q_OS_WIN
        QSKIP("the temporary folder is taken from TMPDIR on Unix only");
#else
        Project project;
        project.image(QStringLiteral("0001.png"), rectangle("scratch", 10, 10, 60, 50), true);
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.format = formats::ExportFormat::CocoDetection;
        request.outputDir = project.output();
        // A file where the temporary folder should be: nothing can be created in it.
        const QString notAFolder = project.out.filePath(QStringLiteral("not-a-folder"));
        writeFile(notAFolder, "x");
        const QByteArray tmp = qgetenv("TMPDIR");
        qputenv("TMPDIR", notAFolder.toLocal8Bit());
        const ExportReport report = service.run(request);
        if (tmp.isEmpty())
            qunsetenv("TMPDIR");
        else
            qputenv("TMPDIR", tmp);
        QVERIFY(!report.written);
        QVERIFY(!QFileInfo::exists(project.output()));
#endif
    }

    // Review of PR #8, item 13: init writes the project file whole or not at all (format spec 1).
    void initLeavesNoPartialProjectFile()
    {
#ifdef Q_OS_WIN
        QSKIP("the file size limit is set with setrlimit on Unix only");
#else
        QTemporaryDir data;
        QTemporaryDir local;
        const QStringList args{QStringLiteral("init"), data.path(), QStringLiteral("--classes"),
                               QStringLiteral("scratch,dent,crack,stain"), QStringLiteral("--local-data"), local.path()};
        QProcess process;
        process.setChildProcessModifier([] {
            ::signal(SIGXFSZ, SIG_IGN);  // the write fails instead of ending the program
            const rlimit limit{100, 100};
            ::setrlimit(RLIMIT_FSIZE, &limit);
        });
        process.start(QStringLiteral(LITEANNO_CLI), args);
        QVERIFY(process.waitForFinished(60000));
        QVERIFY(process.exitCode() != 0);
        QVERIFY(!QFileInfo::exists(data.filePath(QStringLiteral("liteanno/project.json"))));
        // Nothing to clear up: a second try works.
        QProcess again;
        again.start(QStringLiteral(LITEANNO_CLI), args);
        QVERIFY(again.waitForFinished(60000));
        QCOMPARE(again.exitCode(), 0);
        QVERIFY(QJsonDocument::fromJson(readFile(data.filePath(QStringLiteral("liteanno/project.json")))).isObject());
#endif
    }

    void commandLine()
    {
        QTemporaryDir data;
        QTemporaryDir local;
        const auto cli = [&](const QStringList& args, int expectedExit = 0) {
            QProcess process;
            process.start(QStringLiteral(LITEANNO_CLI), args + QStringList{QStringLiteral("--local-data"), local.path()});
            if (!process.waitForFinished(60000))
                qFatal("liteanno-cli did not finish");
            const QString output = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
            if (process.exitCode() != expectedExit)
                qFatal("liteanno-cli %s: exit %d\n%s", qPrintable(args.join(QLatin1Char(' '))), process.exitCode(),
                       qPrintable(output));
            return output;
        };
        cli({QStringLiteral("init"), data.path(), QStringLiteral("--classes"), QStringLiteral("scratch,dent"),
             QStringLiteral("--shape-types"), QStringLiteral("rectangle")});
        const QJsonObject config =
            QJsonDocument::fromJson(readFile(data.filePath(QStringLiteral("liteanno/project.json")))).object();
        QCOMPARE(config.value(QStringLiteral("classes")).toArray().size(), 2);
        QCOMPARE(config.value(QStringLiteral("classes")).toArray()[1].toObject().value(QStringLiteral("id")).toInt(), 1);
        QCOMPARE(config.value(QStringLiteral("shapeTypes")).toArray().first().toString(), QStringLiteral("rectangle"));
        // init never replaces a project file.
        cli({QStringLiteral("init"), data.path()}, 1);

        // Pre-labels written by a script: unconfirmed; one has a class the project lacks.
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        for (const char* name : {"a.png", "b.png", "c.png"})
            QVERIFY(image.save(data.filePath(QLatin1String(name))));
        const auto annotation = [&](const char* name, const QByteArray& shapes) {
            writeFile(data.filePath(QLatin1String(name)),
                      QByteArray(R"({"version": "4.2.0", "flags": {}, "shapes": [)") + shapes
                          + R"(], "imagePath": "x.png", "imageData": null, "imageHeight": 100, "imageWidth": 200, "checked": false})");
        };
        annotation("a.json", rectangle("scratch", 10, 10, 60, 50));
        annotation("b.json", rectangle("crack", 10, 10, 60, 50));
        QString output = cli({QStringLiteral("status"), data.path()});
        QVERIFY2(output.contains(QStringLiteral(u"图片 3 张，已完成 0 张")), qPrintable(output));
        QVERIFY2(output.contains(QStringLiteral(u"待确认：2")), qPrintable(output));
        QVERIFY2(output.contains(QStringLiteral(u"未处理：1")), qPrintable(output));

        const QByteArray before = readFile(data.filePath(QStringLiteral("a.json")));
        output = cli({QStringLiteral("confirm"), data.path(), QStringLiteral("--dry-run")});
        QVERIFY2(output.contains(QStringLiteral(u"可以确认 1 张")), qPrintable(output));
        QCOMPARE(readFile(data.filePath(QStringLiteral("a.json"))), before);
        output = cli({QStringLiteral("confirm"), data.path()});
        QVERIFY2(output.contains(QStringLiteral(u"已确认 1 张")), qPrintable(output));
        QVERIFY(readFile(data.filePath(QStringLiteral("a.json"))).contains("\"checked\": true"));
        QVERIFY(readFile(data.filePath(QStringLiteral("b.json"))).contains("\"checked\": false"));

        const QTemporaryDir out;
        output = cli({QStringLiteral("export"), data.path(), QStringLiteral("--format"), QStringLiteral("coco-det"),
                      QStringLiteral("--out"), out.filePath(QStringLiteral("coco"))});
        QVERIFY2(output.contains(QStringLiteral(u"已导出 1 张")), qPrintable(output));
        cli({QStringLiteral("export"), data.path(), QStringLiteral("--format"), QStringLiteral("nope"),
             QStringLiteral("--out"), out.filePath(QStringLiteral("x"))}, 2);
    }
};

QTEST_GUILESS_MAIN(TestExport)
#include "tst_export.moc"
