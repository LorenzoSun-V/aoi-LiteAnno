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

namespace {

// Whether the record file still holds what was scanned.
bool recordUnchanged(const RecoveryCandidate& candidate)
{
    QFile file(candidate.recordPath);
    return file.open(QIODevice::ReadOnly) && formats::contentDigest(file.readAll()) == candidate.recordDigest;
}

} // namespace

RecoveryState RecoveryService::classify(const RecoveryRecord& record, const FileStamp& current) const
{
    if (current.exists && current.digest.isEmpty())
        return RecoveryState::TargetUnreadable;
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
        const QByteArray bytes = file.readAll();
        formats::RecoveryRecordResult parsed = formats::parseRecoveryRecord(bytes);
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
        candidate.recordDigest = formats::contentDigest(bytes);
        candidate.annotationPath = m_location.rootPath + QLatin1Char('/') + annotationPathForImage(record.imagePath);
        candidate.current = formats::readFileStamp(candidate.annotationPath);
        candidate.state = classify(record, candidate.current);
        candidate.record = std::move(record);
        result.candidates.append(std::move(candidate));
    }
    return result;
}

RecordResult RecoveryService::restore(const RecoveryCandidate& candidate, bool overwriteConflict,
                                      const formats::SaveOptions& options) const
{
    using formats::SaveResult;
    using formats::SaveStatus;

    // Run before every write attempt: the record and the file can change while a retry waits.
    RecordStatus stopped = RecordStatus::Failed;
    bool alreadySaved = false;
    const formats::WriteCheck check = [&]() -> SaveResult {
        if (!recordUnchanged(candidate)) {
            stopped = RecordStatus::Stale;
            return {SaveStatus::ReadOnly, QStringLiteral("the recovery record changed since the scan")};
        }
        switch (classify(candidate.record, formats::readFileStamp(candidate.annotationPath))) {
        case RecoveryState::TargetUnreadable:
            return {SaveStatus::Failed, QStringLiteral("the file cannot be read to check for changes")};
        case RecoveryState::AlreadySaved:
            alreadySaved = true;
            return {SaveStatus::NotModified, QString()};
        case RecoveryState::Conflict:
            if (!overwriteConflict) {
                stopped = RecordStatus::Conflict;
                return {SaveStatus::ExternallyModified, QStringLiteral("the file was changed after the edit began")};
            }
            break;
        case RecoveryState::Restorable:
            break;
        }
        return {SaveStatus::Saved, QString()};
    };

    const SaveResult written =
        formats::writeFileAtomically(candidate.annotationPath, candidate.record.annotation, options, check);
    if (written.status != SaveStatus::Saved && !alreadySaved)
        return {written.status == SaveStatus::Failed ? RecordStatus::Failed : stopped, written.error};
    QFile::remove(candidate.recordPath);
    return {RecordStatus::Done, QString()};
}

RecordResult RecoveryService::discard(const RecoveryCandidate& candidate) const
{
    if (!recordUnchanged(candidate))
        return {RecordStatus::Stale, QStringLiteral("the recovery record changed since the scan")};
    if (!QFile::remove(candidate.recordPath))
        return {RecordStatus::Failed, QStringLiteral("the recovery record cannot be removed")};
    return {RecordStatus::Done, QString()};
}

int RecoveryService::removeAlreadySaved(const RecoveryScan& scan) const
{
    int removed = 0;
    for (const RecoveryCandidate& candidate : scan.candidates) {
        // Checked again: since the scan a newer edit may have replaced the record, or the file.
        if (candidate.state != RecoveryState::AlreadySaved || !recordUnchanged(candidate)
            || classify(candidate.record, formats::readFileStamp(candidate.annotationPath))
                   != RecoveryState::AlreadySaved)
            continue;
        if (QFile::remove(candidate.recordPath))
            ++removed;
    }
    return removed;
}

} // namespace liteanno::application
