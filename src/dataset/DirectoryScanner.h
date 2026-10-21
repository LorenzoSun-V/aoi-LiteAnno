#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace liteanno::dataset {

// Size and modification time of a file as listed, the version the catalog compares
// (architecture 6.1, 6.3). size is -1 when there is no file.
struct FileVersion {
    qint64 size = -1;
    qint64 modifiedMs = 0;  // milliseconds since the epoch, UTC

    bool exists() const { return size >= 0; }
    bool operator==(const FileVersion& other) const { return size == other.size && modifiedMs == other.modifiedMs; }
    bool operator!=(const FileVersion& other) const { return !(*this == other); }
};

struct ScannedImage {
    QString path;             // image identifier: relative to the data root, '/' separators
    bool nameCollision = false;  // STEM_COLLISION or NAME_COLLISION (format spec 2)
    FileVersion image;
    // The annotation file found next to it in the same listing (format spec 2): its name as
    // on disk, empty when there is none.
    QString annotationName;
    FileVersion annotation;
};

struct ScanResult {
    QList<ScannedImage> images;  // in natural order
    QStringList unsupported;     // recognized but unsupported image types (format spec 9.3)
    QStringList orphanAnnotations;  // .json files without an image (format spec 2)
    QStringList longPaths;       // images whose absolute path has more than 259 characters
    bool complete = true;        // false when the scan was cancelled
};

struct ScanOptions {
    // Asked between directories; returning true stops the scan, which is then incomplete.
    std::function<bool()> cancelled;
    // Called after each directory with the number of images found so far.
    std::function<void(int)> progress;
};

// Lists the images under a data root (format spec 2): every extension of 9.1 in any
// sub-directory, except liteanno/, upstream's EXIF backup directories and hidden entries.
// The first stage of architecture 6.1: one listing per directory gives the images, their
// annotation files and the sizes and times of both.
ScanResult scanDirectory(const QString& root, const ScanOptions& options = {});

bool isImageFileName(const QString& fileName);

} // namespace liteanno::dataset
