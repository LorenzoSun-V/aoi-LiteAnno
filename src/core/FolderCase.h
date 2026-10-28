#pragma once

#include <QString>

namespace liteanno {

#ifdef Q_OS_WIN
// What Windows says about a folder's case setting (fsutil file setCaseSensitiveInfo). Only
// Insensitive lets a look-up by name stand for a listing of the folder.
enum class FolderCase {
    Insensitive,  // read: names ignore case
    Sensitive,    // read: the folder tells case apart
    NoSetting,    // the query is not supported here (ERROR_INVALID_PARAMETER, _NOT_SUPPORTED,
                  // _INVALID_FUNCTION): usually a file system without the setting, not proof
    Unknown,      // the folder cannot be opened, or another error
};

FolderCase folderCase(const QString& folder);

// Tests only: the query fails with `error` instead of asking Windows; 0 asks Windows again.
void failFolderCaseQueryForTesting(unsigned long error);
#endif

} // namespace liteanno
