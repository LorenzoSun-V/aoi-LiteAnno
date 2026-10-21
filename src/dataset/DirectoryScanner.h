#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace liteanno::dataset {

struct ScannedImage {
    QString path;             // image identifier: relative to the data root, '/' separators
    bool nameCollision = false;  // STEM_COLLISION or NAME_COLLISION (format spec 2)
};

struct ScanResult {
    QList<ScannedImage> images;  // in natural order
    QStringList unsupported;     // recognized but unsupported image types (format spec 9.3)
};

// Lists the images under a data root (format spec 2): every extension of 9.1 in any
// sub-directory, except liteanno/, upstream's EXIF backup directories and hidden entries.
// The first stage of architecture 6.1; the catalog comes later.
ScanResult scanDirectory(const QString& root);

bool isImageFileName(const QString& fileName);

} // namespace liteanno::dataset
