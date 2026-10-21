#include "application/SaveCoordinator.h"

#include "core/FormatVersion.h"
#include "core/ImagePath.h"
#include "formats/RecoveryRecord.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>

namespace liteanno::application {

using formats::SaveResult;
using formats::SaveStatus;

SaveCoordinator::SaveCoordinator(LocalDataPaths paths, ProjectLocation location)
    : m_paths(std::move(paths))
    , m_location(std::move(location))
{
}

QString SaveCoordinator::recoveryRecordPath(const QString& imagePath) const
{
    // One file per image; the name does not depend on characters the file system may reject.
    const QByteArray digest = QCryptographicHash::hash(imagePath.toUtf8(), QCryptographicHash::Sha256).toHex();
    return m_paths.recoveryDir(m_location.projectId, m_location.bindingId) + QLatin1Char('/')
           + QString::fromLatin1(digest.left(32)) + QLatin1String(".json");
}

QString SaveCoordinator::annotationFilePath(const QString& imagePath) const
{
    return m_location.rootPath + QLatin1Char('/') + annotationPathForImage(imagePath);
}

SaveOutcome SaveCoordinator::save(const QString& imagePath, formats::LoadedAnnotation& annotation,
                                  qint64 documentVersion, const formats::SaveOptions& options)
{
    if (!isValidImagePath(imagePath))
        return {{SaveStatus::Failed, QStringLiteral("invalid image path: ") + imagePath}, false};
    if (annotation.file.isReadOnly() || !annotation.file.needsWrite())
        return {formats::saveAnnotationFile(annotationFilePath(imagePath), annotation, options), false};

    formats::RecoveryRecord record;
    record.formatVersion = QString::fromLatin1(kFormatVersion);
    record.projectId = m_location.projectId;
    record.bindingId = m_location.bindingId;
    record.rootPathAtWrite = m_location.rootPath;
    record.imagePath = imagePath;
    record.documentVersion = documentVersion;
    record.baseDigest = annotation.stamp.exists ? annotation.stamp.digest : QString();
    const QDateTime now = QDateTime::currentDateTime();
    record.createdAt = now.toOffsetFromUtc(now.offsetFromUtc()).toString(Qt::ISODate);
    record.annotation = annotation.file.toBytes();

    const std::optional<QByteArray> recordBytes = formats::recoveryRecordToBytes(record);
    const QString recordPath = recoveryRecordPath(imagePath);
    if (!recordBytes)
        return {{SaveStatus::Failed, QStringLiteral("the document cannot be serialized")}, false};
    if (!QDir().mkpath(m_paths.recoveryDir(m_location.projectId, m_location.bindingId)))
        return {{SaveStatus::Failed, QStringLiteral("cannot create the recovery directory")}, false};
    // The local disk is not where the virus scanner problem is; one attempt is enough.
    formats::SaveOptions recordOptions;
    recordOptions.maxAttempts = 1;
    const SaveResult recordResult = formats::writeFileAtomically(recordPath, *recordBytes, recordOptions);
    if (recordResult.status != SaveStatus::Saved) {
        // Without the local copy the file is not touched: an interrupted write could not be undone.
        return {{SaveStatus::Failed, QStringLiteral("cannot write the recovery record: ") + recordResult.error},
                false};
    }
    if (stageHook)
        stageHook(SaveStage::RecoveryRecordWritten);

    const SaveResult result = formats::saveAnnotationFile(annotationFilePath(imagePath), annotation, options);
    if (result.status != SaveStatus::Saved)
        return {result, true};
    if (stageHook)
        stageHook(SaveStage::AnnotationWritten);

    QFile::remove(recordPath);
    return {result, false};
}

} // namespace liteanno::application
