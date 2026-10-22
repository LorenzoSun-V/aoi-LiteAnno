#pragma once

// The words the window shows for program states, in one place.

#include "core/AnnotationStatus.h"
#include "core/ShapeType.h"
#include "core/Validation.h"
#include "formats/AnnotationFile.h"
#include "formats/Exporters.h"

#include <QString>

namespace liteanno::ui {

QString statusText(AnnotationStatus status);
QString shapeTypeText(ShapeType type);
QString blockerText(const ConfirmBlocker& blocker);
QString readOnlyText(formats::ReadOnlyReason reason);
QString exportFormatText(formats::ExportFormat format);

} // namespace liteanno::ui
