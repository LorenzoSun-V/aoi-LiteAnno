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
    // Images whose absolute path, or that of their annotation file (existing or to be
    // written), has more than 259 characters.
    QStringList longPaths;
    // Folders that could not be listed. Their images are unknown, so the scan is incomplete.
    QStringList unreadable;
    bool cancelled = false;
    // False when cancelled or when a folder could not be listed: what the scan did not see
    // may still be there (architecture 6.2).
    bool complete = true;
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

// Whether another image in the same folder as `imageFile` (an absolute path) has the same
// stem or the same name under the comparison of format spec 3: STEM_COLLISION or
// NAME_COLLISION found without a full scan. True also when the folder cannot be listed,
// since then nothing says the shared annotation file may be written.
bool hasNameCollision(const QString& imageFile);

// The search pattern hasNameCollision() asks Windows with for the images that may have the
// stem `stem`: the stem with a '?' for each character that a name not in ASCII also folds to
// (s, k, ';' and '`' in any case), then ".*". Empty for a stem that is empty or not ASCII,
// whose folder is listed in full instead.
QString nameQueryPattern(QStringView stem);

} // namespace liteanno::dataset
