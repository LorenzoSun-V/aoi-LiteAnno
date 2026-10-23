#pragma once

#include "core/ImageInfo.h"
#include "core/RgbImage.h"

#include <QImage>
#include <QString>
#include <QStringList>

#include <optional>

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

// Only the size after orientation and the orientation, from the file header where the
// format allows it (the second scan stage checks sizes without decoding). Empty when the
// file is not a readable image.
std::optional<ImageInfo> readImageInfo(const QString& path);

// A mask image for the mask import, pixel for pixel as stored: an 8-bit PNG (grey, grey with
// alpha, palette, RGB or RGBA), alpha left out, no orientation applied. Other formats and
// 16-bit PNGs are refused: a lossy or scaled value would be a different class.
RgbImage readMaskImage(const QString& path);

// The decoders the folder scan counts on (dataset/DirectoryScanner.cpp) that Qt does not
// have here; empty when all are there. A single-file build must have them linked in.
QStringList missingImageFormats();

} // namespace liteanno::imaging
