#include "formats/AnnotationStore.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

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
    if (!out.open(QIODevice::WriteOnly))
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

LoadedAnnotation loadAnnotationFile(const QString& jsonPath, const ImageInfo& image,
                                    const QStringList& imageLabelOptions)
{
    QFile file(jsonPath);
    if (!file.exists())
        return {AnnotationFile::createNew(image, imageLabelOptions), FileStamp()};
    if (!file.open(QIODevice::ReadOnly))
        return {AnnotationFile::unreadable(image, file.errorString()), readFileStamp(jsonPath)};
    const QByteArray bytes = file.readAll();
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

SaveResult writeFileAtomically(const QString& path, const QByteArray& bytes, const SaveOptions& options)
{
    const int maxAttempts = qMax(1, options.maxAttempts);
    SaveResult result;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (attempt > 1) {
            if (options.beforeRetry)
                options.beforeRetry(attempt);
            QThread::msleep(static_cast<unsigned long>(qMax(0, options.retryDelayMs)));
        }
        result = writeOnce(path, bytes);
        result.attempts = attempt;
        if (result.status == SaveStatus::Saved)
            break;
    }
    return result;
}

SaveResult saveAnnotationFile(const QString& jsonPath, LoadedAnnotation& annotation, const SaveOptions& options)
{
    AnnotationFile& file = annotation.file;
    if (file.isReadOnly())
        return {SaveStatus::ReadOnly, QString::fromLatin1(readOnlyReasonName(file.readOnlyReason()))};
    if (!file.needsWrite())
        return {SaveStatus::NotModified, QString()};

    // A change made between this check and the rename below is not detected; the window is
    // one write of a small file.
    if (!options.overwriteExternalChanges) {
        // A scanner that opened the file exclusively blocks reading it too, for a moment.
        FileStamp current = readFileStamp(jsonPath);
        for (int attempt = 1; attempt < options.maxAttempts && current.exists && current.digest.isEmpty(); ++attempt) {
            QThread::msleep(static_cast<unsigned long>(qMax(0, options.retryDelayMs)));
            current = readFileStamp(jsonPath);
        }
        if (current.exists && current.digest.isEmpty())
            return {SaveStatus::Failed, QStringLiteral("the file cannot be read to check for changes")};
        if (!current.sameContent(annotation.stamp))
            return {SaveStatus::ExternallyModified, QStringLiteral("the file was changed by another program")};
    }

    const QByteArray bytes = file.toBytes();
    SaveResult result = writeFileAtomically(jsonPath, bytes, options);
    if (result.status != SaveStatus::Saved)
        return result;

    file.markSaved();
    annotation.stamp = stampOf(jsonPath, bytes);
    return result;
}

} // namespace liteanno::formats
