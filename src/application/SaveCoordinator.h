#pragma once

#include "application/LocalDataPaths.h"
#include "formats/AnnotationStore.h"

#include <QString>

#include <functional>

namespace liteanno::application {

// Where the images of one project are and which local data belongs to them.
struct ProjectLocation {
    QString projectId;
    QString bindingId;
    QString rootPath;  // the data root, '/' separators
};

enum class SaveStage {
    RecoveryRecordWritten,  // the local copy exists, the annotation file is not replaced yet
    AnnotationWritten,      // the annotation file is replaced, the local copy not removed yet
};

struct SaveOutcome {
    formats::SaveResult result;
    bool recoveryRecordKept = false;  // the edit is safe in a local record (failure, external change)
};

// Saves one document the way architecture section 8.1 describes: the snapshot goes to a
// local recovery record first, then the annotation file is replaced atomically, then the
// record is removed. A crash or a pulled USB drive at any point leaves either the old file
// and a record, or the new file.
//
// Synchronous for now; the pending-save limit of architecture section 8.2 comes with the
// editing interface.
class SaveCoordinator
{
public:
    SaveCoordinator(LocalDataPaths paths, ProjectLocation location);

    // `imagePath` is the image identifier (format spec 3); the annotation file is next to the
    // image. `documentVersion` grows with each edit of the document.
    SaveOutcome save(const QString& imagePath, formats::LoadedAnnotation& annotation, qint64 documentVersion,
                     const formats::SaveOptions& options = {});

    // The record file of an image.
    QString recoveryRecordPath(const QString& imagePath) const;
    QString annotationFilePath(const QString& imagePath) const;

    // Called after each stage; tests use it to stop the process at that point.
    std::function<void(SaveStage)> stageHook;

private:
    LocalDataPaths m_paths;
    ProjectLocation m_location;
};

} // namespace liteanno::application
