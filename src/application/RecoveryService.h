#pragma once

#include "application/SaveCoordinator.h"
#include "formats/AnnotationStore.h"
#include "formats/RecoveryRecord.h"

#include <QList>
#include <QString>

namespace liteanno::application {

enum class RecoveryState {
    Restorable,    // the file is as it was before the edit: the record can be written back
    AlreadySaved,  // the file already holds the recorded content; the record is left over
    Conflict,      // the file changed after the edit began: show both, the user decides
    TargetUnreadable,  // the file cannot be read now (held by another program, drive gone)
};

struct RecoveryCandidate {
    QString recordPath;
    QString recordDigest;  // of the record file as scanned, to notice a newer record
    formats::RecoveryRecord record;
    RecoveryState state = RecoveryState::Conflict;
    QString annotationPath;      // where the record would be written back
    formats::FileStamp current;  // the annotation file as it is now
};

struct UnusableRecord {
    QString recordPath;
    QString error;
};

struct RecoveryScan {
    QList<RecoveryCandidate> candidates;
    QList<UnusableRecord> unusable;  // kept on disk; only the user removes records
    // The record folder is there but cannot be listed: there may be records nobody sees.
    QString error;
};

enum class RecordStatus {
    Done,
    Conflict,  // the file changed after the edit began; nothing written
    Stale,     // the record changed since the scan (a newer edit was saved there); nothing done
    Failed,    // the file or the record cannot be read or written now; nothing removed
    NameCollision,  // a same-name image is next to the image (format spec 2); nothing written
};

struct RecordResult {
    RecordStatus status = RecordStatus::Failed;
    QString error;
};

// Finds and applies the recovery records of one project location (format spec 8.1).
// Records are never removed without a call from the user, except ones whose content the
// annotation file already holds.
class RecoveryService
{
public:
    RecoveryService(LocalDataPaths paths, ProjectLocation location);

    RecoveryScan scan() const;

    // Writes the recorded content back and removes the record. A conflict is overwritten
    // only when the user chose the recorded version. Every action checks the record and the
    // file again first, so a candidate from an old scan never removes a newer edit.
    RecordResult restore(const RecoveryCandidate& candidate, bool overwriteConflict = false,
                         const formats::SaveOptions& options = {}) const;

    // The user dropped the recorded edit.
    RecordResult discard(const RecoveryCandidate& candidate) const;

    // Removes records the annotation file still holds; returns how many.
    int removeAlreadySaved(const RecoveryScan& scan) const;

private:
    RecoveryState classify(const formats::RecoveryRecord& record, const formats::FileStamp& current) const;

    LocalDataPaths m_paths;
    ProjectLocation m_location;
};

} // namespace liteanno::application
