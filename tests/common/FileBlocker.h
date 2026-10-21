#pragma once

// Makes a file temporarily unreplaceable or unreadable, the way a virus scanner or the
// search indexer can on Windows. Elsewhere file permissions stand in for it; they do not stop
// root, so check isActive() and skip the test when it is false.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <unistd.h>
#endif

namespace liteanno::test {

class FileBlocker
{
public:
    enum class Mode {
        Replace,  // the file can be read but not replaced
        Read,     // the file can be neither read nor replaced
    };

    FileBlocker(const QString& path, Mode mode)
    {
#ifdef Q_OS_WIN
        const DWORD share = mode == Mode::Replace ? FILE_SHARE_READ : 0;
        m_handle = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ,
                               share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        m_active = m_handle != INVALID_HANDLE_VALUE;
#else
        if (geteuid() == 0)
            return;
        m_target = mode == Mode::Replace ? QFileInfo(path).absolutePath() : path;
        m_permissions = QFile::permissions(m_target);
        const QFile::Permissions blocked =
            mode == Mode::Replace ? QFile::ReadOwner | QFile::ExeOwner : QFile::Permissions();
        m_active = QFile::setPermissions(m_target, blocked);
#endif
    }

    ~FileBlocker() { release(); }
    FileBlocker(const FileBlocker&) = delete;
    FileBlocker& operator=(const FileBlocker&) = delete;

    bool isActive() const { return m_active; }

    void release()
    {
        if (!m_active)
            return;
        m_active = false;
#ifdef Q_OS_WIN
        CloseHandle(m_handle);
#else
        QFile::setPermissions(m_target, m_permissions);
#endif
    }

private:
    bool m_active = false;
#ifdef Q_OS_WIN
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    QString m_target;
    QFile::Permissions m_permissions;
#endif
};

} // namespace liteanno::test
