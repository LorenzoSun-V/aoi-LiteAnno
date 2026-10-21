#pragma once

#include <QString>

namespace liteanno::platform {

// Where a directory lives: the volume and the path inside it, without a drive letter, so a
// USB drive that comes back under another letter gives the same answer (format spec 8).
struct VolumeLocation {
    bool ok = false;
    QString volumeId;      // Windows: the volume serial number; elsewhere the device name
    QString pathInVolume;  // '/' separators, starting with '/'; case-folded on Windows
    QString error;
};

VolumeLocation locateOnVolume(const QString& directory);

// The "bindingId" of a data root (format spec 8): a SHA-256 digest of the volume and the
// path inside it, as 32 lowercase hex digits.
QString computeBindingId(const QString& volumeId, const QString& pathInVolume);

// Empty when the directory cannot be located.
QString bindingIdForDirectory(const QString& directory, QString* error = nullptr);

} // namespace liteanno::platform
