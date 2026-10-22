#include "application/ImportService.h"

#include "application/RecoveryService.h"
#include "core/ShapeType.h"
#include "dataset/DirectoryScanner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <optional>

namespace liteanno::application {

using formats::ImportFormat;

namespace {

QString stemPath(const QString& imagePath)
{
    const qsizetype dot = imagePath.lastIndexOf(QLatin1Char('.'));
    const qsizetype slash = imagePath.lastIndexOf(QLatin1Char('/'));
    return dot > slash ? imagePath.left(dot) : imagePath;
}

QString fileNameOf(const QString& path)
{
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

std::optional<QByteArray> readFile(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return std::nullopt;
    }
    return file.readAll();
}

// The names of the YOLO class numbers: classes.txt in the label folder, as upstream asks for
// one, or else the ids of the project file.
std::optional<QStringList> yoloClassNames(const QString& labelDir, const ProjectConfig& config, QString& error)
{
    const QString classesFile = QDir(labelDir).filePath(QStringLiteral("classes.txt"));
    if (QFileInfo(classesFile).isFile()) {
        const std::optional<QByteArray> bytes = readFile(classesFile, error);
        if (!bytes) {
            error = QStringLiteral(u"无法读取 %1：%2").arg(QDir::toNativeSeparators(classesFile), error);
            return std::nullopt;
        }
        QString text = QString::fromUtf8(*bytes);
        if (text.startsWith(QChar(0xFEFF)))
            text.remove(0, 1);
        text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        QStringList names = text.split(QLatin1Char('\n'));
        if (!names.isEmpty() && names.last().isEmpty())
            names.removeLast();
        return names;
    }
    if (config.classes.isEmpty()) {
        error = QStringLiteral(u"标注文件夹里没有 classes.txt，项目文件也没有类别列表，无法把类别编号换成名称。");
        return std::nullopt;
    }
    QStringList names;
    for (const ClassDef& c : config.classes) {
        if (c.id < 0)
            continue;
        if (c.id >= names.size())
            names.resize(c.id + 1);
        names[c.id] = c.name;
    }
    return names;
}

// Why the project cannot take the imported shapes, or nothing.
QString projectRefuses(const QList<Shape>& shapes, const ProjectConfig& config)
{
    QSet<QString> classNames;
    for (const ClassDef& c : config.classes)
        classNames.insert(c.name);
    for (const Shape& shape : shapes) {
        if (!classNames.isEmpty() && !classNames.contains(shape.label))
            return QStringLiteral(u"类别“%1”不在项目的类别列表里").arg(shape.label);
        if (!config.shapeTypes.contains(shape.type))
            return QStringLiteral(u"项目不标 %1").arg(shapeTypeName(shape.type));
    }
    return {};
}

// An annotation file the mode leaves alone.
bool keepsExisting(const ImageOnDisk& image, ImportMode mode)
{
    return mode == ImportMode::OnlyNew && image.annotation && !image.annotation->file.isNew();
}

// Why the annotation file of the image cannot be written now, or nothing.
QString cannotWrite(const ImageOnDisk& image, const QSet<QString>& recorded)
{
    if (image.missing)
        return QStringLiteral(u"图片不存在");
    if (image.nameCollision)
        return QStringLiteral(u"同一目录下有同名图片");
    if (recorded.contains(image.path))
        return QStringLiteral(u"窗口里有这张图片未写入的修改");
    if (image.annotation->file.isReadOnly())
        return QStringLiteral(u"标注文件只读（%1）").arg(annotationStatusText(image.status));
    return {};
}

} // namespace

QString importModeName(ImportMode mode)
{
    switch (mode) {
    case ImportMode::OnlyNew:
        return QStringLiteral("new");
    case ImportMode::Append:
        return QStringLiteral("append");
    case ImportMode::Replace:
        return QStringLiteral("replace");
    }
    return {};
}

std::optional<ImportMode> importModeFromName(const QString& name)
{
    for (ImportMode mode : {ImportMode::OnlyNew, ImportMode::Append, ImportMode::Replace}) {
        if (importModeName(mode) == name)
            return mode;
    }
    return std::nullopt;
}

const char* importResultName(ImportResult result)
{
    switch (result) {
    case ImportResult::Imported:
        return "imported";
    case ImportResult::NoLabelFile:
        return "no-label-file";
    case ImportResult::HasAnnotation:
        return "has-annotation";
    case ImportResult::NotWritten:
        return "not-written";
    case ImportResult::Unconvertible:
        return "unconvertible";
    }
    return "unknown";
}

int ImportReport::count(ImportResult result) const
{
    return int(std::count_if(items.cbegin(), items.cend(), [result](const ImportItem& item) { return item.result == result; }));
}

QList<ImportItem> ImportReport::unconvertible() const
{
    QList<ImportItem> found;
    for (const ImportItem& item : items) {
        if (item.result == ImportResult::Unconvertible)
            found.append(item);
    }
    return found;
}

ImportService::ImportService(const ProjectSession& project, ImageInfoReader readImageInfo)
    : m_project(project)
    , m_readImageInfo(std::move(readImageInfo))
{
}

ImportReport ImportService::run(const ImportRequest& request, const Progress& progress) const
{
    ImportReport report;
    const ProjectConfig& config = m_project.config();
    const ImportFormat format = request.format;
    if (config.shapeTypes.isEmpty()) {
        report.error = QStringLiteral(u"这个项目不标形状，不能导入 %1。").arg(formats::importFormatName(format));
        return report;
    }
    if (request.labelDir.trimmed().isEmpty() || !QFileInfo(request.labelDir).isDir()) {
        report.error = QStringLiteral(u"请选择标注文件夹。");
        return report;
    }
    const QDir labelDir(request.labelDir);
    QStringList classNames;
    if (formats::importNeedsClassNames(format)) {
        std::optional<QStringList> names = yoloClassNames(request.labelDir, config, report.error);
        if (!names)
            return report;
        classNames = std::move(*names);
    }

    const ExportScope scope = scopeOf(m_project, request.batches, [&progress] { return progress && !progress(0, 0); });
    if (!scope.error.isEmpty()) {
        report.error = scope.error;
        report.cancelled = scope.error == QStringLiteral(u"已取消");
        return report;
    }
    // Images with unsaved edits of the window in a recovery record are left alone: writing
    // them here would make the record look outdated.
    QSet<QString> recorded;
    const RecoveryService recovery(m_project.paths(), m_project.location());
    for (const RecoveryCandidate& candidate : recovery.scan().candidates)
        recorded.insert(candidate.record.imagePath);

    // Pair the label files: by the relative path first, then by the file name at the top of
    // the label folder. A file name several images would take pairs none of them.
    const QString suffix = formats::importLabelSuffix(format);
    QList<QString> labelFiles;
    labelFiles.reserve(scope.images.size());
    QHash<QString, int> byName;
    for (const ScopeImage& image : scope.images) {
        const QString nested = labelDir.filePath(stemPath(image.path) + suffix);
        if (QFileInfo(nested).isFile()) {
            labelFiles.append(nested);
            continue;
        }
        const QString top = labelDir.filePath(stemPath(fileNameOf(image.path)) + suffix);
        if (top != nested && QFileInfo(top).isFile()) {
            labelFiles.append(top);
            ++byName[top];
        } else {
            labelFiles.append(QString());
        }
    }

    const int total = int(scope.images.size()) * 2;
    QList<QList<Shape>> imported(scope.images.size());
    report.items.reserve(scope.images.size());
    for (qsizetype i = 0; i < scope.images.size(); ++i) {
        if (progress && !progress(int(i), total)) {
            report.cancelled = true;
            return report;
        }
        const ScopeImage& scoped = scope.images[i];
        ImportItem item;
        item.image = scoped.path;
        item.labelFile = labelFiles[i];
        const auto done = [&report, &item](ImportResult result, const QString& reason = {}) {
            item.result = result;
            item.reason = reason;
            report.items.append(item);
        };
        if (item.labelFile.isEmpty()) {
            done(ImportResult::NoLabelFile);
            continue;
        }
        if (byName.value(item.labelFile) > 1) {
            done(ImportResult::Unconvertible, QStringLiteral(u"%1 张图片都按文件名配对到 %2，无法确定是哪一张的")
                                                  .arg(byName.value(item.labelFile))
                                                  .arg(QDir::toNativeSeparators(item.labelFile)));
            continue;
        }
        const ImageOnDisk image =
            readImageOnDisk(m_project, scoped.path, scoped.missing, scoped.nameCollision, m_readImageInfo);
        if (!image.missing && keepsExisting(image, request.mode)) {
            done(ImportResult::HasAnnotation, QStringLiteral(u"已有标注文件"));
            continue;
        }
        if (const QString reason = cannotWrite(image, recorded); !reason.isEmpty()) {
            done(ImportResult::NotWritten, reason);
            continue;
        }
        const ImageInfo& info = image.annotation->file.image();
        if (info.width <= 0 || info.height <= 0) {
            done(ImportResult::Unconvertible, QStringLiteral(u"读不出图片尺寸"));
            continue;
        }
        QString error;
        const std::optional<QByteArray> bytes = readFile(item.labelFile, error);
        if (!bytes) {
            done(ImportResult::Unconvertible, QStringLiteral(u"无法读取标注文件：%1").arg(error));
            continue;
        }
        formats::ImportedLabels labels;
        switch (format) {
        case ImportFormat::YoloDetection:
        case ImportFormat::YoloSegmentation:
        case ImportFormat::YoloObb:
            labels = formats::parseYoloLabels(*bytes, format, classNames, info.width, info.height);
            break;
        case ImportFormat::VocRectangle:
        case ImportFormat::VocPolygon:
            labels = formats::parseVocAnnotation(*bytes, format, info.width, info.height);
            break;
        case ImportFormat::Dota:
            labels = formats::parseDotaLabels(*bytes);
            break;
        }
        if (labels.error.isEmpty())
            labels.error = projectRefuses(labels.shapes, config);
        if (!labels.error.isEmpty()) {
            done(ImportResult::Unconvertible, labels.error);
            continue;
        }
        item.shapes = int(labels.shapes.size());
        imported[i] = std::move(labels.shapes);
        done(ImportResult::Imported);
    }
    if (const int refused = report.count(ImportResult::Unconvertible); refused > 0 && !request.skipUnconvertible) {
        report.error = QStringLiteral(u"有 %1 张图片的标注无法导入，没有写入任何标注文件。").arg(refused);
        return report;
    }

    // Write: each file is read again and saved with the checks of a normal save, so a change
    // made since the check above is never overwritten.
    for (qsizetype i = 0; i < scope.images.size(); ++i) {
        if (progress && !progress(int(scope.images.size() + i), total)) {
            report.cancelled = true;
            return report;
        }
        ImportItem& item = report.items[i];
        if (item.result != ImportResult::Imported)
            continue;
        const ScopeImage& scoped = scope.images[i];
        ImageOnDisk image =
            readImageOnDisk(m_project, scoped.path, scoped.missing, scoped.nameCollision, m_readImageInfo);
        if (!image.missing && keepsExisting(image, request.mode)) {
            item.result = ImportResult::HasAnnotation;
            item.reason = QStringLiteral(u"检查之后有了标注文件");
            continue;
        }
        if (const QString reason = cannotWrite(image, recorded); !reason.isEmpty()) {
            item.result = ImportResult::NotWritten;
            item.reason = reason;
            continue;
        }
        // Straight on the document: no undo history is needed here.
        formats::LoadedAnnotation& annotation = *image.annotation;
        AnnotationDocument& document = annotation.file.document();
        if (request.mode == ImportMode::Replace) {
            while (!document.shapes().isEmpty())
                document.removeShape(document.shapes().size() - 1);
        }
        bool added = true;
        for (const Shape& shape : std::as_const(imported[i]))
            added = added && document.addShape(shape) >= 0;
        if (!added) {
            item.result = ImportResult::NotWritten;
            item.reason = QStringLiteral(u"有对象无法加入标注");
            continue;
        }
        if (!annotation.file.needsWrite()) {
            item.reason = QStringLiteral(u"没有要写入的变化");
            continue;
        }
        const QString imageFile = m_project.imageFilePath(scoped.path);
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
            formats::saveAnnotationFile(m_project.annotationFilePath(scoped.path), annotation, options);
        if (saved.status == formats::SaveStatus::Saved) {
            report.written = true;
        } else {
            item.result = ImportResult::NotWritten;
            item.reason = saved.error.isEmpty() ? QString::fromLatin1(formats::saveStatusName(saved.status)) : saved.error;
        }
    }
    if (progress)
        progress(total, total);
    return report;
}

} // namespace liteanno::application
