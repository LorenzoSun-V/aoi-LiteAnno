#pragma once

#include <QString>
#include <QStringView>

#include <optional>

namespace liteanno {

// The shape types X-AnyLabeling knows (upstream shape.py, get_supported_shape).
enum class ShapeType {
    Rectangle,
    Polygon,
    Rotation,
    Point,
    Line,
    LineStrip,
    Circle,
    Quadrilateral,
    Cuboid,
};

// Maps the upstream "shape_type" string; nullopt for names upstream does not know.
std::optional<ShapeType> shapeTypeFromName(QStringView name);
QString shapeTypeName(ShapeType type);

// The seven types this program edits (format spec 4.2). Quadrilateral and cuboid are kept
// unchanged but not edited.
bool isEditableShapeType(ShapeType type);

// Whether a shape of this type read from disk with `count` points can be used
// (format spec 13.5). Shapes that fail this check are kept unchanged as unrecognized objects.
bool acceptsPointCount(ShapeType type, qsizetype count);

} // namespace liteanno
