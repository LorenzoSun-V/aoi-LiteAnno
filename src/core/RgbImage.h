#pragma once

#include <QString>

#include <cstdint>
#include <vector>

namespace liteanno {

// 8-bit RGB pixels, row by row, three bytes each: what OpenCV's imread gives (in its BGR
// order) for an 8-bit image file, without alpha. Empty with an error when the file cannot be
// read that way.
struct RgbImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
    QString error;
};

} // namespace liteanno
