#pragma once

#include "application/ExportService.h"
#include "core/RgbImage.h"
#include "formats/Importers.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace liteanno::application {

// Reads a mask image pixel for pixel (imaging::readMaskImage); the application layer does not
// decode images itself.
using MaskImageReader = std::function<RgbImage(const QString& absolutePath)>;

// What happens to an image that already has an annotation file (architecture 11.2).
enum class ImportMode {
    OnlyNew,  // leave it alone; import only images without one
    Append,   // add the imported shapes after its own
    Replace,  // remove its shapes and put the imported ones in their place
};
QString importModeName(ImportMode mode);  // "new", "append", "replace"
std::optional<ImportMode> importModeFromName(const QString& name);

struct ImportRequest {
    formats::ImportFormat format = formats::ImportFormat::YoloDetection;
    // The folder of the label files, one per image (paired by relative path, then by file
    // name at the top of the folder); for COCO the one JSON file.
    QString labelDir;
    QString labelFile;
    // YOLO: the classes.txt that names the class numbers; empty: classes.txt in the label
    // folder, else the project's classes.
    QString classesFile;
    // Masks: the mapping of values or colours to classes; empty: mask_colors.json in the
    // label folder or the folder above it.
    QString mappingFile;
    // Pose: the classes and their keypoint names (X-AnyLabeling's pose YAML). Needed for YOLO
    // pose; for COCO keypoints, empty takes the names from the file's categories.
    QString poseConfigFile;
    QStringList batches;  // list stems ("b000003"); empty: the whole project
    ImportMode mode = ImportMode::OnlyNew;
    // Images whose edits the window holds in memory only (their save failed): left alone
    // like images with a recovery record.
    QSet<QString> heldImages;
    bool skipUnconvertible = false;  // "跳过这些图片继续导入"
};

enum class ImportResult {
    Imported,       // its annotation file was written
    Unchanged,      // the labels change nothing (an empty label file for an image without one)
    NoLabelFile,    // nothing to import for this image
    HasAnnotation,  // has an annotation file and the mode leaves it alone
    NotWritten,     // missing, read-only, name collision, unsaved window edits, write failed
    Unconvertible,  // the label file cannot be imported
};

// A COCO image record that names no image of the scope.
struct UnmatchedLabel {
    QString fileName;
    QString reason;
};
const char* importResultName(ImportResult result);

struct ImportItem {
    QString image;
    ImportResult result = ImportResult::NoLabelFile;
    QString labelFile;  // absolute, when one was found
    QString reason;
    int shapes = 0;  // imported shapes
};

// Where the time of an import went, for diagnosing slow imports on a field PC
// (microseconds). Collected in memory; importTimingText() puts it in words afterwards.
struct ImportFileTiming {
    QString image;
    qint64 readUs = 0;  // reading the image and its annotation file again before writing
    qint64 saveUs = 0;  // the whole save, all attempts
    formats::SaveTiming save;
};

struct ImportTiming {
    qint64 prepareUs = 0;   // options, the folder scan, recovery records, pairing label files
    qint64 checkUs = 0;     // reading and checking every label file (the first half of the progress)
    qint64 writeUs = 0;     // writing the annotation files (the second half)
    qint64 progressUs = 0;  // in the progress callback (the window's progress dialog)
    int progressCalls = 0;
    QList<ImportFileTiming> files;  // each written or attempted file
};

struct ImportReport {
    // No annotation file was changed unless this is true.
    bool written = false;
    QString error;
    bool cancelled = false;
    QList<ImportItem> items;
    QList<UnmatchedLabel> unmatched;  // COCO only
    ImportTiming timing;

    int count(ImportResult result) const;
    QList<ImportItem> unconvertible() const;
};

// The timing of a finished import as plain text: totals, the time per step summed over all
// files, the attempts that failed and why, the slowest files, then one line per file.
QString importTimingText(const ImportReport& report);

// Turns label files into annotation files (architecture 11.2): reads the label file of every
// image of the scope, refuses when any cannot be imported (unless told to skip those images),
// then writes the changed annotation files one by one with the checks of a normal save.
// Imported and changed images are waiting for confirmation afterwards.
class ImportService
{
public:
    // Called with the images handled so far; returning false cancels. A cancel while writing
    // keeps the files written so far.
    using Progress = std::function<bool(int done, int total)>;

    ImportService(const ProjectSession& project, ImageInfoReader readImageInfo, MaskImageReader readMask = {});

    ImportReport run(const ImportRequest& request, const Progress& progress = {}) const;

private:
    const ProjectSession& m_project;
    ImageInfoReader m_readImageInfo;
    MaskImageReader m_readMask;
};

} // namespace liteanno::application
