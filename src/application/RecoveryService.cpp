#include "application/RecoveryService.h"

#include "core/ImagePath.h"

#include <QDir>
#include <QFile>

namespace liteanno::application {

using formats::FileStamp;
using formats::RecoveryRecord;

RecoveryService::RecoveryService(LocalDataPaths paths, ProjectLocation location)
    : m_paths(std::move(paths))
    , m_location(std::move(location))
{
}

RecoveryState RecoveryService::classify(const RecoveryRecord& record, const FileStamp& current) const
{
    if (current.exists && current.digest == formats::contentDigest(record.annotation))
        return RecoveryState::AlreadySaved;
    const bool unchanged = record.baseDigest.isEmpty() ? !current.exists
                                                       : current.exists && current.digest == record.baseDigest;
    return unchanged ? RecoveryState::Restorable : RecoveryState::Conflict;
}

RecoveryScan RecoveryService::scan() const
{
    RecoveryScan result;
    const QDir dir(m_paths.recoveryDir(m_location.projectId, m_location.bindingId));
    const QStringList names = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& name : names) {
        const QString recordPath = dir.filePath(name);
        QFile file(recordPath);
        if (!file.open(QIODevice::ReadOnly)) {
            result.unusable.append({recordPath, file.errorString()});
            continue;
        }
        formats::RecoveryRecordResult parsed = formats::parseRecoveryRecord(file.readAll());
        if (!parsed.record) {
            result.unusable.append({recordPath, parsed.error});
            continue;
        }
        RecoveryRecord& record = *parsed.record;
        if (record.projectId != m_location.projectId || record.bindingId != m_location.bindingId) {
            result.unusable.append({recordPath, QStringLiteral("belongs to another project location")});
            continue;
        }
        // The path comes from a file on disk: never let it point outside the data root.
        if (!isValidImagePath(record.imagePath)) {
            result.unusable.append({recordPath, QStringLiteral("invalid image path: ") + record.imagePath});
            continue;
        }

        RecoveryCandidate candidate;
        candidate.recordPath = recordPath;
        candidate.annotationPath = m_location.rootPath + QLatin1Char('/') + annotationPathForImage(record.imagePath);
        candidate.current = formats::readFileStamp(candidate.annotationPath);
        candidate.state = classify(record, candidate.current);
        candidate.record = std::move(record);
        result.candidates.append(std::move(candidate));
    }
    return result;
}

RestoreResult RecoveryService::restore(const RecoveryCandidate& candidate, bool overwriteConflict,
                                       const formats::SaveOptions& options) const
{
    // Check again: the file may have changed since the scan.
    const FileStamp current = formats::readFileStamp(candidate.annotationPath);
    const RecoveryState state = classify(candidate.record, current);
    if (state == RecoveryState::Conflict && !overwriteConflict)
        return {RestoreStatus::Conflict, QStringLiteral("the file was changed after the edit began")};
    if (state != RecoveryState::AlreadySaved) {
        const formats::SaveResult written =
            formats::writeFileAtomically(candidate.annotationPath, candidate.record.annotation, options);
        if (written.status != formats::SaveStatus::Saved)
            return {RestoreStatus::Failed, written.error};
    }
    QFile::remove(candidate.recordPath);
    return {RestoreStatus::Restored, QString()};
}

bool RecoveryService::discard(const RecoveryCandidate& candidate) const
{
    return QFile::remove(candidate.recordPath);
}

int RecoveryService::removeAlreadySaved(const RecoveryScan& scan) const
{
    int removed = 0;
    for (const RecoveryCandidate& candidate : scan.candidates) {
        if (candidate.state == RecoveryState::AlreadySaved && QFile::remove(candidate.recordPath))
            ++removed;
    }
    return removed;
}

} // namespace liteanno::application
