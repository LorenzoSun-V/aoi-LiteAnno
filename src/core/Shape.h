#pragma once

#include "core/Geometry.h"
#include "core/ShapeType.h"

#include <QHash>
#include <QString>

#include <optional>

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
    // Rotated boxes only: upstream's angle in radians, "direction" in the file. The corners
    // are what counts (format spec 4.2); the angle is kept in step for upstream.
    double direction = 0;
    QString description;
    // "group_id" when the file holds an integer: joins the rectangle and keypoints of one
    // pose, or the polygons of one instance. Another value reads as none and stays in the
    // file as it is until the group is edited.
    std::optional<qint64> groupId;
    // String values of the "attributes" object, for the confirm conditions (format spec 5.3).
    // Not written back: the object on disk stays as it is.
    QHash<QString, QString> attributes;
    bool locked = false;
    // A pasted shape: the JSON object it was copied from. Fields this program does not edit
    // (score, group_id, difficult, flags, attributes, ...) are written as they were, as
    // upstream's paste keeps them.
    QByteArray copiedObject;
};

} // namespace liteanno
