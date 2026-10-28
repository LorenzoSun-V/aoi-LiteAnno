#include "core/FolderCase.h"

#ifdef Q_OS_WIN
#include <QDir>

#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace liteanno {

namespace {
std::atomic<DWORD> g_failQuery{0};
} // namespace

FolderCase folderCase(const QString& folder)
{
    // FileCaseSensitiveInfo and FILE_CS_FLAG_CASE_SENSITIVE_DIR (Windows 10 1803 SDK), spelled
    // out as older SDK headers or target versions hide them.
    constexpr auto kCaseSensitiveInfo = static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
    constexpr ULONG kCaseSensitiveDir = 0x1;
    struct {
        ULONG flags;
    } info{};
    const QString native = QDir::toNativeSeparators(folder);
    const HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return FolderCase::Unknown;
    const DWORD injected = g_failQuery.load();
    const bool read = injected == 0 && GetFileInformationByHandleEx(handle, kCaseSensitiveInfo, &info, sizeof(info));
    const DWORD error = read ? ERROR_SUCCESS : injected != 0 ? injected : GetLastError();
    CloseHandle(handle);
    if (read)
        return (info.flags & kCaseSensitiveDir) != 0 ? FolderCase::Sensitive : FolderCase::Insensitive;
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION)
        return FolderCase::NoSetting;
    return FolderCase::Unknown;
}

void failFolderCaseQueryForTesting(unsigned long error)
{
    g_failQuery.store(error);
}

} // namespace liteanno
#endif
