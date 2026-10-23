// liteanno-cli (architecture 13): init, status, export and import for engineers. It takes the
// same project lock as the window, so it never changes a project open there. There is no bulk
// confirm: "checked" means a person looked at the image (architecture 4).

#include "application/ExportService.h"
#include "application/ImportService.h"
#include "application/LocalDataPaths.h"
#include "application/ProjectSession.h"
#include "application/RecoveryService.h"
#include "application/WorksetStore.h"
#include "core/BundledLicenses.h"
#include "core/FormatVersion.h"
#include "core/Version.h"
#include "dataset/DirectoryScanner.h"
#include "formats/AnnotationStore.h"
#include "formats/ProjectFile.h"
#include "imaging/ImageLoader.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <cstdio>
#include <map>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace liteanno;
using application::ProjectSession;

namespace {

enum ExitCode { Ok = 0, Failed = 1, Usage = 2 };

void print(const QString& text)
{
    const QByteArray bytes = (text + QLatin1Char('\n')).toUtf8();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

void printError(const QString& text)
{
    const QByteArray bytes = (text + QLatin1Char('\n')).toUtf8();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stderr);
    std::fflush(stderr);
}

QString openErrorText(const ProjectSession::OpenResult& result)
{
    switch (result.error) {
    case ProjectSession::OpenError::None:
        return QString();
    case ProjectSession::OpenError::ProjectFileUnreadable:
        return QStringLiteral(u"有 liteanno 文件夹，但项目文件 liteanno/project.json 不存在或无法读取（%1）。").arg(result.message);
    case ProjectSession::OpenError::InvalidProjectFile:
        return QStringLiteral(u"项目文件有错误：%1（%2）").arg(result.field, result.message);
    case ProjectSession::OpenError::LocationUnknown:
        return QStringLiteral(u"无法识别项目所在的磁盘：%1").arg(result.message);
    case ProjectSession::OpenError::Locked:
        return QStringLiteral(u"项目已经在别处打开（进程 %1，%2），请先关闭。").arg(result.holder.pid).arg(result.holder.hostName);
    case ProjectSession::OpenError::LockFailed:
        return QStringLiteral(u"无法创建项目锁文件：%1").arg(result.message);
    }
    return QString();
}

struct Common {
    QString root;
    QStringList batches;
    application::LocalDataPaths localData = application::LocalDataPaths::standard();
};

std::unique_ptr<ProjectSession> openProject(const Common& common)
{
    ProjectSession::OpenResult result = ProjectSession::open(common.root, common.localData);
    if (!result.session)
        printError(openErrorText(result));
    return std::move(result.session);
}

int init(const QCommandLineParser& parser, const Common& common)
{
    const QString file = common.root + QStringLiteral("/liteanno/project.json");
    if (!QFileInfo(common.root).isDir()) {
        printError(QStringLiteral(u"文件夹 %1 不存在。").arg(common.root));
        return Failed;
    }
    if (QFileInfo::exists(file)) {
        printError(QStringLiteral(u"%1 已经有项目文件，没有改动。").arg(QDir::toNativeSeparators(file)));
        return Failed;
    }
    // The folder as a plain one, under the lock the window takes (format spec 8): never while
    // it is open there, and not while edits the window kept for it wait in local recovery
    // records, which the project's new id would no longer find (13.6).
    const std::unique_ptr<ProjectSession> folder = openProject(common);
    if (!folder)
        return Failed;
    if (!folder->isPlainFolder()) {
        printError(QStringLiteral(u"%1 已经有项目文件，没有改动。").arg(QDir::toNativeSeparators(file)));
        return Failed;
    }
    const application::RecoveryScan records = application::RecoveryService(folder->paths(), folder->location()).scan();
    if (!records.error.isEmpty()) {
        printError(QStringLiteral(u"无法读取这个文件夹在本机的恢复记录（%1：%2），不能确认没有未写入的编辑，没有创建项目文件。")
                       .arg(QDir::toNativeSeparators(folder->paths().recoveryDir(folder->location().projectId,
                                                                                 folder->location().bindingId)),
                            records.error));
        return Failed;
    }
    if (!records.candidates.isEmpty() || !records.unusable.isEmpty()) {
        printError(QStringLiteral(u"这个文件夹在本机还有 %1 份没有写入标注文件的编辑。请先用窗口打开这个文件夹，"
                                  u"在“文件 > 处理未写入的编辑”中处理完，再创建项目文件。")
                       .arg(records.candidates.size() + records.unusable.size()));
        return Failed;
    }
    ProjectConfig config;
    config.formatVersion = QString::fromLatin1(kFormatVersion);
    config.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    config.name = parser.value(QStringLiteral("name"));
    const QDateTime now = QDateTime::currentDateTime();
    config.createdAt = application::WorksetStore::timestamp(now);
    for (const QString& name : parser.value(QStringLiteral("shape-types")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const std::optional<ShapeType> type = shapeTypeFromName(name.trimmed());
        if (!type || !isEditableShapeType(*type)) {
            printError(QStringLiteral(u"不认识的形状类型：%1").arg(name));
            return Usage;
        }
        config.shapeTypes.append(*type);
    }
    int id = 0;
    for (const QString& name : parser.value(QStringLiteral("classes")).split(QLatin1Char(','), Qt::SkipEmptyParts))
        config.classes.append({id++, name.trimmed(), QString()});
    if (parser.isSet(QStringLiteral("batch-size"))) {
        bool ok = false;
        config.batchSize = parser.value(QStringLiteral("batch-size")).toInt(&ok);
        if (!ok || config.batchSize < 1) {
            printError(QStringLiteral(u"--batch-size 必须是正整数。"));
            return Usage;
        }
    }
    const QByteArray bytes = formats::projectFileToBytes(config);
    // Read back with the same rules the window uses, so a bad class list is caught here.
    const formats::ProjectFileResult check = formats::parseProjectFile(bytes);
    if (!check.config) {
        printError(QStringLiteral(u"项目设置有错误：%1（%2）").arg(check.field, check.error));
        return Usage;
    }
    const QString liteannoFolder = QFileInfo(file).absolutePath();
    const bool newFolder = !QFileInfo::exists(liteannoFolder);
    QDir().mkpath(liteannoFolder);
    // Format spec 1: written whole or not at all, so a failed init leaves nothing to clear up
    // (a liteanno folder without a project file would not open again, 13.6); and never in
    // place of a project file, also one another program makes meanwhile.
    const formats::SaveResult written = formats::createFileExclusively(file, bytes);
    if (written.status != formats::SaveStatus::Saved && newFolder)
        QDir().rmdir(liteannoFolder);  // only when still empty
    if (written.status == formats::SaveStatus::ExternallyModified) {
        printError(QStringLiteral(u"%1 已存在，没有改动。").arg(QDir::toNativeSeparators(file)));
        return Failed;
    }
    if (written.status != formats::SaveStatus::Saved) {
        printError(QStringLiteral(u"无法写入 %1：%2").arg(QDir::toNativeSeparators(file), written.error));
        return Failed;
    }
    print(QStringLiteral(u"已创建 %1").arg(QDir::toNativeSeparators(file)));
    return Ok;
}

int status(const Common& common)
{
    const std::unique_ptr<ProjectSession> project = openProject(common);
    if (!project)
        return Failed;
    const application::ExportScope scope = application::scopeOf(*project, common.batches);
    if (!scope.error.isEmpty()) {
        printError(scope.error);
        return Failed;
    }
    std::map<int, int> counts;
    int done = 0;
    for (const application::ScopeImage& scoped : scope.images) {
        const application::ImageOnDisk image = application::readImageOnDisk(*project, scoped.path, scoped.missing,
                                                                            scoped.nameCollision, imaging::readImageInfo);
        ++counts[int(image.status)];
        if (countsAsDone(image.status))
            ++done;
    }
    print(QStringLiteral(u"图片 %1 张，已完成 %2 张%3").arg(scope.images.size()).arg(done).arg(
        scope.scanComplete ? QString() : QStringLiteral(u"（有文件夹无法读取，扫描不完整）")));
    for (const auto& [status, count] : counts)
        print(QStringLiteral(u"  %1：%2").arg(annotationStatusText(AnnotationStatus(status))).arg(count));
    return Ok;
}

int exportProject(const QCommandLineParser& parser, const Common& common)
{
    const std::optional<formats::ExportFormat> format = formats::exportFormatFromName(parser.value(QStringLiteral("format")));
    if (!format) {
        QStringList names;
        for (formats::ExportFormat f : formats::exportFormats())
            names.append(formats::exportFormatName(f));
        printError(QStringLiteral(u"--format 必须是 %1 之一。").arg(names.join(QStringLiteral(", "))));
        return Usage;
    }
    if (!parser.isSet(QStringLiteral("out"))) {
        printError(QStringLiteral(u"请用 --out 指定导出文件夹。"));
        return Usage;
    }
    const std::unique_ptr<ProjectSession> project = openProject(common);
    if (!project)
        return Failed;
    application::ExportRequest request;
    request.format = *format;
    request.outputDir = parser.value(QStringLiteral("out"));
    request.batches = common.batches;
    request.skipUnconvertible = parser.isSet(QStringLiteral("skip-unconvertible"));
    request.copyImages = parser.isSet(QStringLiteral("copy-images"));
    request.classesFile = parser.value(QStringLiteral("classes-file"));
    request.poseConfigFile = parser.value(QStringLiteral("pose-config"));
    const application::ExportService service(*project, imaging::readImageInfo);
    const application::ExportReport report = service.run(request);
    for (const application::ExportItem& item : report.unconvertible())
        printError(QStringLiteral(u"  %1：%2").arg(item.image, item.reason));
    if (!report.written) {
        printError(report.error);
        if (!report.unconvertible().isEmpty() && !request.skipUnconvertible)
            printError(QStringLiteral(u"加 --skip-unconvertible 可以跳过这些图片继续导出。"));
        return Failed;
    }
    print(QStringLiteral(u"已导出 %1 张（其中无目标 %2 张）；跳过 %3 张，未完成 %4 张，无法转换 %5 张。\n导出清单：%6")
              .arg(report.count(application::ExportResult::Exported) + report.count(application::ExportResult::Negative))
              .arg(report.count(application::ExportResult::Negative))
              .arg(report.count(application::ExportResult::Skipped))
              .arg(report.count(application::ExportResult::NotDone))
              .arg(report.count(application::ExportResult::Unconvertible))
              .arg(QDir::toNativeSeparators(report.manifestPath)));
    return Ok;
}

int importLabels(const QCommandLineParser& parser, const Common& common)
{
    const std::optional<formats::ImportFormat> format = formats::importFormatFromName(parser.value(QStringLiteral("format")));
    if (!format) {
        QStringList names;
        for (formats::ImportFormat f : formats::importFormats())
            names.append(formats::importFormatName(f));
        printError(QStringLiteral(u"导入的 --format 必须是 %1 之一。").arg(names.join(QStringLiteral(", "))));
        return Usage;
    }
    if (!parser.isSet(QStringLiteral("labels"))) {
        printError(QStringLiteral(u"请用 --labels 指定标注文件夹（COCO：标注文件）。"));
        return Usage;
    }
    const std::optional<application::ImportMode> mode =
        application::importModeFromName(parser.value(QStringLiteral("mode")).isEmpty() ? QStringLiteral("new")
                                                                                        : parser.value(QStringLiteral("mode")));
    if (!mode) {
        printError(QStringLiteral(u"--mode 必须是 new、append 或 replace 之一。"));
        return Usage;
    }
    const std::unique_ptr<ProjectSession> project = openProject(common);
    if (!project)
        return Failed;
    application::ImportRequest request;
    request.format = *format;
    if (formats::importReadsOneFile(*format))
        request.labelFile = parser.value(QStringLiteral("labels"));
    else
        request.labelDir = parser.value(QStringLiteral("labels"));
    request.classesFile = parser.value(QStringLiteral("classes-file"));
    request.mappingFile = parser.value(QStringLiteral("mapping"));
    request.poseConfigFile = parser.value(QStringLiteral("pose-config"));
    request.batches = common.batches;
    request.mode = *mode;
    request.skipUnconvertible = parser.isSet(QStringLiteral("skip-unconvertible"));
    const application::ImportService service(*project, imaging::readImageInfo, imaging::readMaskImage);
    const application::ImportReport report = service.run(request);
    for (const application::ImportItem& item : report.unconvertible())
        printError(QStringLiteral(u"  %1：%2").arg(item.image, item.reason));
    for (const application::UnmatchedLabel& label : report.unmatched)
        printError(QStringLiteral(u"  COCO 图片 %1 没有导入：%2").arg(label.fileName, label.reason));
    if (!report.error.isEmpty()) {
        printError(report.error);
        if (!report.unconvertible().isEmpty() && !request.skipUnconvertible)
            printError(QStringLiteral(u"加 --skip-unconvertible 可以跳过这些图片继续导入。"));
        return Failed;
    }
    for (const application::ImportItem& item : report.items) {
        if (item.result == application::ImportResult::NotWritten)
            printError(QStringLiteral(u"没有写入 %1：%2").arg(item.image, item.reason));
    }
    int empty = 0;
    for (const application::ImportItem& item : report.items)
        empty += item.result == application::ImportResult::Imported && item.shapes == 0 ? 1 : 0;
    print(QStringLiteral(u"已导入 %1 张（其中标注文件里没有对象的 %6 张）；已有标注而未改动 %2 张，没有标注文件 %3 张，没有写入 %4 张，无法转换 %5 张，"
                         u"标注文件里没有对象而不用写入 %7 张。")
              .arg(report.count(application::ImportResult::Imported))
              .arg(report.count(application::ImportResult::HasAnnotation))
              .arg(report.count(application::ImportResult::NoLabelFile))
              .arg(report.count(application::ImportResult::NotWritten))
              .arg(report.count(application::ImportResult::Unconvertible))
              .arg(empty)
              .arg(report.count(application::ImportResult::Unchanged)));
    if (!report.unmatched.isEmpty())
        print(QStringLiteral(u"COCO 文件里有 %1 张图片在文件夹里找不到，见上面的列表。").arg(report.unmatched.size()));
    return report.count(application::ImportResult::NotWritten) > 0 ? Failed : Ok;
}

// LGPL 3.0, section 4: where the source is, the terms Qt is used under, and how to link the
// program again with another Qt (Help > About says the same in the window).
QString licenseNotice()
{
    return QStringLiteral(
               u"aoi-LiteAnno %1\n"
               u"源码：https://github.com/LorenzoSun-V/aoi-LiteAnno\n"
               u"本程序使用 Qt %2，按 GNU LGPL 3.0 授权使用。单文件版把 Qt 静态链接在内，可以用修改过的 Qt "
               u"重新编译、链接，做法见源码中 README 的“单文件版与 Qt 许可”。Qt 的源码：https://download.qt.io/archive/qt/\n"
               u"另含 nlohmann/json（MIT）、OpenCV 的多边形填充代码（Apache-2.0）、SQLite（公有领域）以及 Qt 自带的第三方库。")
        .arg(QString::fromLatin1(kAppVersion), QString::fromLatin1(qVersion()));
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("liteanno-cli"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(kAppVersion));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        u"aoi-LiteAnno 命令行工具\n\n"
        u"  init <数据文件夹>      创建项目文件 liteanno/project.json\n"
        u"  status <数据文件夹>    统计各状态的图片数量\n"
        u"  export <数据文件夹>    导出已确认的图片（没有项目文件的文件夹：导出有标注的图片）\n"
        u"  import <数据文件夹>    把 --labels 文件夹里的标注导入为每张图片的 JSON"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("init | status | export | import"));
    parser.addPositionalArgument(QStringLiteral("root"), QStringLiteral(u"数据文件夹（项目根目录）"));
    parser.addOptions({
        {QStringLiteral("batch"), QStringLiteral(u"只处理这个批次（如 b000003），可以写多次"), QStringLiteral("batch")},
        {QStringLiteral("name"), QStringLiteral(u"init：项目名称"), QStringLiteral("name")},
        {QStringLiteral("classes"), QStringLiteral(u"init：类别，用逗号分隔，编号从 0 开始"), QStringLiteral("names")},
        {QStringLiteral("shape-types"), QStringLiteral(u"init：形状类型，如 rectangle,polygon"), QStringLiteral("types")},
        {QStringLiteral("batch-size"), QStringLiteral(u"init：每批图片数，默认 1000"), QStringLiteral("count")},
        {QStringLiteral("format"), QStringLiteral(u"export：yolo-det | yolo-seg | yolo-obb | voc | voc-polygon | coco-det | coco-seg | dota | mask | yolo-pose | coco-pose | labelme | classify；"
                        u"import：yolo-det | yolo-seg | yolo-obb | voc | voc-polygon | coco-det | coco-seg | dota | mask | yolo-pose | coco-pose"),
         QStringLiteral("format")},
        {QStringLiteral("out"), QStringLiteral(u"export：导出文件夹，必须是新的或空的"), QStringLiteral("folder")},
        {QStringLiteral("skip-unconvertible"), QStringLiteral(u"export、import：跳过无法转换的图片，继续处理其余图片")},
        {QStringLiteral("labels"), QStringLiteral(u"import：标注文件夹；COCO 是标注文件 .json"), QStringLiteral("path")},
        {QStringLiteral("mapping"), QStringLiteral(u"import mask：掩膜的映射文件，默认标注文件夹或上一级的 mask_colors.json"), QStringLiteral("file")},
        {QStringLiteral("mode"), QStringLiteral(u"import：已有标注的图片 new 不动（默认）、append 追加、replace 替换"), QStringLiteral("mode")},
        {QStringLiteral("copy-images"), QStringLiteral(u"export：同时复制图片")},
        {QStringLiteral("classes-file"), QStringLiteral(u"export、import YOLO：类别文件 classes.txt，每行一个类别，第一行编号 0"), QStringLiteral("file")},
        {QStringLiteral("pose-config"), QStringLiteral(u"export、import 姿态（yolo-pose、coco-pose）：姿态配置文件 .yaml，每个类别的关键点名称"), QStringLiteral("file")},
        {QStringLiteral("local-data"), QStringLiteral(u"本机数据文件夹（测试用）"), QStringLiteral("folder")},
        {QStringLiteral("licenses"), QStringLiteral(u"显示源码地址、Qt 的授权说明和附带的许可证列表")},
        {QStringLiteral("license"), QStringLiteral(u"显示一份许可证全文（名称见 --licenses）"), QStringLiteral("name")},
        {QStringLiteral("check-single-file"), QStringLiteral(u"检查单文件版带齐了许可证和读图格式（打包检查用）")},
    });
    parser.process(app);
    if (parser.isSet(QStringLiteral("licenses"))) {
        print(licenseNotice());
        const QStringList names = bundledLicenseNames();
        if (names.isEmpty())
            print(QStringLiteral(u"这个版本没有附带许可证全文，请见源码仓库。"));
        else
            print(QStringLiteral(u"附带的许可证（用 --license <名称> 显示全文）：\n  %1").arg(names.join(QStringLiteral("\n  "))));
        return Ok;
    }
    if (parser.isSet(QStringLiteral("license"))) {
        const QString text = bundledLicenseText(parser.value(QStringLiteral("license")));
        if (text.isEmpty()) {
            printError(QStringLiteral(u"没有这份许可证，名称见 --licenses。"));
            return Usage;
        }
        print(text);
        return Ok;
    }
    if (parser.isSet(QStringLiteral("check-single-file"))) {
#ifdef QT_STATIC
        const bool staticQt = true;
#else
        const bool staticQt = false;
#endif
        const QStringList missingLicenses = missingRequiredLicenses();
        const QStringList missingFormats = imaging::missingImageFormats();
        if (!staticQt)
            printError(QStringLiteral(u"Qt 不是静态链接的。"));
        if (!missingLicenses.isEmpty())
            printError(QStringLiteral(u"缺少许可证：%1").arg(missingLicenses.join(QLatin1Char(' '))));
        if (!missingFormats.isEmpty())
            printError(QStringLiteral(u"缺少读图格式：%1").arg(missingFormats.join(QLatin1Char(' '))));
        if (!staticQt || !missingLicenses.isEmpty() || !missingFormats.isEmpty())
            return Failed;
        print(QStringLiteral(u"静态 Qt，许可证 %1 份，读图格式齐全。").arg(bundledLicenseNames().size()));
        return Ok;
    }
    const QStringList args = parser.positionalArguments();
    if (args.size() != 2) {
        printError(parser.helpText());
        return Usage;
    }
    Common common;
    common.root = QDir::cleanPath(QFileInfo(args[1]).absoluteFilePath());
    common.batches = parser.values(QStringLiteral("batch"));
    if (parser.isSet(QStringLiteral("local-data")))
        common.localData = application::LocalDataPaths(parser.value(QStringLiteral("local-data")));
    const QString command = args[0];
    if (command == QLatin1String("init"))
        return init(parser, common);
    if (command == QLatin1String("status"))
        return status(common);
    if (command == QLatin1String("export"))
        return exportProject(parser, common);
    if (command == QLatin1String("import"))
        return importLabels(parser, common);
    printError(QStringLiteral(u"不认识的命令：%1").arg(command));
    return Usage;
}
