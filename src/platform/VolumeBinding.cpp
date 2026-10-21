#include "platform/VolumeBinding.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace liteanno::platform {

namespace {

QString withLeadingSlash(QString path)
{
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    if (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

#ifdef Q_OS_WIN
QString lastErrorText()
{
    return QStringLiteral("Windows error %1").arg(GetLastError());
}
#endif

} // namespace

VolumeLocation locateOnVolume(const QString& directory)
{
    VolumeLocation location;
    const QString canonical = QFileInfo(directory).canonicalFilePath();
    if (canonical.isEmpty()) {
        location.error = QStringLiteral("directory not found: ") + directory;
        return location;
    }
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(canonical).toStdWString();
    wchar_t volumePath[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(native.c_str(), volumePath, MAX_PATH)) {
        location.error = lastErrorText();
        return location;
    }
    DWORD serial = 0;
    if (!GetVolumeInformationW(volumePath, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        location.error = lastErrorText();
        return location;
    }
    const QString volumeRoot = QDir::fromNativeSeparators(QString::fromWCharArray(volumePath));
    QString inside = canonical;
    if (inside.startsWith(volumeRoot, Qt::CaseInsensitive))
        inside = inside.mid(volumeRoot.size());
    location.volumeId = QStringLiteral("%1").arg(serial, 8, 16, QLatin1Char('0'));
    // NTFS, FAT and exFAT ignore case; the same directory typed differently is the same place.
    location.pathInVolume = withLeadingSlash(inside).toCaseFolded();
#else
    const QStorageInfo storage(canonical);
    if (!storage.isValid()) {
        location.error = QStringLiteral("no file system for ") + canonical;
        return location;
    }
    location.volumeId = QString::fromLocal8Bit(storage.device());
    location.pathInVolume = withLeadingSlash(QDir(storage.rootPath()).relativeFilePath(canonical));
    if (location.pathInVolume == QLatin1String("/."))
        location.pathInVolume = QStringLiteral("/");
#endif
    location.ok = true;
    return location;
}

QString computeBindingId(const QString& volumeId, const QString& pathInVolume)
{
    const QByteArray input = volumeId.toUtf8() + '\n' + pathInVolume.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex().left(32));
}

QString bindingIdForDirectory(const QString& directory, QString* error)
{
    const VolumeLocation location = locateOnVolume(directory);
    if (!location.ok) {
        if (error)
            *error = location.error;
        return QString();
    }
    return computeBindingId(location.volumeId, location.pathInVolume);
}

} // namespace liteanno::platform
