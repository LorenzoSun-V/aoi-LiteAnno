#pragma once

#include "application/LocalDataPaths.h"
#include "application/ProjectLock.h"
#include "application/SaveCoordinator.h"
#include "core/ProjectConfig.h"

#include <QString>

#include <memory>

namespace liteanno::application {

// An open project: its configuration, where it is, and the lock that keeps a second window
// or the command line from changing it at the same time (format spec 6, 8).
class ProjectSession
{
public:
    enum class OpenError {
        None,
        ProjectFileUnreadable,  // liteanno/ is there (or cannot be checked), its project.json cannot be read
        InvalidProjectFile,  // field and message say what is wrong
        LocationUnknown,     // the volume of the folder cannot be identified
        Locked,              // holder says who has it open
        LockFailed,          // the local lock file cannot be created
    };

    struct OpenResult {
        std::unique_ptr<ProjectSession> session;
        OpenError error = OpenError::None;
        QString field;    // InvalidProjectFile: JSON pointer of the field
        QString message;  // details, in English
        ProjectLock::Holder holder;
    };

    // `root` is the data root, the folder that holds liteanno/. A folder without
    // liteanno/project.json opens as a plain image folder (plainFolderConfig()).
    static OpenResult open(const QString& root, const LocalDataPaths& paths);

    ~ProjectSession();

    const ProjectConfig& config() const { return m_config; }
    // Opened without a project file, as X-AnyLabeling opens a folder: any label, every shape
    // tool, no batches, and nothing written into the folder but the annotation files.
    bool isPlainFolder() const { return m_plainFolder; }
    // Of the project file as read: statuses derived under another one are not current.
    const QString& configDigest() const { return m_configDigest; }
    const ProjectLocation& location() const { return m_location; }
    const LocalDataPaths& paths() const { return m_paths; }
    QString rootPath() const { return m_location.rootPath; }

    // Absolute paths for an image identifier (format spec 3).
    QString imageFilePath(const QString& imagePath) const;
    QString annotationFilePath(const QString& imagePath) const;

private:
    ProjectSession(ProjectConfig config, QString configDigest, ProjectLocation location, LocalDataPaths paths,
                   std::unique_ptr<ProjectLock> lock, bool plainFolder);

    ProjectConfig m_config;
    bool m_plainFolder = false;
    QString m_configDigest;
    ProjectLocation m_location;
    LocalDataPaths m_paths;
    std::unique_ptr<ProjectLock> m_lock;
};

const char* openErrorName(ProjectSession::OpenError error);

// What a folder without a project file is labeled with. Its project id comes from where the
// folder is, so local recovery records and the catalog stay with the folder.
ProjectConfig plainFolderConfig(const QString& bindingId);

} // namespace liteanno::application
