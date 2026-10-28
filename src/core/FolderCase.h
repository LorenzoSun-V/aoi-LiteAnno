#pragma once

#include <QString>

namespace liteanno {

#ifdef Q_OS_WIN
// Whether the folder is set to tell case apart in names (fsutil file setCaseSensitiveInfo).
// Not known (the folder cannot be opened, another error): true, so that callers fall back to
// listing the folder. Windows versions and file systems without the setting have no such
// folders.
bool caseSensitiveFolder(const QString& folder);
#endif

} // namespace liteanno
