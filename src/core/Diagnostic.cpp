#include "core/Diagnostic.h"

namespace liteanno {

const char* diagnosticCodeName(DiagnosticCode code)
{
    switch (code) {
    case DiagnosticCode::FileUnreadable: return "FileUnreadable";
    case DiagnosticCode::JsonError: return "JsonError";
    case DiagnosticCode::TopLevelNotObject: return "TopLevelNotObject";
    case DiagnosticCode::ShapesNotArray: return "ShapesNotArray";
    case DiagnosticCode::UnsupportedFormatVersion: return "UnsupportedFormatVersion";
    case DiagnosticCode::ExifOrientationUnconfirmed: return "ExifOrientationUnconfirmed";
    case DiagnosticCode::ImageSizeMismatch: return "ImageSizeMismatch";
    case DiagnosticCode::MissingVersion: return "MissingVersion";
    case DiagnosticCode::MissingShapes: return "MissingShapes";
    case DiagnosticCode::MissingImagePath: return "MissingImagePath";
    case DiagnosticCode::ImagePathMismatch: return "ImagePathMismatch";
    case DiagnosticCode::MissingImageData: return "MissingImageData";
    case DiagnosticCode::EmbeddedImageData: return "EmbeddedImageData";
    case DiagnosticCode::InvalidChecked: return "InvalidChecked";
    case DiagnosticCode::InvalidFlags: return "InvalidFlags";
    case DiagnosticCode::InvalidImageLabelValue: return "InvalidImageLabelValue";
    case DiagnosticCode::MissingImageSize: return "MissingImageSize";
    case DiagnosticCode::InvalidImageSize: return "InvalidImageSize";
    case DiagnosticCode::InvalidLiteanno: return "InvalidLiteanno";
    case DiagnosticCode::MissingFormatVersion: return "MissingFormatVersion";
    case DiagnosticCode::UnknownStatus: return "UnknownStatus";
    case DiagnosticCode::InvalidSkipReason: return "InvalidSkipReason";
    case DiagnosticCode::ShapeTypeDefaulted: return "ShapeTypeDefaulted";
    case DiagnosticCode::ShapeUnrecognized: return "ShapeUnrecognized";
    case DiagnosticCode::ShapePreserveOnly: return "ShapePreserveOnly";
    case DiagnosticCode::InvalidShapeField: return "InvalidShapeField";
    }
    return "Unknown";
}

} // namespace liteanno
