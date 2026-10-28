#include "core/FolderCase.h"

#ifdef Q_OS_WIN
#include <QDir>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace liteanno {

bool caseSensitiveFolder(const QString& folder)
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
        return true;
    const bool read = GetFileInformationByHandleEx(handle, kCaseSensitiveInfo, &info, sizeof(info));
    const DWORD error = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (read)
        return (info.flags & kCaseSensitiveDir) != 0;
    return error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION;
}

} // namespace liteanno
#endif
