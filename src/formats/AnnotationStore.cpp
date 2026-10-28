#include "formats/AnnotationStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>

#include <algorithm>

#ifdef Q_OS_WIN
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace liteanno::formats {

namespace {

FileStamp stampOf(const QString& path, const QByteArray& bytes)
{
    const QFileInfo info(path);
    return {true, bytes.size(), info.lastModified(), contentDigest(bytes)};
}

SaveResult writeOnce(const QString& path, const QByteArray& bytes)
{
    QSaveFile out(path);
    // Unbuffered: a write cut short (disk full) shows in the count. Through the buffer it is
    // lost, and commit() would put the first part of the file in place of the old one.
    if (!out.open(QIODevice::WriteOnly | QIODevice::Unbuffered))
        return {SaveStatus::Failed, out.errorString()};
    if (out.write(bytes) != bytes.size()) {
        const QString error = out.errorString();
        out.cancelWriting();
        return {SaveStatus::Failed, error};
    }
    if (!out.commit())
        return {SaveStatus::Failed, out.errorString()};
    return {SaveStatus::Saved, QString()};
}

#ifdef Q_OS_WIN
// Whether the folder is set to tell case apart in names. Not known (the folder cannot be
// opened, another error): true, so the folder is listed. Windows versions and file systems
// without the setting have no such folders.
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
#endif

// fsync, or FlushFileBuffers on Windows: the bytes are on the disk, not only in the cache.
bool syncToDisk(int fd)
{
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    return handle != INVALID_HANDLE_VALUE && FlushFileBuffers(handle);
#else
    return ::fsync(fd) == 0;
#endif
}

} // namespace

QString contentDigest(const QByteArray& bytes)
{
    return QStringLiteral("sha256:")
           + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

FileStamp readFileStamp(const QString& path)
{
    QFile file(path);
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly))
        return {true, QFileInfo(path).size(), QFileInfo(path).lastModified(), QString()};
    return stampOf(path, file.readAll());
}

QString existingAnnotationPath(const QString& jsonPath)
{
    const QFileInfo info(jsonPath);
#ifdef Q_OS_WIN
    // Names ignore case: at most one file matches, and the search gives its name as it is on
    // disk, which Windows keeps when the file is replaced under it. One look-up, not a listing
    // of the folder: this runs for every image an import or export touches. The name holds
    // no wildcard characters, as Windows allows none in file names.
    WIN32_FIND_DATAW data;
    const QString native = QDir::toNativeSeparators(info.absoluteFilePath());
    const HANDLE find = FindFirstFileExW(reinterpret_cast<const wchar_t*>(native.utf16()), FindExInfoBasic, &data,
                                         FindExSearchNameMatch, nullptr, 0);
    if (find != INVALID_HANDLE_VALUE) {
        FindClose(find);
        const QString onDisk = QString::fromWCharArray(data.cFileName);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            if (onDisk != info.fileName() && onDisk.compare(info.fileName(), Qt::CaseInsensitive) == 0)
                return info.dir().filePath(onDisk);
            return jsonPath;
        }
    }
    // Not found, or a folder of that name. In a folder set to tell case apart (fsutil
    // setCaseSensitiveInfo) the look-up finds the exact name only, while the scan pairs
    // "a.JSON" too: the folder is listed as before, so editing, import and export use the
    // file the scan shows.
    if (!caseSensitiveFolder(info.path()))
        return jsonPath;
    QString pattern = info.fileName();
    // Wildcard characters in the name are matched literally.
    for (const QChar special : {QLatin1Char('['), QLatin1Char('*'), QLatin1Char('?')})
        pattern.replace(special, QLatin1Char('[') + special + QLatin1Char(']'));
    // Name filters ignore case; the scan pairs a differing stem too in such a folder.
    const QStringList names = info.dir().entryList({pattern}, QDir::Files | QDir::Hidden, QDir::Name);
    if (names.isEmpty() || names.contains(info.fileName()))
        return jsonPath;
    // Sorted, so the choice between "a.JSON" and "a.Json" is the same every time.
    return info.dir().filePath(names.first());
#else
    // Case-sensitive names: the lower-case file is the one (a folder of that name is not).
    if (info.isFile())
        return jsonPath;
    // Otherwise a file whose extension differs only in case. The 16 spellings of "json" are
    // looked up one by one instead of listing the folder, as this runs for every image without
    // an annotation file that an import or export touches.
    const QByteArray base = QFile::encodeName(info.path() + QLatin1Char('/') + info.completeBaseName()) + '.';
    QStringList names;
    for (int mask = 1; mask < 16; ++mask) {
        QByteArray path = base + "json";
        for (int i = 0; i < 4; ++i) {
            if (mask & (1 << i))
                path[base.size() + i] = char(path[base.size() + i] - 'a' + 'A');
        }
        struct stat st;
        if (stat(path.constData(), &st) == 0 && S_ISREG(st.st_mode))
            names.append(QFile::decodeName(path.mid(path.lastIndexOf('/') + 1)));
    }
    if (names.isEmpty())
        return jsonPath;
    // Sorted, so the choice between "a.JSON" and "a.Json" is the same every time.
    std::sort(names.begin(), names.end());
    return info.dir().filePath(names.first());
#endif
}

