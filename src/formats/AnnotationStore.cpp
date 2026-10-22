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
#ifndef Q_OS_WIN
    // Case-sensitive names: the lower-case file is the one.
    if (info.exists())
        return jsonPath;
#endif
    QString pattern = info.fileName();
    // Wildcard characters in the name are matched literally.
    for (const QChar special : {QLatin1Char('['), QLatin1Char('*'), QLatin1Char('?')})
        pattern.replace(special, QLatin1Char('[') + special + QLatin1Char(']'));
    // Name filters ignore case. Listing also gives the name as it is on disk, which Windows
    // keeps when the file is replaced under it.
    QStringList names = info.dir().entryList({pattern}, QDir::Files | QDir::Hidden, QDir::Name);
    if (names.contains(info.fileName()))
        return jsonPath;
#ifndef Q_OS_WIN
    // The filter ignores case in the whole name; on Linux only the extension may differ.
    const QString stem = info.completeBaseName() + QLatin1Char('.');
    names.erase(std::remove_if(names.begin(), names.end(),
                               [&stem](const QString& name) { return !name.startsWith(stem, Qt::CaseSensitive); }),
                names.end());
#endif
    if (names.isEmpty())
        return jsonPath;
    // Sorted, so the choice between "a.JSON" and "a.Json" is the same every time.
    return info.dir().filePath(names.first());
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
