#include "application/ExportService.h"

#include "application/FileStatus.h"
#include "application/WorksetStore.h"
#include "core/Version.h"
#include "formats/PngFile.h"
#include "dataset/DirectoryScanner.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryFile>

#include <algorithm>

namespace liteanno::application {

using formats::ExportFormat;

namespace {

Qt::CaseSensitivity pathCase()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString absoluteClean(const QString& path)
{
    const QFileInfo info(path);
    const QString canonical = info.exists() ? info.canonicalFilePath() : QString();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool within(const QString& path, const QString& folder)
{
    if (path.compare(folder, pathCase()) == 0)
        return true;
    const QString prefix = folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/');
    return path.startsWith(prefix, pathCase());
}

// Labels that make a folder name on Windows and Linux: no separators or reserved characters.
bool usableFolderName(const QString& name)
{
    static const QString reserved = QStringLiteral("<>:\"/\\|?*");
    if (name.trimmed().isEmpty() || name == QLatin1String(".") || name == QLatin1String("..") || name.endsWith(QLatin1Char('.'))
        || name.endsWith(QLatin1Char(' ')))
        return false;
    return std::none_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() < 32 || reserved.contains(c); });
}

QString csvField(const QString& text)
{
    if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"')) && !text.contains(QLatin1Char('\n'))
        && !text.contains(QLatin1Char('\r')))
        return text;
    QString quoted = text;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

QString stemPath(const QString& imagePath)
{
    const qsizetype dot = imagePath.lastIndexOf(QLatin1Char('.'));
    const qsizetype slash = imagePath.lastIndexOf(QLatin1Char('/'));
    return dot > slash ? imagePath.left(dot) : imagePath;
}

// Files and folders an export created, removed again when it does not finish.
class Created
{
public:
    explicit Created(QString root)
        : m_root(std::move(root))
    {
    }

    QString error() const { return m_error; }

    bool makeFolder(const QString& folder)
    {
        QStringList missing;
        for (QString f = folder; !QFileInfo::exists(f) && within(f, m_root); f = QFileInfo(f).absolutePath()) {
            missing.prepend(f);
            if (f == m_root)
                break;
        }
        for (const QString& f : std::as_const(missing)) {
            if (!QDir().mkdir(f)) {
                m_error = QStringLiteral(u"无法创建文件夹 %1").arg(QDir::toNativeSeparators(f));
                return false;
            }
            m_folders.append(f);
        }
        return true;
    }