LoadedAnnotation loadAnnotationFile(const QString& jsonPath, const ImageInfo& image,
                                    const QStringList& imageLabelOptions, QByteArray* bytesRead)
{
    QFile file(jsonPath);
    if (!file.exists())
        return {AnnotationFile::createNew(image, imageLabelOptions), FileStamp()};
    if (!file.open(QIODevice::ReadOnly))
        return {AnnotationFile::unreadable(image, file.errorString()), readFileStamp(jsonPath)};
    const QByteArray bytes = file.readAll();
    if (bytesRead)
        *bytesRead = bytes;
    return {AnnotationFile::fromBytes(bytes, image), stampOf(jsonPath, bytes)};
}

const char* saveStatusName(SaveStatus status)
{
    switch (status) {
    case SaveStatus::Saved:
        return "saved";
    case SaveStatus::NotModified:
        return "not-modified";
    case SaveStatus::ReadOnly:
        return "read-only";
    case SaveStatus::ExternallyModified:
        return "externally-modified";
    case SaveStatus::Failed:
        return "failed";
    }
    return "unknown";
}

SaveResult writeFileAtomically(const QString& path, const QByteArray& bytes, const SaveOptions& options,
                               const WriteCheck& check)
{
    const int maxAttempts = qMax(1, options.maxAttempts);
    SaveResult result;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (attempt > 1) {
            if (options.beforeRetry)
                options.beforeRetry(attempt);
            QThread::msleep(static_cast<unsigned long>(qMax(0, options.retryDelayMs)));
        }
        // Checked before every attempt: the target can change while a retry waits.
        if (check) {
            result = check();
            result.attempts = attempt;
            if (result.status == SaveStatus::Failed)
                continue;
            if (result.status != SaveStatus::Saved)
                return result;
        }
        if (options.mayWrite) {
            result = options.mayWrite();
            result.attempts = attempt;
            if (result.status != SaveStatus::Saved)
                return result;
        }
        result = writeOnce(path, bytes);
        result.attempts = attempt;
        if (result.status == SaveStatus::Saved)
            break;
    }
    return result;
}

SaveResult createFileExclusively(const QString& path, const QByteArray& bytes, const std::function<void()>& beforePublish)
{
    if (QFileInfo::exists(path))
        return {SaveStatus::ExternallyModified, QStringLiteral("the file exists")};
    // Next to the target, so the name can be given without copying.
    QTemporaryFile temporary(path + QStringLiteral(".XXXXXX"));
    if (!temporary.open())
        return {SaveStatus::Failed, temporary.errorString()};
    // flush() reports a write cut short (disk full), which the buffered count does not.
    if (temporary.write(bytes) != bytes.size() || !temporary.flush()) {
        const QString error = temporary.errorString();
        return {SaveStatus::Failed, error.isEmpty() ? QStringLiteral("the file was not written whole") : error};
    }
    // On the disk before it gets its name (format spec 1), as QSaveFile does for the others.
    if (!syncToDisk(temporary.handle()))
        return {SaveStatus::Failed, QStringLiteral("the file could not be written to the disk")};
    temporary.close();
    // A temporary file is only for its owner; the project file is read by others as usual.
    temporary.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser
                             | QFile::ReadGroup | QFile::ReadOther);
    if (beforePublish)
        beforePublish();
    // QFile::rename never replaces a file: renameat2(RENAME_NOREPLACE) or link() on Unix,
    // MoveFileEx without MOVEFILE_REPLACE_EXISTING on Windows; the check and the naming are one.
    if (!temporary.rename(path)) {
        if (QFileInfo::exists(path))
            return {SaveStatus::ExternallyModified, QStringLiteral("the file exists")};
        return {SaveStatus::Failed, temporary.errorString()};
    }
    temporary.setAutoRemove(false);
    return {SaveStatus::Saved, QString(), 1};
}

SaveResult checkUnchanged(const QString& path, const FileStamp& expected)
{
    const FileStamp current = readFileStamp(path);
    // A scanner that opened the file exclusively blocks reading it for a moment: retried.
    if (current.exists && current.digest.isEmpty())
        return {SaveStatus::Failed, QStringLiteral("the file cannot be read to check for changes")};
    if (!current.sameContent(expected))
        return {SaveStatus::ExternallyModified, QStringLiteral("the file was changed by another program")};
    return {SaveStatus::Saved, QString()};
}

SaveResult saveAnnotationFile(const QString& jsonPath, LoadedAnnotation& annotation, const SaveOptions& options)
{
    AnnotationFile& file = annotation.file;
    if (file.isReadOnly())
        return {SaveStatus::ReadOnly, QString::fromLatin1(readOnlyReasonName(file.readOnlyReason()))};
    if (!file.needsWrite())
        return {SaveStatus::NotModified, QString()};

    // A change made between a check and the rename right after it is not detected; the
    // window is one write of a small file.
    WriteCheck check;
    if (!options.overwriteExternalChanges)
        check = [&jsonPath, &annotation] { return checkUnchanged(jsonPath, annotation.stamp); };

    const QByteArray bytes = file.toBytes();
    SaveResult result = writeFileAtomically(jsonPath, bytes, options, check);
    if (result.status != SaveStatus::Saved)
        return result;

    file.markSaved();
    annotation.stamp = stampOf(jsonPath, bytes);
    return result;
}

} // namespace liteanno::formats
