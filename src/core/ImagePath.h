#pragma once

#include <QString>

namespace liteanno {

// Whether `path` is a valid image identifier: relative to the data root, '/' separators, no
// drive letter, no ".", ".." or empty segments, no leading or trailing '/' (format spec 3).
bool isValidImagePath(const QString& path);

// The annotation file of an image: same directory, the file name without its last extension,
// plus ".json" (format spec 2). "a.b.jpg" gives "a.b.json".
QString annotationPathForImage(const QString& imagePath);

} // namespace liteanno
