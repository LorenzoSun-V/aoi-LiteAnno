#include "application/ProjectSession.h"

#include "core/FormatVersion.h"
#include "core/ImagePath.h"
#include "formats/AnnotationStore.h"
#include "formats/ProjectFile.h"
#include "platform/VolumeBinding.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <optional>

namespace liteanno::application {

ProjectSession::ProjectSession(ProjectConfig config, QString configDigest, ProjectLocation location,
                               LocalDataPaths paths, std::unique_ptr<ProjectLock> lock, bool plainFolder)
    : m_config(std::move(config))
    , m_plainFolder(plainFolder)
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
    const bool plainFolder = !QFileInfo(projectFile).exists();
    std::optional<ProjectConfig> config;
    QString digest = QStringLiteral("plain-folder");
    if (!plainFolder) {
        formats::ProjectFileResult parsed = formats::readProjectFile(projectFile);
        if (!parsed.config) {
            result.error = OpenError::InvalidProjectFile;
            result.field = parsed.field;
            result.message = parsed.error;
            return result;
        }
        config = std::move(parsed.config);
        digest = std::move(parsed.digest);
    } else if (!QFileInfo(rootPath).isDir()) {
        result.error = OpenError::LocationUnknown;
        result.message = QStringLiteral("not a folder: ") + rootPath;
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

    if (plainFolder)
        config = plainFolderConfig(bindingId);
    ProjectLocation location{config->projectId, bindingId, rootPath};
    result.session.reset(new ProjectSession(std::move(*config), std::move(digest), std::move(location), paths,
                                            std::move(lock), plainFolder));
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

ProjectConfig plainFolderConfig(const QString& bindingId)
{
    ProjectConfig config;
    config.formatVersion = QString::fromLatin1(kFormatVersion);
    config.projectId = QStringLiteral("folder-") + bindingId;
    config.shapeTypes = {ShapeType::Rectangle, ShapeType::Polygon, ShapeType::Rotation, ShapeType::Point,
                         ShapeType::Line,      ShapeType::LineStrip, ShapeType::Circle};
    return config;
}

} // namespace liteanno::application
