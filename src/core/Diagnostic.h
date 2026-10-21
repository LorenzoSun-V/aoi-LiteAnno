#pragma once

#include <QList>
#include <QString>

namespace liteanno {

// Problems found while reading an annotation file. The first group makes the file
// read-only; the rest are warnings shown to the user and listed in scan reports.
enum class DiagnosticCode {
    FileUnreadable,              // the file exists but cannot be read
    JsonError,                   // not strict JSON (format spec 4.6, 13.5)
    TopLevelNotObject,           // 4.6
    ShapesNotArray,              // 13.3
    UnsupportedFormatVersion,    // newer major or unreadable liteanno.formatVersion (section 1, 13.5)
    ExifOrientationUnconfirmed,  // 13.4
    ImageSizeMismatch,           // SIZE_MISMATCH (5.3)

    MissingVersion,
    MissingShapes,
    MissingImagePath,
    ImagePathMismatch,
    MissingImageData,
    EmbeddedImageData,
    InvalidChecked,
    InvalidFlags,
    InvalidImageLabelValue,
    MissingImageSize,
    InvalidImageSize,
    InvalidLiteanno,
    MissingFormatVersion,
    UnknownStatus,
    InvalidSkipReason,
    ShapeTypeDefaulted,          // missing or null shape_type read as polygon (13.5)
    ShapeUnrecognized,
    ShapePreserveOnly,
    InvalidShapeField,
};

struct Diagnostic {
    DiagnosticCode code;
    QString location;  // JSON pointer, e.g. "/shapes/3/points"; empty for the whole file
    QString detail;
};

const char* diagnosticCodeName(DiagnosticCode code);

} // namespace liteanno
