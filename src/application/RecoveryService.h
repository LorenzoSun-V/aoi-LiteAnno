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
};

struct RecoveryCandidate {
    QString recordPath;
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
};

enum class RestoreStatus {
    Restored,
    Conflict,  // the file changed since the scan or since the edit began; nothing written
    Failed,
};

struct RestoreResult {
    RestoreStatus status = RestoreStatus::Failed;
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

    // Writes the recorded content back. A conflict is overwritten only when the user chose
    // the recorded version.
    RestoreResult restore(const RecoveryCandidate& candidate, bool overwriteConflict = false,
                          const formats::SaveOptions& options = {}) const;

    // The user dropped the recorded edit.
    bool discard(const RecoveryCandidate& candidate) const;

    // Removes records the annotation file already holds; returns how many.
    int removeAlreadySaved(const RecoveryScan& scan) const;

private:
    RecoveryState classify(const formats::RecoveryRecord& record, const formats::FileStamp& current) const;

    LocalDataPaths m_paths;
    ProjectLocation m_location;
};

} // namespace liteanno::application
