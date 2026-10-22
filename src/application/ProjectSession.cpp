#include "application/ProjectSession.h"

#include "core/ImagePath.h"
#include "formats/AnnotationStore.h"
#include "formats/ProjectFile.h"
#include "platform/VolumeBinding.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace liteanno::application {

ProjectSession::ProjectSession(ProjectConfig config, QString configDigest, ProjectLocation location,
                               LocalDataPaths paths, std::unique_ptr<ProjectLock> lock)
    : m_config(std::move(config))
    , m_configDigest(std::move(configDigest))
    , m_location(std::move(location))
    , m_paths(std::move(paths))
    , m_lock(std::move(lock))
{
}

ProjectSession::~ProjectSession() = default;

ProjectSession::OpenResult ProjectSession::open(const QString& root, const LocalDataPaths& paths)
{
    OpenResult result;
    const QString rootPath = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    const QString projectFile = rootPath + QLatin1String("/liteanno/project.json");
    if (!QFile::exists(projectFile)) {
        result.error = OpenError::NoProjectFile;
        result.message = projectFile;
        return result;
    }
    formats::ProjectFileResult parsed = formats::readProjectFile(projectFile);
    if (!parsed.config) {
        result.error = OpenError::InvalidProjectFile;
        result.field = parsed.field;
        result.message = parsed.error;
        return result;
    }

    QString error;
    const QString bindingId = platform::bindingIdForDirectory(rootPath, &error);
    if (bindingId.isEmpty()) {
        result.error = OpenError::LocationUnknown;
        result.message = error;
        return result;
    }

    auto lock = std::make_unique<ProjectLock>(paths.lockFile(bindingId));
    switch (lock->tryLock()) {
    case ProjectLock::Status::Locked:
        break;
    case ProjectLock::Status::HeldElsewhere:
        result.error = OpenError::Locked;
        result.holder = lock->holder();
        return result;
    case ProjectLock::Status::Error:
        result.error = OpenError::LockFailed;
        result.message = lock->path();
        return result;
    }

    ProjectLocation location{parsed.config->projectId, bindingId, rootPath};
    result.session.reset(new ProjectSession(std::move(*parsed.config), std::move(parsed.digest), std::move(location), paths, std::move(lock)));
    return result;
}

QString ProjectSession::imageFilePath(const QString& imagePath) const
{
    return m_location.rootPath + QLatin1Char('/') + imagePath;
}

QString ProjectSession::annotationFilePath(const QString& imagePath) const
{
    return formats::existingAnnotationPath(m_location.rootPath + QLatin1Char('/') + annotationPathForImage(imagePath));
}

const char* openErrorName(ProjectSession::OpenError error)
{
    switch (error) {
    case ProjectSession::OpenError::None:
        return "none";
    case ProjectSession::OpenError::NoProjectFile:
        return "no-project-file";
    case ProjectSession::OpenError::InvalidProjectFile:
        return "invalid-project-file";
    case ProjectSession::OpenError::LocationUnknown:
        return "location-unknown";
    case ProjectSession::OpenError::Locked:
        return "locked";
    case ProjectSession::OpenError::LockFailed:
        return "lock-failed";
    }
    return "unknown";
}

} // namespace liteanno::application
