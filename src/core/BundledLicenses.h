#pragma once

#include <QString>
#include <QStringList>

namespace liteanno {

// The licence texts the single-file build carries (":/licenses", cmake/LiteAnnoBundledFiles.cmake):
// file names, sorted.
QStringList bundledLicenseNames();

// A licence text by its name; empty when there is none.
QString bundledLicenseText(const QString& name);

// The texts every single-file build must carry that are missing or empty: Qt's LGPL and the
// GPL it builds on, and the licences of the libraries whose code is linked in with special
// notices (nlohmann/json, the OpenCV port, libjpeg-turbo, zlib).
QStringList missingRequiredLicenses();

} // namespace liteanno