    bool write(const QString& path, const QByteArray& bytes)
    {
        if (!makeFolder(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            m_error = QStringLiteral(u"无法写入 %1：%2").arg(QDir::toNativeSeparators(path), file.errorString());
            return false;
        }
        m_files.append(path);
        if (file.write(bytes) != bytes.size() || !file.flush()) {
            m_error = QStringLiteral(u"无法写入 %1：%2").arg(QDir::toNativeSeparators(path), file.errorString());
            return false;
        }
        return true;
    }

    bool copy(const QString& from, const QString& to)
    {
        if (!makeFolder(QFileInfo(to).absolutePath()))
            return false;
        if (!QFile::copy(from, to)) {
            m_error = QStringLiteral(u"无法复制图片到 %1").arg(QDir::toNativeSeparators(to));
            return false;
        }
        m_files.append(to);
        return true;
    }

    void opened(const QString& path) { m_files.append(path); }

    void removeAll()
    {
        for (auto it = m_files.crbegin(); it != m_files.crend(); ++it)
            QFile::remove(*it);
        for (auto it = m_folders.crbegin(); it != m_folders.crend(); ++it)
            QDir().rmdir(*it);
        m_files.clear();
        m_folders.clear();
    }

private:
    QString m_root;
    QStringList m_files;
    QStringList m_folders;
    QString m_error;
};

QString notDoneReason(AnnotationStatus status)
{
    switch (status) {
    case AnnotationStatus::ConfirmedWithProblems:
        return QStringLiteral(u"已确认，但现在不满足确认条件");
    default:
        return QStringLiteral(u"未完成（%1）").arg(annotationStatusText(status));
    }
}

} // namespace

ImageOnDisk readImageOnDisk(const ProjectSession& project, const QString& imagePath, bool missing, bool nameCollision,
                            const ImageInfoReader& readImageInfo)
{
    ImageOnDisk result;
    result.path = imagePath;
    result.nameCollision = nameCollision;
    const QString imageFile = project.imageFilePath(imagePath);
    if (missing || !QFileInfo(imageFile).isFile()) {
        result.missing = true;
        result.status = AnnotationStatus::Missing;
        return result;
    }
    ImageInfo info = readImageInfo ? readImageInfo(imageFile).value_or(ImageInfo()) : ImageInfo();
    info.fileName = QFileInfo(imageFile).fileName();
    result.annotation = formats::loadAnnotationFile(project.annotationFilePath(imagePath), info,
                                                    project.config().imageLabels.options, &result.bytes);
    result.status = statusOfFile(result.annotation->file, project.config(), nameCollision,
                                 result.annotation->stamp.exists);
    return result;
}

ExportScope scopeOf(const ProjectSession& project, const QStringList& batchStems, const std::function<bool()>& cancelled)
{
    ExportScope scope;
    dataset::ScanOptions options;
    options.cancelled = cancelled;
    const dataset::ScanResult scan = dataset::scanDirectory(project.rootPath(), options);
    if (scan.cancelled) {
        scope.error = QStringLiteral(u"已取消");
        return scope;
    }
    scope.scanComplete = scan.complete;
    if (batchStems.isEmpty()) {
        scope.wholeProject = true;
        if (!scan.complete) {
            scope.error = QStringLiteral(u"有 %1 个文件夹无法读取（例如 %2），整个项目的图片不全，不能按整个项目处理。")
                              .arg(scan.unreadable.size())
                              .arg(scan.unreadable.value(0));
            return scope;
        }
        scope.images.reserve(scan.images.size());
        for (const dataset::ScannedImage& image : scan.images)
            scope.images.append({image.path, false, image.nameCollision});
        return scope;
    }

    WorksetStore worksets(project.rootPath(), project.config().projectId);
    if (!worksets.load()) {
        scope.error = QStringLiteral(u"无法读取批次文件夹 %1").arg(QDir::toNativeSeparators(worksets.directory()));
        return scope;
    }
    QHash<QString, bool> found;  // image -> name collision
    found.reserve(scan.images.size());
    for (const dataset::ScannedImage& image : scan.images)
        found.insert(image.path, image.nameCollision);
    QSet<QString> listed;
    for (const QString& stem : batchStems) {
        const WorksetStore::Batch* batch = worksets.find(stem);
        if (!batch) {
            scope.error = QStringLiteral(u"没有批次 %1").arg(stem);
            return scope;
        }
        if (!batch->list) {
            scope.error = QStringLiteral(u"批次清单 %1 已损坏（%2）").arg(stem, batch->error);
            return scope;
        }
        scope.batches.append({batch->number, batch->list->batchId});
        for (const QString& item : batch->list->items) {
            if (listed.contains(item))
                continue;
            listed.insert(item);
            const auto it = found.constFind(item);
            scope.images.append({item, it == found.cend(), it != found.cend() && *it});
        }
    }
    return scope;
}

const char* exportResultName(ExportResult result)
{
    switch (result) {
    case ExportResult::Exported:
        return "exported";
    case ExportResult::Negative:
        return "negative";
    case ExportResult::Skipped:
        return "skipped";
    case ExportResult::NotDone:
        return "not-done";
    case ExportResult::Unconvertible:
        return "unconvertible";
    }
    return "unknown";
}

int ExportReport::count(ExportResult result) const
{
    return int(std::count_if(items.cbegin(), items.cend(), [result](const ExportItem& item) { return item.result == result; }));
}

QList<ExportItem> ExportReport::unconvertible() const
{
    QList<ExportItem> found;
    for (const ExportItem& item : items) {
        if (item.result == ExportResult::Unconvertible)
            found.append(item);
    }
    return found;
}

ExportService::ExportService(const ProjectSession& project, ImageInfoReader readImageInfo)
    : m_project(project)
    , m_readImageInfo(std::move(readImageInfo))
{
}

ExportReport ExportService::run(const ExportRequest& request, const Progress& progress) const
{
    ExportReport report;
    const ProjectConfig& config = m_project.config();
    const ExportFormat format = request.format;
    const bool classification = format == ExportFormat::Classification;
    if (classification && config.imageLabels.options.isEmpty()) {
        report.error = QStringLiteral(u"这个项目没有图片级标签，不能导出图片分类。");
        return report;
    }
    if (!classification && config.shapeTypes.isEmpty()) {
        report.error = QStringLiteral(u"这个项目不标形状，不能导出为 %1。").arg(formats::exportFormatName(format));
        return report;
    }
    const bool singleChoice = config.imageLabels.mode == ImageLabelMode::Single;
    QHash<QString, int> classIds;
    QList<std::pair<int, QString>> categories;
    for (const ClassDef& c : config.classes) {
        classIds.insert(c.name, c.id);
        categories.append({c.id, c.name});
    }
    std::sort(categories.begin(), categories.end());
    if (formats::exportNeedsClassIds(format) && classIds.isEmpty()) {
        report.error = QStringLiteral(u"项目文件没有类别列表（classes），%1 需要类别编号。").arg(formats::exportFormatName(format));
        return report;
    }
    // Masks: background 0, each class its id + 1 (written to mask_colors.json).
    QHash<QString, int> maskValues;
    if (format == ExportFormat::Mask) {
        for (const ClassDef& c : config.classes) {
            if (c.id < 0 || c.id > 254) {
                report.error = QStringLiteral(u"掩膜的像素值是类别编号加 1，类别“%1”的编号 %2 超出了 0–254。").arg(c.name).arg(c.id);
                return report;
            }
            maskValues.insert(c.name, c.id + 1);
        }
    }

    // The output folder: new or empty, and apart from the data so a scan never lists it.
    const QString root = absoluteClean(m_project.rootPath());
    const QString output = absoluteClean(request.outputDir);
    if (request.outputDir.trimmed().isEmpty()) {
        report.error = QStringLiteral(u"请选择导出文件夹。");
        return report;
    }
    if (within(output, root) || within(root, output)) {
        report.error = QStringLiteral(u"导出文件夹不能在数据文件夹里面，也不能包含数据文件夹。");
        return report;
    }
    if (QFileInfo(output).exists()) {
        if (!QFileInfo(output).isDir()) {
            report.error = QStringLiteral(u"%1 不是文件夹。").arg(QDir::toNativeSeparators(output));
            return report;
        }
        if (!QDir(output).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            report.error = QStringLiteral(u"导出文件夹 %1 不是空的，请选择一个空文件夹或新文件夹。").arg(QDir::toNativeSeparators(output));
            return report;
        }
    }

    int done = 0;
    int total = 0;
    const auto step = [&]() { return !progress || progress(done, total); };
    const ExportScope scope = scopeOf(m_project, request.batches, [&] { return progress && !progress(0, 0); });
    if (!scope.error.isEmpty()) {
        report.error = scope.error;
        report.cancelled = scope.error == QStringLiteral(u"已取消");
        return report;
    }
    total = int(scope.images.size()) * 2;

    // First pass: what each image is, without writing anything.
    QHash<QString, QString> digests;  // exported images: the file content they were read with
    for (const ScopeImage& scoped : scope.images) {
        ExportItem item;
        item.image = scoped.path;
        const ImageOnDisk image = readImageOnDisk(m_project, scoped.path, scoped.missing, scoped.nameCollision, m_readImageInfo);
        if (image.annotation)
            item.status = QString::fromLatin1(annotationStatusName(image.status));
        if (image.missing) {
            item.reason = QStringLiteral(u"图片缺失");
        } else if (image.nameCollision) {
            item.reason = QStringLiteral(u"同一目录下有同名图片");
        } else if (image.status == AnnotationStatus::Skipped) {
            item.result = ExportResult::Skipped;
            item.reason = image.annotation->file.document().review().skipReason;
        } else if (image.status != AnnotationStatus::Confirmed) {
            item.reason = notDoneReason(image.status);
        } else if (classification) {
            const AnnotationDocument& document = image.annotation->file.document();
            QStringList chosen;
            for (const QString& option : config.imageLabels.options) {
                if (document.imageLabel(option).value_or(false))
                    chosen.append(option);
            }
            if (singleChoice && chosen.size() != 1) {
                item.result = ExportResult::Unconvertible;
                item.reason = chosen.isEmpty() ? QStringLiteral(u"没有选图片级标签") : QStringLiteral(u"选了不止一个图片级标签");
            } else if (singleChoice && !usableFolderName(chosen.first())) {
                item.result = ExportResult::Unconvertible;
                item.reason = QStringLiteral(u"标签“%1”不能用作文件夹名").arg(chosen.first());
            } else {
                item.result = ExportResult::Exported;
                item.imageLabels = chosen;
                digests.insert(scoped.path, image.annotation->stamp.digest);
            }
        } else {
            const formats::ExportImage shapes = formats::exportImageOf(scoped.path, image.bytes, image.annotation->file);
            const QList<formats::ExportIssue> issues = formats::unconvertibleShapes(shapes, format, classIds);
            if (!issues.isEmpty()) {
                item.result = ExportResult::Unconvertible;
                QStringList reasons;
                for (const formats::ExportIssue& issue : issues)
                    reasons.append(QStringLiteral(u"第 %1 个对象：%2").arg(issue.shape + 1).arg(issue.reason));
                item.reason = reasons.join(QStringLiteral(u"；"));
            } else {
                item.result = shapes.shapes.isEmpty() ? ExportResult::Negative : ExportResult::Exported;
                item.clampedObjects = formats::clampedShapes(shapes, format);
                digests.insert(scoped.path, image.annotation->stamp.digest);
            }
        }
        report.items.append(item);
        ++done;
        if (!step()) {
            report.cancelled = true;
            report.error = QStringLiteral(u"已取消");
            return report;
        }
    }
    const int unconvertible = report.count(ExportResult::Unconvertible);
    if (unconvertible > 0 && !request.skipUnconvertible) {
        report.error = QStringLiteral(u"有 %1 张图片包含无法转换为 %2 的对象，没有导出。可以选择跳过这些图片继续导出。")
                           .arg(unconvertible)
                           .arg(formats::exportFormatName(format));
        return report;
    }
    if (report.count(ExportResult::Exported) + report.count(ExportResult::Negative) == 0) {
        report.error = QStringLiteral(u"没有已确认的图片可以导出。");
        return report;
    }

    // Second pass: the files.
    Created created(output);
    const auto fail = [&](const QString& error) {
        created.removeAll();
        report.error = error;
        report.written = false;
        return report;
    };
    if (!created.makeFolder(output))
        return fail(created.error());
    QFile coco;
    QTemporaryFile cocoAnnotations;
    std::unique_ptr<formats::CocoWriter> cocoWriter;
    const bool isCoco = format == ExportFormat::CocoDetection || format == ExportFormat::CocoSegmentation;
    if (isCoco) {
        coco.setFileName(output + QLatin1Char('/') + formats::CocoWriter::fileName(format));
        if (!coco.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !cocoAnnotations.open())
            return fail(QStringLiteral(u"无法写入 %1").arg(QDir::toNativeSeparators(coco.fileName())));
        created.opened(coco.fileName());
        const QDate today = QDate::currentDate();
        cocoWriter = std::make_unique<formats::CocoWriter>(
            format, categories,
            formats::CocoInfo{today.year(), QString::fromLatin1(kAppVersion), today.toString(Qt::ISODate)});
        if (!cocoWriter->begin(&coco, &cocoAnnotations))
            return fail(QStringLiteral(u"无法写入 %1").arg(QDir::toNativeSeparators(coco.fileName())));
    }
    QStringList csvRows;  // multiple-choice classification
    for (const ExportItem& item : std::as_const(report.items)) {
        ++done;
        if (item.result == ExportResult::Exported || item.result == ExportResult::Negative) {
            const ScopeImage scoped{item.image, false, false};
            const ImageOnDisk image = readImageOnDisk(m_project, scoped.path, false, false, m_readImageInfo);
            // The file must be the one checked in the first pass.
            if (!image.annotation || image.annotation->stamp.digest != digests.value(item.image))
                return fail(QStringLiteral(u"导出过程中 %1 的标注文件被修改了，请重新导出。").arg(item.image));
            const formats::ExportImage shapes = formats::exportImageOf(item.image, image.bytes, image.annotation->file);
            const QString stem = output + QLatin1Char('/') + stemPath(item.image);
            bool ok = true;
            QString imageTarget = output + QLatin1Char('/') + item.image;  // where a copy goes
            bool copyImage = request.copyImages;
            switch (format) {
            case ExportFormat::YoloDetection:
            case ExportFormat::YoloSegmentation:
            case ExportFormat::YoloObb:
                ok = created.write(stem + QStringLiteral(".txt"), formats::yoloLabels(shapes, format, classIds));
                break;
            case ExportFormat::Dota:
                ok = created.write(stem + QStringLiteral(".txt"), formats::dotaLabels(shapes));
                break;
            case ExportFormat::Mask:
                ok = created.write(stem + QStringLiteral(".png"),
                                   formats::grayscalePng(shapes.width, shapes.height, formats::maskPixels(shapes, maskValues)));
                break;
            case ExportFormat::LabelMe:
                // LabelMe opens the image named in the file, next to it: the image is always copied.
                ok = created.write(stem + QStringLiteral(".json"),
                                   formats::labelMeFile(shapes, image.bytes, QFileInfo(item.image).fileName()));
                copyImage = true;
                break;
            case ExportFormat::Classification:
                if (singleChoice) {
                    imageTarget = output + QLatin1Char('/') + item.imageLabels.value(0) + QLatin1Char('/') + item.image;
                    copyImage = true;
                } else {
                    QStringList row{csvField(item.image)};
                    for (const QString& option : config.imageLabels.options)
                        row.append(item.imageLabels.contains(option) ? QStringLiteral("1") : QStringLiteral("0"));
                    csvRows.append(row.join(QLatin1Char(',')));
                }
                break;
            case ExportFormat::VocRectangle:
            case ExportFormat::VocPolygon:
                ok = created.write(stem + QStringLiteral(".xml"),
                                   formats::vocAnnotation(shapes, format,
                                                          QDir::toNativeSeparators(QFileInfo(stem).absolutePath())));
                break;
            case ExportFormat::CocoDetection:
            case ExportFormat::CocoSegmentation:
                ok = cocoWriter->add(shapes, item.image, classIds);
                if (!ok)
                    return fail(QStringLiteral(u"无法写入 %1").arg(QDir::toNativeSeparators(coco.fileName())));
                break;
            }
            if (!ok)
                return fail(created.error());
            if (copyImage) {
                const QString target = isCoco ? output + QStringLiteral("/images/") + item.image : imageTarget;
                if (!created.copy(m_project.imageFilePath(item.image), target))
                    return fail(created.error());
            }
        }
        if (!step()) {
            created.removeAll();
            report.cancelled = true;
            report.error = QStringLiteral(u"已取消");
            return report;
        }
    }
    if (cocoWriter && (!cocoWriter->finish() || !coco.flush()))
        return fail(QStringLiteral(u"无法写入 %1").arg(QDir::toNativeSeparators(coco.fileName())));
    coco.close();

    if (classification && !singleChoice) {
        QStringList header{QStringLiteral("image")};
        for (const QString& option : config.imageLabels.options)
            header.append(csvField(option));
        csvRows.prepend(header.join(QLatin1Char(',')));
        // With a byte order mark, so that Excel reads the Chinese names.
        if (!created.write(output + QStringLiteral("/labels.csv"), "\xEF\xBB\xBF" + (csvRows.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8()))
            return fail(created.error());
    }
    if (format == ExportFormat::Mask) {
        if (!created.write(output + QStringLiteral("/mask_colors.json"), formats::maskColorsFile(maskValues, categories)))
            return fail(created.error());
    }

    // YOLO class names, when the ids are 0, 1, 2...
    if (format == ExportFormat::YoloDetection || format == ExportFormat::YoloSegmentation || format == ExportFormat::YoloObb) {
        bool contiguous = true;
        QStringList names;
        for (int i = 0; i < categories.size(); ++i) {
            contiguous = contiguous && categories[i].first == i;
            names.append(categories[i].second);
        }
        const QString classesFile = output + QStringLiteral("/classes.txt");
        if (contiguous && !QFileInfo::exists(classesFile) && !created.write(classesFile, names.join(QLatin1Char('\n')).toUtf8() + '\n'))
            return fail(created.error());
    }

    formats::ExportManifest manifest;
    manifest.projectId = config.projectId;
    manifest.exportedAt = WorksetStore::timestamp(QDateTime::currentDateTime());
    manifest.appVersion = QString::fromLatin1(kAppVersion);
    manifest.wholeProject = scope.wholeProject;
    manifest.batches = scope.batches;
    manifest.scanComplete = scope.scanComplete;
    manifest.format = format;
    manifest.skipUnconvertible = request.skipUnconvertible;
    manifest.copyImages = request.copyImages;
    manifest.categories = categories;
    int clamped = 0;
    for (const ExportItem& item : std::as_const(report.items)) {
        manifest.items.append({item.image, QString::fromLatin1(exportResultName(item.result)), item.status, item.reason,
                               item.clampedObjects});
        clamped += item.clampedObjects;
    }
    for (ExportResult result : {ExportResult::Exported, ExportResult::Negative, ExportResult::Skipped,
                                ExportResult::NotDone, ExportResult::Unconvertible})
        manifest.counts.append({QString::fromLatin1(exportResultName(result)), report.count(result)});
    manifest.counts.append({QStringLiteral("clampedObjects"), clamped});
    report.manifestPath = output + QStringLiteral("/liteanno-export.json");
    if (!created.write(report.manifestPath, formats::exportManifestToBytes(manifest)))
        return fail(created.error());
    report.written = true;
    return report;
}

} // namespace liteanno::application
