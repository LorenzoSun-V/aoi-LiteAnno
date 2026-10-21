#pragma once

#include "core/Geometry.h"
#include "core/ShapeType.h"

#include <QString>

namespace liteanno {

enum class ShapeKind {
    Editable,      // one of the seven editable types, well formed
    PreserveOnly,  // an upstream type kept but not edited: quadrilateral, cuboid
    Unrecognized,  // kept unchanged; blocks confirm and export (format spec 4.2, 13.5)
};

// One object in the "shapes" array. Only the fields this program edits are modelled;
// everything else stays in the file untouched.
struct Shape {
    // Identifies the shape within its document. A reader uses it to find the shape's
    // original object on disk; new shapes get an id when added to a document.
    quint64 id = 0;
    ShapeKind kind = ShapeKind::Editable;
    ShapeType type = ShapeType::Polygon;  // not meaningful for unrecognized objects
    QString label;
    PointList points;
    QString description;
    bool locked = false;
};

} // namespace liteanno
