#pragma once

// Takes away the right to list a folder, as a folder of another user or a broken ACL does.
// Windows: a deny entry for Everyone, which also binds administrators. Elsewhere: file
// permissions, which do not stop root; check isActive() and skip the test when it is false.

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QString>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace liteanno::test {

class FolderBlocker
{
public:
    explicit FolderBlocker(const QString& path)
        : m_path(QDir::toNativeSeparators(path))
    {
#ifdef Q_OS_WIN
        // S-1-1-0 is Everyone; RD is "list folder".
        m_active = QProcess::execute(QStringLiteral("icacls"), {m_path, QStringLiteral("/deny"), QStringLiteral("*S-1-1-0:(RD)")})
                   == 0;
        // The deny entry must actually stop listing.
        if (m_active && !QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
            release();
#else
        if (geteuid() == 0)
            return;
        m_permissions = QFile::permissions(path);
        m_active = QFile::setPermissions(path, QFile::Permissions());
#endif
    }

    ~FolderBlocker() { release(); }
    FolderBlocker(const FolderBlocker&) = delete;
    FolderBlocker& operator=(const FolderBlocker&) = delete;

    bool isActive() const { return m_active; }

    void release()
    {
        if (!m_active)
            return;
        m_active = false;
#ifdef Q_OS_WIN
        QProcess::execute(QStringLiteral("icacls"), {m_path, QStringLiteral("/remove:d"), QStringLiteral("*S-1-1-0")});
#else
        QFile::setPermissions(m_path, m_permissions);
#endif
    }

private:
    QString m_path;
    bool m_active = false;
#ifndef Q_OS_WIN
    QFile::Permissions m_permissions;
#endif
};

} // namespace liteanno::test
