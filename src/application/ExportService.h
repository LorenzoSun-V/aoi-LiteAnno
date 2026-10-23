#pragma once

#include "application/ProjectSession.h"
#include "core/AnnotationStatus.h"
#include "core/ImageInfo.h"
#include "formats/AnnotationStore.h"
#include "formats/Exporters.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace liteanno::application {

// Reads the size and orientation of an image without decoding it where possible; the
// application layer has no image codecs, so the caller passes them in.
using ImageInfoReader = std::function<std::optional<ImageInfo>(const QString& absolutePath)>;

// One image of the project as its files are now (format spec 5.1).
struct ImageOnDisk {
    QString path;
    bool missing = false;
    bool nameCollision = false;
    std::optional<formats::LoadedAnnotation> annotation;  // nothing when missing
    QByteArray bytes;                                     // the annotation file as read
    AnnotationStatus status = AnnotationStatus::Unprocessed;
};

// `missing` and `nameCollision` as the scan found them.
ImageOnDisk readImageOnDisk(const ProjectSession& project, const QString& imagePath, bool missing, bool nameCollision,
                            const ImageInfoReader& readImageInfo);

// The images of a scope, in order: the listed batches, or the whole project. The folder is
// scanned for both; the whole project needs a complete scan (format spec 7.1, architecture
// 11). Empty with `error` set when the scope cannot be used.
struct ScopeImage {
    QString path;
    bool missing = false;  // not found by the scan
    bool nameCollision = false;
};
struct ExportScope {
    QList<ScopeImage> images;
    bool wholeProject = false;
    bool scanComplete = false;
    QList<std::pair<int, QString>> batches;  // number and batchId
    QString error;
};
ExportScope scopeOf(const ProjectSession& project, const QStringList& batchStems,
                    const std::function<bool()>& cancelled = {});

struct ExportRequest {
    formats::ExportFormat format = formats::ExportFormat::YoloDetection;
    QString outputDir;
    QStringList batches;             // list stems ("b000003"); empty: the whole project
    bool skipUnconvertible = false;  // "跳过这些图片继续导出" (architecture 11)
    bool copyImages = false;
    // A classes.txt whose line numbers are the class ids, as X-AnyLabeling takes them; empty:
    // the project file's classes. COCO then numbers as upstream does: detection from 1,
    // segmentation with "_background_" as 0.
    QString classesFile;
    // Pose formats: the pose configuration (classes and their keypoints), as X-AnyLabeling
    // takes it; a copy is written next to the export as pose_config.yaml.
    QString poseConfigFile;
};

enum class ExportResult {
    Exported,
    Negative,       // confirmed without shapes: an empty YOLO file, a COCO image without annotations
    Skipped,        // skipped by the user; not exported
    NotDone,        // not confirmed, missing, name collision...; not exported
    Unconvertible,  // holds objects the format cannot take
};
const char* exportResultName(ExportResult result);

struct ExportItem {
    QString image;
    ExportResult result = ExportResult::NotDone;
    QString status;  // annotationStatusName(), empty when the file was not read
    QString reason;
    int clampedObjects = 0;
    QStringList imageLabels;  // classification: the image-level labels chosen
};

struct ExportReport {
    // Nothing is in the output folder unless this is true (architecture 11).
    bool written = false;
    QString error;
    bool cancelled = false;
    QList<ExportItem> items;
    QString manifestPath;

    int count(ExportResult result) const;
    QList<ExportItem> unconvertible() const;
};

// Exports confirmed images (architecture 11): reads every image of the scope, refuses when
// any has an object the format cannot take (unless told to skip those images), then writes
// the files and liteanno-export.json. The output folder must be new or empty and outside the
// data folder.
class ExportService
{
public:
    // Called with the images handled so far; returning false cancels.
    using Progress = std::function<bool(int done, int total)>;

    ExportService(const ProjectSession& project, ImageInfoReader readImageInfo);

    ExportReport run(const ExportRequest& request, const Progress& progress = {}) const;

private:
    const ProjectSession& m_project;
    ImageInfoReader m_readImageInfo;
};

} // namespace liteanno::application
