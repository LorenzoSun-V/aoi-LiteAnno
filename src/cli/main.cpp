// liteanno-cli (architecture 13): init, status, confirm and export for engineers. It takes
// the same project lock as the window, so it never changes a project open there.

#include "application/DocumentSession.h"
#include "application/ExportService.h"
#include "application/LocalDataPaths.h"
#include "application/ProjectSession.h"
#include "application/RecoveryService.h"
#include "application/WorksetStore.h"
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
#include <QSet>
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
    QDir().mkpath(QFileInfo(file).absolutePath());
    // Format spec 1: written whole or not at all, so a failed init leaves nothing to clear up;
    // and never in place of a project file, also one another program makes meanwhile.
    const formats::SaveResult written = formats::createFileExclusively(file, bytes);
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

int confirm(const QCommandLineParser& parser, const Common& common)
{
    const std::unique_ptr<ProjectSession> project = openProject(common);
    if (!project)
        return Failed;
    const bool dryRun = parser.isSet(QStringLiteral("dry-run"));
    const application::ExportScope scope = application::scopeOf(*project, common.batches);
    if (!scope.error.isEmpty()) {
        printError(scope.error);
        return Failed;
    }
    // Images with unsaved edits of the window in a recovery record are left alone: writing
    // them here would make the record look outdated.
    QSet<QString> recorded;
    const application::RecoveryService recovery(project->paths(), project->location());
    for (const application::RecoveryCandidate& candidate : recovery.scan().candidates)
        recorded.insert(candidate.record.imagePath);

    int confirmed = 0;
    int blocked = 0;
    int failed = 0;
    for (const application::ScopeImage& scoped : scope.images) {
        application::ImageOnDisk image = application::readImageOnDisk(*project, scoped.path, scoped.missing,
                                                                      scoped.nameCollision, imaging::readImageInfo);
        if (image.status != AnnotationStatus::PendingConfirmation)
            continue;
        if (recorded.contains(scoped.path)) {
            print(QStringLiteral(u"跳过 %1：窗口里有未写入的修改").arg(scoped.path));
            ++blocked;
            continue;
        }
        application::DocumentSession session(scoped.path, std::move(*image.annotation), project->config(),
                                             scoped.nameCollision);
        if (!session.confirm()) {
            ++blocked;
            continue;
        }
        if (dryRun) {
            ++confirmed;
            continue;
        }
        formats::LoadedAnnotation edited = session.copy();
        const QString imageFile = project->imageFilePath(scoped.path);
        formats::SaveOptions options;
        options.mayWrite = [imageFile] {
            formats::SaveResult result;
            if (dataset::hasNameCollision(imageFile)) {
                result.status = formats::SaveStatus::ReadOnly;
                result.error = QStringLiteral(u"同一目录下有同名图片");
            } else {
                result.status = formats::SaveStatus::Saved;
            }
            return result;
        };
        const formats::SaveResult saved =
            formats::saveAnnotationFile(project->annotationFilePath(scoped.path), edited, options);
        if (saved.status == formats::SaveStatus::Saved) {
            ++confirmed;
        } else {
            ++failed;
            printError(QStringLiteral(u"没有写入 %1：%2").arg(scoped.path, saved.error.isEmpty()
                                                                             ? QString::fromLatin1(formats::saveStatusName(saved.status))
                                                                             : saved.error));
        }
    }
    print(QStringLiteral(u"%1 %2 张；不满足确认条件或不能写入的 %3 张；失败 %4 张。")
              .arg(dryRun ? QStringLiteral(u"可以确认") : QStringLiteral(u"已确认"))
              .arg(confirmed)
              .arg(blocked)
              .arg(failed));
    return failed > 0 ? Failed : Ok;
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
        u"  confirm <数据文件夹>   把满足确认条件的待确认图片标为已确认\n"
        u"  export <数据文件夹>    导出已确认的图片（没有项目文件的文件夹：导出有标注的图片）"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("init | status | confirm | export"));
    parser.addPositionalArgument(QStringLiteral("root"), QStringLiteral(u"数据文件夹（项目根目录）"));
    parser.addOptions({
        {QStringLiteral("batch"), QStringLiteral(u"只处理这个批次（如 b000003），可以写多次"), QStringLiteral("batch")},
        {QStringLiteral("name"), QStringLiteral(u"init：项目名称"), QStringLiteral("name")},
        {QStringLiteral("classes"), QStringLiteral(u"init：类别，用逗号分隔，编号从 0 开始"), QStringLiteral("names")},
        {QStringLiteral("shape-types"), QStringLiteral(u"init：形状类型，如 rectangle,polygon"), QStringLiteral("types")},
        {QStringLiteral("batch-size"), QStringLiteral(u"init：每批图片数，默认 1000"), QStringLiteral("count")},
        {QStringLiteral("dry-run"), QStringLiteral(u"confirm：只统计，不写入")},
        {QStringLiteral("format"), QStringLiteral(u"export：yolo-det | yolo-seg | yolo-obb | voc | voc-polygon | coco-det | coco-seg | dota | mask | labelme | classify"),
         QStringLiteral("format")},
        {QStringLiteral("out"), QStringLiteral(u"export：导出文件夹，必须是新的或空的"), QStringLiteral("folder")},
        {QStringLiteral("skip-unconvertible"), QStringLiteral(u"export：跳过有无法转换对象的图片，继续导出")},
        {QStringLiteral("copy-images"), QStringLiteral(u"export：同时复制图片")},
        {QStringLiteral("classes-file"), QStringLiteral(u"export：类别文件 classes.txt，每行一个类别，第一行编号 0"), QStringLiteral("file")},
        {QStringLiteral("local-data"), QStringLiteral(u"本机数据文件夹（测试用）"), QStringLiteral("folder")},
    });
    parser.process(app);
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
    if (command == QLatin1String("confirm"))
        return confirm(parser, common);
    if (command == QLatin1String("export"))
        return exportProject(parser, common);
    printError(QStringLiteral(u"不认识的命令：%1").arg(command));
    return Usage;
}
