// Export (architecture 11): only confirmed images, nothing written when an object cannot be
// converted unless those images are skipped, the output folder rules, batches as the scope,
// and liteanno-cli's init, status and export. Byte-for-byte comparison with upstream's
// converter is in python/export_compat.py.

#include "application/ExportService.h"
#include "application/LocalDataPaths.h"
#include "application/ProjectSession.h"
#include "application/RecoveryService.h"
#include "application/WorksetStore.h"
#include "formats/RecoveryRecord.h"
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

    // `plainFolder`: no project file, as X-AnyLabeling opens a folder.
    explicit Project(bool plainFolder = false)
    {
        if (plainFolder)
            return;
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
    // A folder without a project file: what is labeled is exported, confirmed or not, with the
    // class ids of a classes.txt; nothing is written into the folder.
    void plainFolderExportsWhatIsLabeled()
    {
        Project project(true);
        project.image(QStringLiteral("line1/0001.png"), rectangle("scratch", 10, 10, 60, 50), false);
        project.image(QStringLiteral("line1/0002.png"), QByteArray(), false);  // opened, nothing drawn
        project.image(QStringLiteral("line1/0003.png"), rectangle("dent", 1, 1, 9, 9), true);
        project.image(QStringLiteral("line1/0004.png"), QByteArray(), true);  // confirmed: no objects
        const QStringList before = filesUnder(project.data.path());
        const auto session = project.open();
        QVERIFY(session->isPlainFolder());
        QVERIFY(session->config().classes.isEmpty());
        QVERIFY(session->location().projectId.startsWith(QStringLiteral("folder-")));
        ExportService service(*session, imaging::readImageInfo);

        ExportRequest request;
        request.outputDir = project.output("none");
        ExportReport report = service.run(request);
        QVERIFY(!report.written);
        QVERIFY2(report.error.contains(QStringLiteral("classes.txt")), qPrintable(report.error));

        const QString classes = project.out.filePath(QStringLiteral("classes.txt"));
        writeFile(classes, "\xEF\xBB\xBF" "dent\r\nscratch\r\n\r\n");
        request.classesFile = classes;
        request.outputDir = project.output("yolo");
        report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(report.count(ExportResult::Exported), 2);
        QCOMPARE(report.count(ExportResult::Negative), 1);
        QCOMPARE(report.count(ExportResult::NotDone), 1);
        QVERIFY(readFile(project.output("yolo") + QStringLiteral("/line1/0001.txt")).startsWith("1 "));
        QVERIFY(readFile(project.output("yolo") + QStringLiteral("/line1/0003.txt")).startsWith("0 "));
        QCOMPARE(readFile(project.output("yolo") + QStringLiteral("/line1/0004.txt")), QByteArray());
        QVERIFY(!QFile::exists(project.output("yolo") + QStringLiteral("/line1/0002.txt")));
        QCOMPARE(readFile(project.output("yolo") + QStringLiteral("/classes.txt")), QByteArray("dent\nscratch\n"));

        // Upstream's COCO numbering: detection from 1.
        request.format = formats::ExportFormat::CocoDetection;
        request.outputDir = project.output("coco");
        report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        const QJsonObject coco = QJsonDocument::fromJson(readFile(project.output("coco") + QStringLiteral("/coco_detection.json"))).object();
        QCOMPARE(coco[QStringLiteral("categories")].toArray()[0].toObject()[QStringLiteral("id")].toInt(), 1);
        QCOMPARE(coco[QStringLiteral("categories")].toArray()[0].toObject()[QStringLiteral("name")].toString(), QStringLiteral("dent"));

        // A label the file does not list cannot be numbered.
        writeFile(classes, "dent\n");
        request.format = formats::ExportFormat::YoloDetection;
        request.outputDir = project.output("missing");
        report = service.run(request);
        QVERIFY(!report.written);
        QCOMPARE(report.unconvertible().size(), 1);
        QVERIFY2(report.unconvertible()[0].reason.contains(QStringLiteral("scratch")), qPrintable(report.unconvertible()[0].reason));

        QCOMPARE(filesUnder(project.data.path()), before);
    }

    // Review of #11, 1 and 7: an unconfirmed image is exported only when a confirm would
    // accept it, and image-level marks alone are not "no objects".
    void plainFolderChecksUnconfirmedImages()
    {
        Project project(true);
        const auto annotation = [&project](const char* stem, const QByteArray& shapes, int width, const char* flags) {
            QImage image(200, 100, QImage::Format_RGB32);
            image.fill(Qt::darkCyan);
            QVERIFY(image.save(project.data.filePath(QLatin1String(stem) + QStringLiteral(".png"))));
            writeFile(project.data.filePath(QLatin1String(stem) + QStringLiteral(".json")),
                      QByteArray(R"({"version": "4.2.0", "flags": )") + flags + R"(, "shapes": [)" + shapes
                          + R"(], "imagePath": ")" + stem + R"(.png", "imageData": null, "imageHeight": 100, "imageWidth": )"
                          + QByteArray::number(width) + R"(, "checked": false})");
        };
        annotation("good", rectangle("scratch", 10, 10, 60, 50), 200, "{}");
        annotation("wrongsize", rectangle("scratch", 10, 10, 60, 50), 100, "{}");
        annotation("flat", rectangle("scratch", 10, 10, 10, 50), 200, "{}");
        annotation("point", R"({"label": "scratch", "points": [[5.0, 5.0], [5.0, 5.0], [5.0, 5.0]], "group_id": null, )"
                            R"("description": "", "difficult": false, "shape_type": "polygon", "flags": {}})",
                   200, "{}");
        annotation("marked", QByteArray(), 200, R"({"NG": true})");
        const QString classes = project.out.filePath(QStringLiteral("classes.txt"));
        writeFile(classes, "scratch\n");
        const auto session = project.open();
        ExportService service(*session, imaging::readImageInfo);
        ExportRequest request;
        request.classesFile = classes;
        request.outputDir = project.output("refused");
        ExportReport report = service.run(request);
        QVERIFY(!report.written);
        QStringList refused;
        for (const ExportItem& item : report.unconvertible())
            refused.append(item.image + QLatin1Char(':') + item.reason);
        refused.sort();
        QCOMPARE(refused.size(), 3);
        QVERIFY2(refused[0].startsWith(QStringLiteral("flat.png:")) && refused[0].contains(QStringLiteral(u"几何无效")), qPrintable(refused[0]));
        QVERIFY2(refused[1].startsWith(QStringLiteral("point.png:")) && refused[1].contains(QStringLiteral(u"几何无效")), qPrintable(refused[1]));
        QVERIFY2(refused[2].startsWith(QStringLiteral("wrongsize.png:")) && refused[2].contains(QStringLiteral(u"尺寸")), qPrintable(refused[2]));
        QVERIFY(QDir(project.output("refused")).isEmpty());

        request.skipUnconvertible = true;
        request.outputDir = project.output("skipped");
        report = service.run(request);
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(report.count(ExportResult::Exported), 1);
        QCOMPARE(report.count(ExportResult::Negative), 0);
        QCOMPARE(report.count(ExportResult::NotDone), 1);  // "marked": not a negative sample
        QCOMPARE(filesUnder(project.output("skipped")),
                 (QStringList{QStringLiteral("classes.txt"), QStringLiteral("good.txt"), QStringLiteral("liteanno-export.json")}));
    }

    // Review of #11, 2: a liteanno folder that cannot be looked into is not a plain folder.
    void projectFolderThatCannotBeReadIsNotPlain()
    {
        QTemporaryDir local;
        QTemporaryDir data;
        const LocalDataPaths paths(local.path());
        QVERIFY(QDir(data.path()).mkdir(QStringLiteral("liteanno")));
        ProjectSession::OpenResult result = ProjectSession::open(data.path(), paths);
        QCOMPARE(openErrorName(result.error), "project-file-unreadable");
        QVERIFY(QDir(data.filePath(QStringLiteral("liteanno"))).removeRecursively());
        QVERIFY(QFile::link(data.filePath(QStringLiteral("gone")), data.filePath(QStringLiteral("liteanno"))));
        result = ProjectSession::open(data.path(), paths);
        QCOMPARE(openErrorName(result.error), "project-file-unreadable");
#ifndef Q_OS_WIN
        QFile::remove(data.filePath(QStringLiteral("liteanno")));
        writeFile(data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", "classes": [{"id": 0, "name": "scratch"}]})");
        QVERIFY(ProjectSession::open(data.path(), paths).session);
        QVERIFY(QFile::setPermissions(data.filePath(QStringLiteral("liteanno")), QFileDevice::Permissions()));
        const bool readable = QFileInfo::exists(data.filePath(QStringLiteral("liteanno/project.json")));
        result = ProjectSession::open(data.path(), paths);
        QFile::setPermissions(data.filePath(QStringLiteral("liteanno")),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        if (readable)
            QSKIP("this user reads folders without permission (root)");
        QCOMPARE(openErrorName(result.error), "project-file-unreadable");
#endif
    }

    // Review of #11, 3 and 4: init takes the folder's lock, and leaves a folder whose edits
    // still wait in local recovery records alone.
    void initKeepsToTheFolder()
    {
        QTemporaryDir data;
        QTemporaryDir local;
        const QString projectFile = data.filePath(QStringLiteral("liteanno/project.json"));
        const auto init = [&] {
            QProcess process;
            process.start(QStringLiteral(LITEANNO_CLI), {QStringLiteral("init"), data.path(), QStringLiteral("--classes"),
                                                         QStringLiteral("scratch"), QStringLiteral("--local-data"), local.path()});
            if (!process.waitForFinished(60000))
                qFatal("liteanno-cli did not finish");
            return process.exitCode();
        };
        const LocalDataPaths paths(local.path());
        {
            ProjectSession::OpenResult open = ProjectSession::open(data.path(), paths);
            QVERIFY(open.session && open.session->isPlainFolder());
            QVERIFY(init() != 0);  // open in the window
            QVERIFY(!QFileInfo::exists(projectFile));

            const ProjectLocation& location = open.session->location();
            formats::RecoveryRecord record;
            record.formatVersion = QStringLiteral("1.0");
            record.projectId = location.projectId;
            record.bindingId = location.bindingId;
            record.rootPathAtWrite = location.rootPath;
            record.imagePath = QStringLiteral("0001.png");
            record.documentVersion = 1;
            record.createdAt = QStringLiteral("2026-10-23T10:00:00+08:00");
            record.annotation = R"({"version": "4.2.0", "flags": {}, "shapes": [], "imagePath": "0001.png", )"
                                R"("imageData": null, "imageHeight": 100, "imageWidth": 200})";
            const QString dir = paths.recoveryDir(location.projectId, location.bindingId);
            QVERIFY(QDir().mkpath(dir));
            writeFile(dir + QStringLiteral("/0001.json"), *formats::recoveryRecordToBytes(record));
            QCOMPARE(RecoveryService(paths, location).scan().candidates.size(), 1);
        }
        QVERIFY(init() != 0);  // the edit would no longer be offered
        QVERIFY(!QFileInfo::exists(projectFile));

        {
            ProjectSession::OpenResult open = ProjectSession::open(data.path(), paths);
            QVERIFY(QDir(paths.recoveryDir(open.session->location().projectId, open.session->location().bindingId))
                        .removeRecursively());
        }
        QCOMPARE(init(), 0);
        QVERIFY(QFileInfo::exists(projectFile));
    }

    // Review of #11, round 2: a record folder that cannot be listed is not an empty one, and
    // a record that cannot be used holds init back until it is moved aside.
    void initWaitsForRecordsItCannotSee()
    {
        QTemporaryDir data;
        QTemporaryDir local;
        const QString projectFile = data.filePath(QStringLiteral("liteanno/project.json"));
        const auto init = [&] {
            QProcess process;
            process.start(QStringLiteral(LITEANNO_CLI),
                          {QStringLiteral("init"), data.path(), QStringLiteral("--local-data"), local.path()});
            if (!process.waitForFinished(60000))
                qFatal("liteanno-cli did not finish");
            return process.exitCode();
        };
        const LocalDataPaths paths(local.path());
        ProjectLocation location;
        {
            ProjectSession::OpenResult open = ProjectSession::open(data.path(), paths);
            QVERIFY(open.session);
            location = open.session->location();
        }
        const QString dir = paths.recoveryDir(location.projectId, location.bindingId);
        QVERIFY(QDir().mkpath(QFileInfo(dir).absolutePath()));

        // Not a folder: cannot be listed.
        writeFile(dir, "not a folder");
        QVERIFY(!RecoveryService(paths, location).scan().error.isEmpty());
        QVERIFY(init() != 0);
        QVERIFY(!QFileInfo::exists(projectFile));
        QVERIFY(QFile::remove(dir));

        // A damaged record: init refuses until it is moved aside, kept as it is.
        QVERIFY(QDir().mkpath(dir));
        writeFile(dir + QStringLiteral("/0001.json"), "{\"formatVersion\": \"1.0\", \"proj");
        QCOMPARE(RecoveryService(paths, location).scan().unusable.size(), 1);
        QVERIFY(init() != 0);
        QVERIFY(moveRecordToUnusable(dir + QStringLiteral("/0001.json")));
        const RecoveryScan after = RecoveryService(paths, location).scan();
        QVERIFY(after.candidates.isEmpty() && after.unusable.isEmpty() && after.error.isEmpty());
        QCOMPARE(QDir(dir + QStringLiteral("/unusable")).entryList(QDir::Files).size(), 1);

#ifndef Q_OS_WIN
        // A folder this user may not read (as the review found it).
        QVERIFY(QFile::setPermissions(dir, QFileDevice::Permissions()));
        const bool readable = QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size() > 0;
        const int refused = readable ? 1 : init();
        const bool created = QFileInfo::exists(projectFile);
        QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        if (!readable) {
            QVERIFY(refused != 0);
            QVERIFY(!created);
        }
#endif
        QCOMPARE(init(), 0);
        QVERIFY(QFileInfo::exists(projectFile));
#ifndef Q_OS_WIN
        if (readable)
            QSKIP("this user reads folders without permission (root)");
#endif
    }

    void classesFileRules()
    {
        QCOMPARE(formats::parseClassesFile("a\nb c\n").names, (QStringList{QStringLiteral("a"), QStringLiteral("b c")}));
        QCOMPARE(formats::parseClassesFile("a\rb").names, (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(formats::parseClassesFile("\xEF\xBB\xBF\xE5\x88\x92\xE7\x97\x95\n\n\n").names, QStringList{QStringLiteral(u"划痕")});
        QVERIFY(formats::parseClassesFile("a\n\nb\n").error.contains(QStringLiteral(u"第 2 行")));
        QVERIFY(formats::parseClassesFile("a\nb\na\n").error.contains(QStringLiteral(u"第 3 行")));
        QVERIFY(!formats::parseClassesFile("\n\n").error.isEmpty());
        QVERIFY(!formats::parseClassesFile("\xFF\xFE").error.isEmpty());
        // Review of #11, 6: bytes cut off at the end are not dropped quietly.
        QVERIFY(!formats::parseClassesFile("scratch\xC3").error.isEmpty());
        QVERIFY(!formats::parseClassesFile("scratch\n\xE5\x88").error.isEmpty());
        QVERIFY(!formats::readClassesFile(QStringLiteral("/nonexistent/classes.txt")).error.isEmpty());
    }

    // Field 2026-10-28: a classes.txt of "0: Dirty" lines, as copied from the names of a
    // data.yaml, gives the names, by their numbers.
    void numberedClassLines()
    {
        const QStringList expected = {QStringLiteral("Dirty"), QStringLiteral("Scratch"), QStringLiteral("Crack"), QStringLiteral("Solder")};
        QCOMPARE(formats::parseClassesFile("0: Dirty\n1: Scratch\n2: Crack\n3: Solder\n").names, expected);
        QCOMPARE(formats::parseClassesFile("0: Dirty\r\n1: Scratch\r\n2: Crack\r\n3: Solder").names, expected);
        QCOMPARE(formats::parseClassesFile("2: Crack\n0: Dirty\n3: Solder\n1:   Scratch  \n").names, expected);
        QCOMPARE(formats::parseClassesFile(u8"0: 划痕\n1: '0: 脏污'\n").names, (QStringList{QStringLiteral(u"划痕"), QStringLiteral(u"0: 脏污")}));
        // Not numbered: names that only hold a colon or digits stay as they are.
        QCOMPARE(formats::parseClassesFile("12:30\na:b\n7\n").names,
                 (QStringList{QStringLiteral("12:30"), QStringLiteral("a:b"), QStringLiteral("7")}));
        // Refused rather than guessed.
        QVERIFY(formats::parseClassesFile("0: Dirty\nScratch\n").error.contains(QStringLiteral(u"第 2 行")));
        QVERIFY(formats::parseClassesFile("0: Dirty\n2: Crack\n").error.contains(QStringLiteral(u"缺少编号 1")));
        QVERIFY(formats::parseClassesFile("0: Dirty\n0: Crack\n").error.contains(QStringLiteral(u"编号 0 写了两次")));
        QVERIFY(formats::parseClassesFile("0: Dirty\n1: Dirty\n").error.contains(QStringLiteral("Dirty")));
        QVERIFY(formats::parseClassesFile("0: Dirty\n1:\n").error.contains(QStringLiteral(u"第 2 行")));
    }

    // A data.yaml chosen as the class file: its names.
    void datasetYamlAsClassFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("data.YAML"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("path: ../datasets/aoi\ntrain: images/train\nval: images/val\nnc: 2\nnames:\n  0: Dirty\n  1: Scratch\n");
        file.close();
        QCOMPARE(formats::readClassesFile(path).names, (QStringList{QStringLiteral("Dirty"), QStringLiteral("Scratch")}));
    }

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

    // YOLO OBB and DOTA write corners as they are; a rotated box with a corner outside the
    // image is not left out silently, as upstream does, but stops the export.
    void rotatedBoxesOutsideTheImage()
    {
        Project project;
        writeFile(project.data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", "shapeTypes": ["rotation"], )"
                  R"("classes": [{"id": 0, "name": "scratch"}]})");
        const QByteArray inside = R"({"label": "scratch", "points": [[10, 10], [50, 10], [50, 40], [10, 40]], )"
                                  R"("shape_type": "rotation", "direction": 0})";
        const QByteArray outside = R"({"label": "scratch", "points": [[-2.5, 10], [50, 8], [52, 40], [-0.5, 42]], )"
                                   R"("shape_type": "rotation", "direction": 0.04})";
        project.image(QStringLiteral("a.png"), inside, true);
        project.image(QStringLiteral("b.png"), outside, true);
        const auto session = project.open();
        const ExportService service(*session, imaging::readImageInfo);
        for (const formats::ExportFormat format : {formats::ExportFormat::YoloObb, formats::ExportFormat::Dota}) {
            ExportRequest request;
            request.format = format;
            request.outputDir = project.output(format == formats::ExportFormat::Dota ? "dota" : "obb");
            ExportReport report = service.run(request, {});
            QVERIFY(!report.written);
            QCOMPARE(report.unconvertible().size(), 1);
            QVERIFY(report.unconvertible()[0].reason.contains(QStringLiteral(u"图片外")));
            QVERIFY(!QFileInfo::exists(request.outputDir));
            request.skipUnconvertible = true;
            report = service.run(request, {});
            QVERIFY(report.written);
            QVERIFY(QFileInfo::exists(request.outputDir + QStringLiteral("/a.txt")));
            QVERIFY(!QFileInfo::exists(request.outputDir + QStringLiteral("/b.txt")));
        }
        QCOMPARE(readFile(project.output("dota") + QStringLiteral("/a.txt")), QByteArray("10 10 50 10 50 40 10 40 scratch 0\n"));
    }

    // Review of PR #10 (second round), item 2: DOTA separates fields with spaces, so a class
    // name with a space cannot be written; such images are unconvertible, not changed silently.
    void dotaClassNameWithASpace()
    {
        Project project;
        writeFile(project.data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", "shapeTypes": ["rotation"], )"
                  R"("classes": [{"id": 0, "name": "scratch heavy"}]})");
        project.image(QStringLiteral("a.png"),
                      R"({"label": "scratch heavy", "points": [[10, 10], [50, 10], [50, 40], [10, 40]], "shape_type": "rotation", "direction": 0})",
                      true);
        const auto session = project.open();
        ExportRequest request;
        request.format = formats::ExportFormat::Dota;
        request.outputDir = project.output();
        const ExportReport report = ExportService(*session, imaging::readImageInfo).run(request, {});
        QVERIFY(!report.written);
        QCOMPARE(report.unconvertible().size(), 1);
        QVERIFY2(report.unconvertible()[0].reason.contains(QStringLiteral(u"空白")), qPrintable(report.unconvertible()[0].reason));
        QVERIFY(!QFileInfo::exists(request.outputDir));
        // YOLO OBB writes class ids: the same image is fine there.
        request.format = formats::ExportFormat::YoloObb;
        QVERIFY(ExportService(*session, imaging::readImageInfo).run(request, {}).written);
    }

    // Review of PR #10 (second and third round): a mask has the image's name, so masks go to
    // masks/ and copied images to images/; a data folder with its own images/ folder cannot
    // make the two meet.
    void maskWithCopiedImages()
    {
        Project project;
        project.image(QStringLiteral("a.png"), polygon("scratch"), true);
        project.image(QStringLiteral("images/a.png"), polygon("dent"), true);
        const auto session = project.open();
        ExportRequest request;
        request.format = formats::ExportFormat::Mask;
        request.copyImages = true;
        request.outputDir = project.output();
        const ExportReport report = ExportService(*session, imaging::readImageInfo).run(request, {});
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(filesUnder(request.outputDir),
                 (QStringList{QStringLiteral("images/a.png"), QStringLiteral("images/images/a.png"), QStringLiteral("liteanno-export.json"),
                              QStringLiteral("mask_colors.json"), QStringLiteral("masks/a.png"), QStringLiteral("masks/images/a.png")}));
        for (const char* image : {"a.png", "images/a.png"}) {
            QCOMPARE(readFile(request.outputDir + QStringLiteral("/images/") + QLatin1String(image)),
                     readFile(project.data.filePath(QLatin1String(image))));
            QCOMPARE(QImage(request.outputDir + QStringLiteral("/masks/") + QLatin1String(image)).format(), QImage::Format_Grayscale8);
        }
    }

    // Review of PR #10 (second round), item 4: integer coordinates are written as they are,
    // also ones too large for a double to hold exactly.
    void labelMeKeepsLargeIntegers()
    {
        Project project;
        project.image(QStringLiteral("a.png"),
                      R"({"label": "scratch", "points": [[9223372036854775807, 10], [-9223372036854775807, 10], [30, 40]], )"
                      R"("shape_type": "polygon"})",
                      true);
        const auto session = project.open();
        ExportRequest request;
        request.format = formats::ExportFormat::LabelMe;
        request.outputDir = project.output();
        const ExportReport report = ExportService(*session, imaging::readImageInfo).run(request, {});
        QVERIFY2(report.written, qPrintable(report.error));
        const QByteArray json = readFile(request.outputDir + QStringLiteral("/a.json"));
        QVERIFY2(json.contains("          9223372036854775807,\n"), json.constData());
        QVERIFY2(json.contains("-9223372036854775807,"), json.constData());
        QVERIFY(!json.contains("-9223372036854775808"));
    }

    // Image classification: single choice copies each image into its label's folder; multiple
    // choice writes labels.csv.
    void classification()
    {
        Project project;
        const QByteArray config = R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", )"
                                  R"("imageLabels": {"mode": "%1", "options": ["OK", "NG", "划伤,轻"]}})";
        writeFile(project.data.filePath(QStringLiteral("liteanno/project.json")), QByteArray(config).replace("%1", "single"));
        const auto labelled = [&project](const char* path, const QByteArray& flags, bool checked) {
            project.image(QString::fromLatin1(path), QByteArray(), checked);
            const QString json = project.data.filePath(QString::fromLatin1(path).replace(QStringLiteral(".png"), QStringLiteral(".json")));
            writeFile(json, readFile(json).replace(R"("flags": {})", R"("flags": )" + flags));
        };
        labelled("line1/a.png", R"({"OK": true, "NG": false, "划伤,轻": false})", true);
        labelled("line1/b.png", R"({"OK": false, "NG": true, "划伤,轻": false})", true);
        labelled("line2/a.png", R"({"OK": false, "NG": true, "划伤,轻": false})", true);
        labelled("line2/c.png", R"({"OK": true, "NG": false, "划伤,轻": false})", false);
        {
            const auto session = project.open();
            ExportRequest request;
            request.format = formats::ExportFormat::Classification;
            request.outputDir = project.output("single");
            const ExportReport report = ExportService(*session, imaging::readImageInfo).run(request, {});
            QVERIFY2(report.written, qPrintable(report.error));
            const QStringList expected = {QStringLiteral("NG/line1/b.png"), QStringLiteral("NG/line2/a.png"),
                                          QStringLiteral("OK/line1/a.png"), QStringLiteral("liteanno-export.json")};
            QCOMPARE(filesUnder(request.outputDir), expected);
        }

        writeFile(project.data.filePath(QStringLiteral("liteanno/project.json")), QByteArray(config).replace("%1", "multi"));
        labelled("line1/a.png", R"({"OK": true, "NG": false, "划伤,轻": true})", true);
        const auto session = project.open();
        ExportRequest request;
        request.format = formats::ExportFormat::Classification;
        request.outputDir = project.output("multi");
        const ExportReport report = ExportService(*session, imaging::readImageInfo).run(request, {});
        QVERIFY2(report.written, qPrintable(report.error));
        QCOMPARE(readFile(request.outputDir + QStringLiteral("/labels.csv")),
                 QStringLiteral(u"\uFEFFimage,OK,NG,\"划伤,轻\"\nline1/a.png,1,0,1\nline1/b.png,0,1,0\nline2/a.png,0,1,0\n").toUtf8());
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

        // "checked" means a person looked at the image: there is no bulk confirm.
        const QByteArray before = readFile(data.filePath(QStringLiteral("a.json")));
        cli({QStringLiteral("confirm"), data.path()}, 2);
        QCOMPARE(readFile(data.filePath(QStringLiteral("a.json"))), before);
        // Confirmed in the window.
        writeFile(data.filePath(QStringLiteral("a.json")), QByteArray(before).replace("\"checked\": false", "\"checked\": true"));

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
