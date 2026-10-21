#include "application/LocalDataPaths.h"

#include <QStandardPaths>

namespace liteanno::application {

LocalDataPaths::LocalDataPaths(QString root)
    : m_root(std::move(root))
{
}

LocalDataPaths LocalDataPaths::standard()
{
    // GenericDataLocation is %LOCALAPPDATA% on Windows and ~/.local/share on Linux.
    return LocalDataPaths(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                          + QLatin1String("/LiteAnno"));
}

QString LocalDataPaths::recoveryDir(const QString& projectId, const QString& bindingId) const
{
    return m_root + QLatin1String("/recovery/") + projectId + QLatin1Char('/') + bindingId;
}

QString LocalDataPaths::cacheDir(const QString& projectId, const QString& bindingId) const
{
    return m_root + QLatin1String("/cache/") + projectId + QLatin1Char('/') + bindingId;
}

QString LocalDataPaths::lockFile(const QString& bindingId) const
{
    return m_root + QLatin1String("/locks/") + bindingId + QLatin1String(".lock");
}

} // namespace liteanno::application
