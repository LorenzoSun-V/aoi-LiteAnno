#include "core/ShapeType.h"

namespace liteanno {

namespace {

struct ShapeTypeEntry {
    ShapeType type;
    const char16_t* name;
};

constexpr ShapeTypeEntry kShapeTypes[] = {
    {ShapeType::Rectangle, u"rectangle"},
    {ShapeType::Polygon, u"polygon"},
    {ShapeType::Rotation, u"rotation"},
    {ShapeType::Point, u"point"},
    {ShapeType::Line, u"line"},
    {ShapeType::LineStrip, u"linestrip"},
    {ShapeType::Circle, u"circle"},
    {ShapeType::Quadrilateral, u"quadrilateral"},
    {ShapeType::Cuboid, u"cuboid"},
};

} // namespace

std::optional<ShapeType> shapeTypeFromName(QStringView name)
{
    for (const ShapeTypeEntry& entry : kShapeTypes) {
        if (name == QStringView(entry.name))
            return entry.type;
    }
    return std::nullopt;
}

QString shapeTypeName(ShapeType type)
{
    for (const ShapeTypeEntry& entry : kShapeTypes) {
        if (entry.type == type)
            return QString::fromUtf16(entry.name);
    }
    Q_UNREACHABLE();
    return QString();
}

bool isEditableShapeType(ShapeType type)
{
    return type != ShapeType::Quadrilateral && type != ShapeType::Cuboid;
}

bool acceptsPointCount(ShapeType type, qsizetype count)
{
    switch (type) {
    case ShapeType::Rectangle:
        return count == 2 || count == 4;
    case ShapeType::Rotation:
        return count == 4;
    case ShapeType::Point:
        return count == 1;
    case ShapeType::Line:
    case ShapeType::Circle:
        return count == 2;
    case ShapeType::Polygon:
    case ShapeType::LineStrip:
    case ShapeType::Quadrilateral:
    case ShapeType::Cuboid:
        // Too few points makes the geometry invalid (blocks confirm), not unrecognized.
        return count >= 1;
    }
    return false;
}

} // namespace liteanno
