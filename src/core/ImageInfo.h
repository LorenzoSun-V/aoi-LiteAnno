#pragma once

#include <QString>

namespace liteanno {

// What the image loader knows about the image an annotation file belongs to.
struct ImageInfo {
    QString fileName;         // file name only, e.g. "0001.bmp"; saved as "imagePath"
    int width = 0;            // size after applying the EXIF orientation; 0 when unknown
    int height = 0;
    int exifOrientation = 1;  // EXIF orientation tag 1..8; 1 when the image has none
};

} // namespace liteanno
