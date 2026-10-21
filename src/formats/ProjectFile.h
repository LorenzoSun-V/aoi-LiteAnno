#pragma once

#include "core/ProjectConfig.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace liteanno::formats {

struct ProjectFileResult {
    std::optional<ProjectConfig> config;  // nullopt when the file cannot be used
    QString field;  // JSON pointer of the offending field, "" for the whole file
    QString error;  // why the file was rejected, in English
};

// Reads liteanno/project.json (format spec section 6). Any invalid field rejects the whole
// file; the result names the field so the project can refuse to open with a precise message.
ProjectFileResult parseProjectFile(const QByteArray& bytes);

ProjectFileResult readProjectFile(const QString& path);

} // namespace liteanno::formats
