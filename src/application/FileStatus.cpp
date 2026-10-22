#include "application/FileStatus.h"

#include "core/Validation.h"

namespace liteanno::application {

AnnotationStatus statusOfFile(const formats::AnnotationFile& file, const ProjectConfig& project,
                              bool nameCollision, bool hasJson)
{
    using formats::ReadOnlyReason;
    const AnnotationDocument& document = file.document();
    StatusFacts facts;
    facts.nameCollision = nameCollision;
    facts.jsonError = file.readOnlyReason() == ReadOnlyReason::JsonError
                      || file.readOnlyReason() == ReadOnlyReason::Unreadable;
    facts.orientationPending = file.readOnlyReason() == ReadOnlyReason::ExifOrientationUnconfirmed;
    facts.hasJson = hasJson;
    facts.review = document.review();
    facts.hasContent = !document.shapes().isEmpty();
    for (const ImageLabel& label : document.imageLabels())
        facts.hasContent = facts.hasContent || label.value.value_or(false);
    const bool sizeMismatch = file.readOnlyReason() == ReadOnlyReason::ImageSizeMismatch;
    facts.confirmable = confirmBlockers(document, project, sizeMismatch).isEmpty();
    return deriveStatus(facts);
}

} // namespace liteanno::application
