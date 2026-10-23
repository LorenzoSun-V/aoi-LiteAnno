#include "application/ImportService.h"

#include "application/RecoveryService.h"
#include "core/ShapeType.h"
#include "dataset/DirectoryScanner.h"
#include "formats/Exporters.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <limits>
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

// The names of the YOLO class numbers: the classes.txt chosen, else classes.txt in the label
// folder, as upstream asks for one, else the ids of the project file. The file follows the
// rules of the export's classes.txt (formats::parseClassesFile).
std::optional<formats::ClassNames> yoloClassNames(const QString& chosen, const QString& labelDir, const ProjectConfig& config,
                                          QString& error)
{
    QString classesFile = chosen;
    if (classesFile.isEmpty()) {
        const QString inFolder = QDir(labelDir).filePath(QStringLiteral("classes.txt"));
        if (QFileInfo(inFolder).isFile())
            classesFile = inFolder;
    }
    if (!classesFile.isEmpty()) {
        const formats::ClassesFile read = formats::readClassesFile(classesFile);
        if (!read.error.isEmpty()) {
            error = QStringLiteral(u"类别文件 %1：%2").arg(QDir::toNativeSeparators(classesFile), read.error);
            return std::nullopt;
        }
        formats::ClassNames names;
        for (qsizetype i = 0; i < read.names.size(); ++i)
            names.insert(int(i), read.names[i]);
        return names;
    }
    if (config.classes.isEmpty()) {
        error = QStringLiteral(u"请选择类别文件 classes.txt：标注文件夹里没有，项目文件也没有类别列表，无法把类别编号换成名称。");
        return std::nullopt;
    }
    formats::ClassNames names;
    for (const ClassDef& c : config.classes) {
        if (c.id >= 0)
            names.insert(c.id, c.name);
    }
    return names;
}

// The mask mapping: the file chosen, else mask_colors.json (what our mask export writes) in
// the label folder or the folder above it.
formats::MaskMapping maskMapping(const QString& chosen, const QString& labelDir)
{
    QString file = chosen;
    if (file.isEmpty()) {
        const QDir dir(labelDir);
        for (const QString& candidate : {dir.filePath(QStringLiteral("mask_colors.json")),
                                         dir.filePath(QStringLiteral("../mask_colors.json"))}) {
            if (QFileInfo(candidate).isFile()) {
                file = QDir::cleanPath(candidate);
                break;
            }
        }
    }
    formats::MaskMapping mapping;
    if (file.isEmpty()) {
        mapping.error = QStringLiteral(u"请选择掩膜的映射文件（如 mask_colors.json）：标注文件夹和上一级都没有 mask_colors.json。");
        return mapping;
    }
    QString error;
    const std::optional<QByteArray> bytes = readFile(file, error);
    if (!bytes) {
        mapping.error = QStringLiteral(u"无法读取映射文件 %1：%2").arg(QDir::toNativeSeparators(file), error);
        return mapping;
    }
    mapping = formats::parseMaskMapping(*bytes);
    if (!mapping.error.isEmpty())
        mapping.error = QStringLiteral(u"映射文件 %1：%2").arg(QDir::toNativeSeparators(file), mapping.error);
    return mapping;
}

