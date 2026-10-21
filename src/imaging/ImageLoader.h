#pragma once

#include "core/ImageInfo.h"

#include <QImage>
#include <QString>

namespace liteanno::imaging {

struct LoadedImage {
    QImage image;     // upright (EXIF orientation applied), ready to draw; null on failure
    ImageInfo info;   // size after orientation, and the EXIF orientation found (1 when none)
    QString error;    // why the image cannot be shown
    bool hasMoreFrames = false;  // multi-page TIFF, animated GIF/WebP: only the first is shown
};

// Reads an image for display and annotation (format spec 9.2): decoded by content when the
// extension is wrong, turned upright like OpenCV, first frame only. 16-bit and float images
// are mapped to 8 bits for display; coordinates do not change.
LoadedImage loadImage(const QString& path);

} // namespace liteanno::imaging
