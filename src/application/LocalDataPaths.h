#pragma once

#include <QString>

namespace liteanno::application {

// Data this computer keeps outside the project (format spec 8): %LOCALAPPDATA%/LiteAnno on
// Windows. Tests pass their own root.
class LocalDataPaths
{
public:
    explicit LocalDataPaths(QString root);
    static LocalDataPaths standard();

    const QString& root() const { return m_root; }
    QString recoveryDir(const QString& projectId, const QString& bindingId) const;
    QString cacheDir(const QString& projectId, const QString& bindingId) const;
    QString lockFile(const QString& bindingId) const;

private:
    QString m_root;
};

} // namespace liteanno::application