// A path as the file system compares it: without case on Windows.
QString pathKey(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
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

// Puts the imported shapes into the document as the mode says. Their group numbers are moved
// past the ones the document uses, so an appended instance never joins one already there;
// the parts of one instance keep sharing theirs. False when a shape cannot be added (its
// points do not fit the model, e.g. coordinates that overflowed when scaled).
bool addImported(AnnotationDocument& document, const QList<Shape>& shapes, ImportMode mode)
{
    if (mode == ImportMode::Replace) {
        while (!document.shapes().isEmpty())
            document.removeShape(document.shapes().size() - 1);
    }
    std::optional<qint64> used;
    for (const Shape& shape : document.shapes()) {
        if (shape.groupId)
            used = std::max(used.value_or(*shape.groupId), *shape.groupId);
    }
    QHash<qint64, qint64> renumbered;
    for (Shape shape : shapes) {
        if (used && shape.groupId) {
            if (!renumbered.contains(*shape.groupId)) {
                if (*used == std::numeric_limits<qint64>::max())
                    return false;
                renumbered.insert(*shape.groupId, ++*used);
            }
            shape.groupId = renumbered.value(*shape.groupId);
        }
        if (document.addShape(std::move(shape)) < 0)
            return false;
    }
    return true;
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
    case ImportResult::Unchanged:
        return "unchanged";
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

ImportService::ImportService(const ProjectSession& project, ImageInfoReader readImageInfo, MaskImageReader readMask)
    : m_project(project)
    , m_readImageInfo(std::move(readImageInfo))
    , m_readMask(std::move(readMask))
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
    const bool oneFile = formats::importReadsOneFile(format);
    if (oneFile && (request.labelFile.trimmed().isEmpty() || !QFileInfo(request.labelFile).isFile())) {
        report.error = QStringLiteral(u"请选择 COCO 标注文件（.json）。");
        return report;
    }
    if (!oneFile && (request.labelDir.trimmed().isEmpty() || !QFileInfo(request.labelDir).isDir())) {
        report.error = QStringLiteral(u"请选择标注文件夹。");
        return report;
    }
    const QDir labelDir(request.labelDir);
    formats::ClassNames classNames;
    if (formats::importNeedsClassNames(format)) {
        std::optional<formats::ClassNames> names = yoloClassNames(request.classesFile, request.labelDir, config, report.error);
        if (!names)
            return report;
        classNames = std::move(*names);
    }
    formats::MaskMapping mapping;
    if (format == ImportFormat::Mask) {
        if (!m_readMask) {
            report.error = QStringLiteral(u"这里不能读取掩膜图片。");
            return report;
        }
        mapping = maskMapping(request.mappingFile, request.labelDir);
        if (!mapping.error.isEmpty()) {
            report.error = mapping.error;
            return report;
        }
    }
    formats::PoseConfig pose;
    const bool hasPose = !request.poseConfigFile.trimmed().isEmpty();
    if (format == ImportFormat::YoloPose && !hasPose) {
        report.error = QStringLiteral(u"YOLO 姿态需要姿态配置文件（每个类别的关键点名称），请选择。");
        return report;
    }
    if (formats::importTakesPoseConfig(format) && hasPose) {
        pose = formats::readPoseConfig(request.poseConfigFile);
        if (!pose.error.isEmpty()) {
            report.error = QStringLiteral(u"姿态配置文件 %1 不能用：%2").arg(QDir::toNativeSeparators(request.poseConfigFile), pose.error);
            return report;
        }
    }
    formats::CocoLabels coco;
    if (oneFile) {
        QString error;
        const std::optional<QByteArray> bytes = readFile(request.labelFile, error);
        if (!bytes) {
            report.error = QStringLiteral(u"无法读取 %1：%2").arg(QDir::toNativeSeparators(request.labelFile), error);
            return report;
        }
        coco = formats::parseCocoLabels(*bytes, format, hasPose ? &pose : nullptr);
        if (!coco.error.isEmpty()) {
            report.error = QStringLiteral(u"%1：%2").arg(QDir::toNativeSeparators(request.labelFile), coco.error);
            return report;
        }
    }

    const ExportScope scope = scopeOf(m_project, request.batches, [&progress] { return progress && !progress(0, 0); });
    if (!scope.error.isEmpty()) {
        report.error = scope.error;
        report.cancelled = scope.error == QStringLiteral(u"已取消");
        return report;
    }
    // Images with unsaved edits of the window in a recovery record are left alone: writing
    // them here would make the record look outdated.
    // When the records cannot be listed it is not known which images they hold: nothing is
    // written.
    QSet<QString> recorded;
    const RecoveryScan records = RecoveryService(m_project.paths(), m_project.location()).scan();
    if (!records.error.isEmpty()) {
        report.error = QStringLiteral(u"无法读取未写入修改的恢复记录（%1），不能确定哪些图片有未写入的修改，没有导入。")
                           .arg(records.error);
        return report;
    }
    for (const RecoveryCandidate& candidate : records.candidates)
        recorded.insert(candidate.record.imagePath);
    recorded.unite(request.heldImages);

    // Pair the label files: by the relative path first, then by the file name at the top of
    // the label folder. A file paired by its path is not taken by name for another image, and
    // a file name several images would take pairs none of them.
    const QString suffix = formats::importLabelSuffix(format);
    QList<QString> labelFiles;
    labelFiles.reserve(scope.images.size());
    QHash<QString, int> byName;
    QList<QString> pairingProblems(scope.images.size());
    QList<QString> noLabelReasons(scope.images.size());
    QSet<QString> byPath;
    if (!oneFile) {
        for (const ScopeImage& image : scope.images) {
            const QString nested = labelDir.filePath(stemPath(image.path) + suffix);
            if (QFileInfo(nested).isFile())
                byPath.insert(pathKey(QFileInfo(nested).absoluteFilePath()));
        }
        for (qsizetype i = 0; i < scope.images.size(); ++i) {
            const ScopeImage& image = scope.images[i];
            const QString nested = labelDir.filePath(stemPath(image.path) + suffix);
            const QString top = labelDir.filePath(stemPath(fileNameOf(image.path)) + suffix);
            // A mask folder inside the data folder: its masks are listed as images too, and
            // one is never its own mask.
            const QFileInfo self(m_project.imageFilePath(image.path));
            const auto isLabel = [&self](const QString& file) {
                const QFileInfo info(file);
                return info.isFile() && info != self;
            };
            if (isLabel(nested)) {
                labelFiles.append(nested);
            } else if (top != nested && isLabel(top) && byPath.contains(pathKey(QFileInfo(top).absoluteFilePath()))) {
                labelFiles.append(QString());
                noLabelReasons[i] = QStringLiteral(u"同名的 %1 按路径配给了另一张图片").arg(QDir::toNativeSeparators(top));
            } else if (top != nested && isLabel(top)) {
                labelFiles.append(top);
                ++byName[top];
            } else {
                labelFiles.append(QString());
                // A mask saved as JPEG: said, not passed over.
                if (format == ImportFormat::Mask) {
                    for (const QString& stem : {stemPath(image.path), stemPath(fileNameOf(image.path))}) {
                        for (const char* jpeg : {".jpg", ".jpeg"}) {
                            const QString file = labelDir.filePath(stem + QLatin1String(jpeg));
                            if (labelFiles.last().isEmpty() && QFileInfo(file).isFile()) {
                                labelFiles.last() = file;
                                pairingProblems[i] = QStringLiteral(u"掩膜必须是 PNG 图片（JPEG 有损压缩会改变类别值）");
                            }
                        }
                    }
                }
            }
        }
    }
    // COCO: the image records by the relative path in "file_name", then by the file name
    // alone when one image of the scope has it.
    QList<qsizetype> cocoEntry(scope.images.size(), -1);
    if (oneFile) {
        QHash<QString, QList<qsizetype>> byFileName;
        QHash<QString, qsizetype> byPath;
        for (qsizetype i = 0; i < scope.images.size(); ++i) {
            byFileName[pathKey(fileNameOf(scope.images[i].path))].append(i);
            byPath.insert(pathKey(scope.images[i].path), i);
        }
        for (qsizetype k = 0; k < coco.images.size(); ++k) {
            QString name = coco.images[k].fileName;
            name.replace(QLatin1Char('\\'), QLatin1Char('/'));
            while (name.startsWith(QLatin1String("./")))
                name.remove(0, 2);
            qsizetype found = byPath.value(pathKey(name), -1);
            if (found < 0) {
                const QList<qsizetype> candidates = byFileName.value(pathKey(fileNameOf(name)));
                if (candidates.size() > 1) {
                    report.unmatched.append({coco.images[k].fileName,
                                             QStringLiteral(u"文件夹里有 %1 张图片叫这个名字，无法确定是哪一张").arg(candidates.size())});
                    continue;
                }
                found = candidates.isEmpty() ? -1 : candidates.first();
            }
            if (found < 0) {
                report.unmatched.append({coco.images[k].fileName, QStringLiteral(u"文件夹里没有这张图片")});
                continue;
            }
            if (cocoEntry[found] >= 0) {
                pairingProblems[found] = QStringLiteral(u"COCO 文件里有多条图片记录对应这张图片");
                continue;
            }
            cocoEntry[found] = k;
        }
        for (qsizetype i = 0; i < scope.images.size(); ++i)
            labelFiles.append(cocoEntry[i] >= 0 || !pairingProblems[i].isEmpty() ? request.labelFile : QString());
    }

    // Items are "imported" when checked, and stay so only once their file is written: the
    // others turn "not written" when the import stops.
    const auto stopFrom = [&report](qsizetype first, const QString& reason) {
        for (qsizetype k = first; k < report.items.size(); ++k) {
            if (report.items[k].result == ImportResult::Imported) {
                report.items[k].result = ImportResult::NotWritten;
                report.items[k].reason = reason;
            }
        }
    };
    const QString cancelled = QStringLiteral(u"导入已取消");
    const int total = int(scope.images.size()) * 2;
    QList<QList<Shape>> imported(scope.images.size());
    QList<ImageInfo> checkedInfo(scope.images.size());
    report.items.reserve(scope.images.size());
    for (qsizetype i = 0; i < scope.images.size(); ++i) {
        if (progress && !progress(int(i), total)) {
            report.cancelled = true;
            stopFrom(0, cancelled);
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
            done(ImportResult::NoLabelFile, noLabelReasons.value(i));
            continue;
        }
        if (!pairingProblems[i].isEmpty()) {
            done(ImportResult::Unconvertible, pairingProblems[i]);
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
        formats::ImportedLabels labels;
        if (oneFile) {
            const formats::CocoImageLabels& entry = coco.images[cocoEntry[i]];
            labels = entry.labels;
            if (labels.error.isEmpty() && (entry.width != info.width || entry.height != info.height)) {
                labels.error = QStringLiteral(u"COCO 里的尺寸 %1×%2 和图片的 %3×%4 不一致")
                                   .arg(entry.width)
                                   .arg(entry.height)
                                   .arg(info.width)
                                   .arg(info.height);
            }
        } else if (format == ImportFormat::Mask) {
            const RgbImage mask = m_readMask(item.labelFile);
            if (!mask.error.isEmpty()) {
                labels.error = mask.error;
            } else if (mask.width != info.width || mask.height != info.height) {
                labels.error = QStringLiteral(u"掩膜的尺寸 %1×%2 和图片的 %3×%4 不一致")
                                   .arg(mask.width)
                                   .arg(mask.height)
                                   .arg(info.width)
                                   .arg(info.height);
            } else {
                labels = formats::maskLabels(mask, mapping);
            }
        } else {
            QString error;
            const std::optional<QByteArray> bytes = readFile(item.labelFile, error);
            if (!bytes) {
                done(ImportResult::Unconvertible, QStringLiteral(u"无法读取标注文件：%1").arg(error));
                continue;
            }
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
            case ImportFormat::YoloPose:
                labels = formats::parseYoloPoseLabels(*bytes, pose, info.width, info.height);
                break;
            case ImportFormat::CocoPose:
            case ImportFormat::CocoDetection:
            case ImportFormat::CocoSegmentation:
            case ImportFormat::Mask:
                break;
            }
        }
        if (labels.error.isEmpty())
            labels.error = projectRefuses(labels.shapes, config);
        if (!labels.error.isEmpty()) {
            done(ImportResult::Unconvertible, labels.error);
            continue;
        }
        // Tried on a copy, so that a shape the document refuses stops the import here, before
        // anything is written.
        AnnotationDocument trial = image.annotation->file.document();
        if (!addImported(trial, labels.shapes, request.mode)) {
            done(ImportResult::Unconvertible, QStringLiteral(u"有对象无法加入标注（坐标换算到图片后超出范围）"));
            continue;
        }
        if (!trial.isModified() && !image.annotation->file.needsWrite()) {
            done(ImportResult::Unchanged, QStringLiteral(u"标注文件里没有对象，没有要写入的变化"));
            continue;
        }
        item.shapes = int(labels.shapes.size());
        imported[i] = std::move(labels.shapes);
        checkedInfo[i] = info;
        done(ImportResult::Imported);
    }
    if (const int refused = report.count(ImportResult::Unconvertible); refused > 0 && !request.skipUnconvertible) {
        report.error = QStringLiteral(u"有 %1 张图片的标注无法导入，没有写入任何标注文件。").arg(refused);
        stopFrom(0, QStringLiteral(u"其他图片的标注无法导入，没有写入"));
        return report;
    }

    // Write: each file is read again and saved with the checks of a normal save, so a change
    // made since the check above is never overwritten.
    for (qsizetype i = 0; i < scope.images.size(); ++i) {
        if (progress && !progress(int(scope.images.size() + i), total)) {
            report.cancelled = true;
            stopFrom(i, cancelled);
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
        // The coordinates were scaled to the image as it was checked.
        const ImageInfo& info = image.annotation->file.image();
        if (info.width != checkedInfo[i].width || info.height != checkedInfo[i].height) {
            item.result = ImportResult::NotWritten;
            item.reason = QStringLiteral(u"检查之后图片变了（%1×%2 → %3×%4），请重新导入")
                              .arg(checkedInfo[i].width)
                              .arg(checkedInfo[i].height)
                              .arg(info.width)
                              .arg(info.height);
            continue;
        }
        // Straight on the document: no undo history is needed here.
        formats::LoadedAnnotation& annotation = *image.annotation;
        if (!addImported(annotation.file.document(), imported[i], request.mode)) {
            item.result = ImportResult::NotWritten;
            item.reason = QStringLiteral(u"有对象无法加入标注");
            continue;
        }
        if (!annotation.file.needsWrite()) {
            item.result = ImportResult::Unchanged;
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
