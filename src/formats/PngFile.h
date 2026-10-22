#pragma once

#include <QByteArray>

#include <vector>

namespace liteanno::formats {

// An 8-bit grayscale PNG of `width` x `height` pixels, given row by row. Masks are written
// with it, so exporting needs no image library.
QByteArray grayscalePng(int width, int height, const std::vector<unsigned char>& pixels);

} // namespace liteanno::formats
