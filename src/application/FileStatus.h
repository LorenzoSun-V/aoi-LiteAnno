#pragma once

#include "core/AnnotationStatus.h"
#include "core/ProjectConfig.h"
#include "formats/AnnotationFile.h"

namespace liteanno::application {

// The status of one image from its annotation file as read or as edited (format spec 5.1).
// `hasJson`: the file exists, or edits wait to create it.
AnnotationStatus statusOfFile(const formats::AnnotationFile& file, const ProjectConfig& project,
                              bool nameCollision, bool hasJson);

} // namespace liteanno::application
