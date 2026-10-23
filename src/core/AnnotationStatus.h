#pragma once

#include "core/ReviewState.h"

#include <QString>

#include <optional>

namespace liteanno {

// Derived status of one image (format spec 5.1), in the order it is decided.
enum class AnnotationStatus {
    Missing,               // listed in the batch, image not found
    NameCollision,         // STEM_COLLISION or NAME_COLLISION
    Corrupt,               // JSON_ERROR
    OrientationPending,    // old annotation of an EXIF-rotated image (13.4)
    StateConflict,         // checked and skipped, skipped without reason, or unknown status
    Skipped,
    Confirmed,
    ConfirmedWithProblems,  // checked, but the confirm conditions do not hold
    PendingConfirmation,   // has JSON content, not checked
    Unprocessed,           // no JSON, or an empty unchecked one
};

// What the status is derived from. The caller fills it from the image, its annotation file
// and confirmBlockers().
struct StatusFacts {
    bool imageMissing = false;
    bool nameCollision = false;
    bool jsonError = false;
    bool orientationPending = false;
    bool hasJson = false;
    ReviewState review;
    bool hasContent = false;     // at least one shape or image-level label
    bool confirmable = false;    // confirmBlockers() returned nothing
};

AnnotationStatus deriveStatus(const StatusFacts& facts);

// Whether the status counts as done for the batch (format spec 5.1, 5.4).
bool countsAsDone(AnnotationStatus status);

const char* annotationStatusName(AnnotationStatus status);
// As the window and the reports show it to the user.
QString annotationStatusText(AnnotationStatus status);
// The inverse of annotationStatusName(); nothing for a name it never returns.
std::optional<AnnotationStatus> annotationStatusFromName(const QString& name);

} // namespace liteanno
