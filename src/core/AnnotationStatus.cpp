#include "core/AnnotationStatus.h"

namespace liteanno {

AnnotationStatus deriveStatus(const StatusFacts& facts)
{
    if (facts.imageMissing)
        return AnnotationStatus::Missing;
    if (facts.nameCollision)
        return AnnotationStatus::NameCollision;
    if (facts.jsonError)
        return AnnotationStatus::Corrupt;
    if (facts.orientationPending)
        return AnnotationStatus::OrientationPending;
    if (!facts.hasJson)
        return AnnotationStatus::Unprocessed;

    const ReviewState& review = facts.review;
    switch (review.skip) {
    case SkipState::Unknown:
        return AnnotationStatus::StateConflict;
    case SkipState::Skipped:
        // A reason of only white space is no reason, as for AnnotationDocument::skip().
        if (review.checked || review.skipReason.trimmed().isEmpty())
            return AnnotationStatus::StateConflict;
        return AnnotationStatus::Skipped;
    case SkipState::None:
        break;
    }
    if (review.checked)
        return facts.confirmable ? AnnotationStatus::Confirmed : AnnotationStatus::ConfirmedWithProblems;
    return facts.hasContent ? AnnotationStatus::PendingConfirmation : AnnotationStatus::Unprocessed;
}

bool countsAsDone(AnnotationStatus status)
{
    return status == AnnotationStatus::Skipped || status == AnnotationStatus::Confirmed;
}

const char* annotationStatusName(AnnotationStatus status)
{
    switch (status) {
    case AnnotationStatus::Missing:
        return "missing";
    case AnnotationStatus::NameCollision:
        return "name-collision";
    case AnnotationStatus::Corrupt:
        return "corrupt";
    case AnnotationStatus::OrientationPending:
        return "orientation-pending";
    case AnnotationStatus::StateConflict:
        return "state-conflict";
    case AnnotationStatus::Skipped:
        return "skipped";
    case AnnotationStatus::Confirmed:
        return "confirmed";
    case AnnotationStatus::ConfirmedWithProblems:
        return "confirmed-with-problems";
    case AnnotationStatus::PendingConfirmation:
        return "pending-confirmation";
    case AnnotationStatus::Unprocessed:
        return "unprocessed";
    }
    return "unknown";
}

std::optional<AnnotationStatus> annotationStatusFromName(const QString& name)
{
    for (const AnnotationStatus status :
         {AnnotationStatus::Missing, AnnotationStatus::NameCollision, AnnotationStatus::Corrupt,
          AnnotationStatus::OrientationPending, AnnotationStatus::StateConflict, AnnotationStatus::Skipped,
          AnnotationStatus::Confirmed, AnnotationStatus::ConfirmedWithProblems,
          AnnotationStatus::PendingConfirmation, AnnotationStatus::Unprocessed}) {
        if (name == QLatin1String(annotationStatusName(status)))
            return status;
    }
    return std::nullopt;
}

} // namespace liteanno
