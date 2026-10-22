#include "application/SaveCoordinator.h"

#include "dataset/DirectoryScanner.h"

#include "core/FormatVersion.h"
#include "core/ImagePath.h"
#include "formats/RecoveryRecord.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

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
    return formats::existingAnnotationPath(m_location.rootPath + QLatin1Char('/') + annotationPathForImage(imagePath));
}

namespace {

// A record at this image's path that this program cannot use (a newer format, damaged, of
// another location or image) may hold edits nobody delivered yet. It is moved to
// "unusable/" next to the records instead of being replaced. Returns an error when it
// stays in the way.
QString moveAsideIfNotOurs(const QString& recordPath, const ProjectLocation& location, const QString& imagePath)
{
    QFile file(recordPath);
    if (!file.exists())
        return QString();
    if (file.open(QIODevice::ReadOnly)) {
        const formats::RecoveryRecordResult parsed = formats::parseRecoveryRecord(file.readAll());
        file.close();
        if (parsed.record && parsed.record->projectId == location.projectId
            && parsed.record->bindingId == location.bindingId && parsed.record->imagePath == imagePath)
            return QString();  // an earlier save of this image; the new record supersedes it
    }
    const QFileInfo info(recordPath);
    const QString asideDir = info.dir().filePath(QStringLiteral("unusable"));
    if (!QDir().mkpath(asideDir))
        return QStringLiteral("an unusable recovery record is in the way and cannot be moved: ") + recordPath;
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    QString target = asideDir + QLatin1Char('/') + info.completeBaseName() + QLatin1Char('-') + stamp + QLatin1String(".json");
    for (int i = 2; QFile::exists(target); ++i) {
        target = asideDir + QLatin1Char('/') + info.completeBaseName() + QLatin1Char('-') + stamp + QLatin1Char('-')
                 + QString::number(i) + QLatin1String(".json");
    }
    if (!QFile::rename(recordPath, target))
        return QStringLiteral("an unusable recovery record is in the way and cannot be moved: ") + recordPath;
    return QString();
}

} // namespace

bool SaveCoordinator::discardRecoveryRecord(const QString& imagePath) const
{
    const QString path = recoveryRecordPath(imagePath);
    return !QFile::exists(path) || QFile::remove(path);
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
    const QString inTheWay = moveAsideIfNotOurs(recordPath, m_location, imagePath);
    if (!inTheWay.isEmpty())
        return {{SaveStatus::Failed, inTheWay}, false};
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

    // Format spec 2, checked before every write attempt: a same-name image may have
    // appeared after the window handed this save over, or while a retry waited. The edit
    // stays in the record.
    bool collision = false;
    formats::SaveOptions annotationOptions = options;
    annotationOptions.mayWrite = [&, outer = options.mayWrite]() -> SaveResult {
        if (dataset::hasNameCollision(m_location.rootPath + QLatin1Char('/') + imagePath)) {
            collision = true;
            return {SaveStatus::ReadOnly, QStringLiteral(u"同一目录下有同名图片，不能写入共用的标注文件")};
        }
        return outer ? outer() : SaveResult{SaveStatus::Saved, QString()};
    };
    const SaveResult result = formats::saveAnnotationFile(annotationFilePath(imagePath), annotation, annotationOptions);
    if (result.status != SaveStatus::Saved) {
        SaveOutcome failed{result, true};
        failed.nameCollision = collision;
        return failed;
    }
    if (stageHook)
        stageHook(SaveStage::AnnotationWritten);

    QFile::remove(recordPath);
    return {result, false};
}

} // namespace liteanno::application
