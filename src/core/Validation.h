#pragma once

#include "core/AnnotationDocument.h"
#include "core/ProjectConfig.h"

#include <QList>
#include <QString>

namespace liteanno {

// Why an image cannot be confirmed (format spec 5.3).
enum class ConfirmBlockerCode {
    UnrecognizedObject,     // an unrecognized or preserve-only object is present
    UnknownClass,           // a label is not in the project's class list
    MissingAttribute,       // a required attribute is empty
    InvalidGeometry,        // zero-area rectangle, polygon under 3 distinct points, zero-radius circle,
                            // linestrip under 2 points
    ImageSizeMismatch,      // imageWidth/imageHeight differ from the image
    ImageLabelNotSelected,  // single choice with no configured option selected
    ImageLabelAmbiguous,    // single choice with more than one configured option selected
};

struct ConfirmBlocker {
    ConfirmBlockerCode code;
    qsizetype shapeIndex = -1;  // the shape concerned, or -1 for the whole image
    QString detail;             // a label, attribute name, ...
};

const char* confirmBlockerCodeName(ConfirmBlockerCode code);

// Every reason the document cannot be confirmed; empty when it can. Coordinates outside
// the image only warn and are not reported here (format spec 5.3).
QList<ConfirmBlocker> confirmBlockers(const AnnotationDocument& document, const ProjectConfig& project,
                                      bool imageSizeMismatch);

// Whether the geometry of an editable shape is valid for confirmation (format spec 5.3).
bool hasValidGeometry(const Shape& shape);

} // namespace liteanno
