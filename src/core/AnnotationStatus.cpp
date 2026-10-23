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

QString annotationStatusText(AnnotationStatus status)
{
    switch (status) {
    case AnnotationStatus::Missing:
        return QStringLiteral(u"缺失");
    case AnnotationStatus::NameCollision:
        return QStringLiteral(u"名称冲突");
    case AnnotationStatus::Corrupt:
        return QStringLiteral(u"标注文件损坏");
    case AnnotationStatus::OrientationPending:
        return QStringLiteral(u"坐标方向待确认");
    case AnnotationStatus::StateConflict:
        return QStringLiteral(u"状态冲突");
    case AnnotationStatus::Skipped:
        return QStringLiteral(u"已跳过");
    case AnnotationStatus::Confirmed:
        return QStringLiteral(u"已确认");
    case AnnotationStatus::ConfirmedWithProblems:
        return QStringLiteral(u"已确认但有问题");
    case AnnotationStatus::PendingConfirmation:
        return QStringLiteral(u"待确认");
    case AnnotationStatus::Unprocessed:
        return QStringLiteral(u"未处理");
    }
    return QString();
}

} // namespace liteanno
