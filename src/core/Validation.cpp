#include "core/Validation.h"

#include <algorithm>

namespace liteanno {

namespace {

bool samePoint(const QPointF& a, const QPointF& b)
{
    return a.x() == b.x() && a.y() == b.y();
}

qsizetype distinctPointCount(const PointList& points)
{
    PointList distinct;
    for (const QPointF& point : points) {
        if (std::none_of(distinct.cbegin(), distinct.cend(), [&point](const QPointF& p) { return samePoint(p, point); }))
            distinct.append(point);
    }
    return distinct.size();
}

} // namespace

const char* confirmBlockerCodeName(ConfirmBlockerCode code)
{
    switch (code) {
    case ConfirmBlockerCode::UnrecognizedObject:
        return "UNRECOGNIZED_OBJECT";
    case ConfirmBlockerCode::UnknownClass:
        return "UNKNOWN_CLASS";
    case ConfirmBlockerCode::MissingAttribute:
        return "MISSING_ATTRIBUTE";
    case ConfirmBlockerCode::InvalidGeometry:
        return "INVALID_GEOMETRY";
    case ConfirmBlockerCode::ImageSizeMismatch:
        return "SIZE_MISMATCH";
    case ConfirmBlockerCode::ImageLabelNotSelected:
        return "IMAGE_LABEL_NOT_SELECTED";
    case ConfirmBlockerCode::ImageLabelAmbiguous:
        return "IMAGE_LABEL_AMBIGUOUS";
    }
    return "UNKNOWN";
}

bool hasValidGeometry(const Shape& shape)
{
    const PointList& points = shape.points;
    switch (shape.type) {
    case ShapeType::Rectangle: {
        if (points.size() < 2)
            return false;
        const auto [minX, maxX] = std::minmax_element(points.cbegin(), points.cend(),
                                                      [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
        const auto [minY, maxY] = std::minmax_element(points.cbegin(), points.cend(),
                                                      [](const QPointF& a, const QPointF& b) { return a.y() < b.y(); });
        return maxX->x() > minX->x() && maxY->y() > minY->y();
    }
    case ShapeType::Polygon:
        return distinctPointCount(points) >= 3;
    case ShapeType::Circle:
        return points.size() == 2 && !samePoint(points[0], points[1]);
    case ShapeType::LineStrip:
        // Read with fewer points it stays editable but cannot be confirmed (format spec 13.5).
        return points.size() >= 2;
    case ShapeType::Rotation:
    case ShapeType::Point:
    case ShapeType::Line:
    case ShapeType::Quadrilateral:
    case ShapeType::Cuboid:
        return true;
    }
    return true;
}

QList<ConfirmBlocker> confirmBlockers(const AnnotationDocument& document, const ProjectConfig& project,
                                      bool imageSizeMismatch)
{
    QList<ConfirmBlocker> blockers;
    if (imageSizeMismatch)
        blockers.append({ConfirmBlockerCode::ImageSizeMismatch, -1, QString()});

    const QList<Shape>& shapes = document.shapes();
    for (qsizetype i = 0; i < shapes.size(); ++i) {
        const Shape& shape = shapes[i];
        if (shape.kind != ShapeKind::Editable) {
            blockers.append({ConfirmBlockerCode::UnrecognizedObject, i, QString()});
            continue;
        }
        if (!project.classes.isEmpty()
            && std::none_of(project.classes.cbegin(), project.classes.cend(),
                            [&shape](const ClassDef& c) { return c.name == shape.label; })) {
            blockers.append({ConfirmBlockerCode::UnknownClass, i, shape.label});
        }
        for (const AttributeDef& attribute : project.attributes.value(shape.label)) {
            if (attribute.required && shape.attributes.value(attribute.name).isEmpty())
                blockers.append({ConfirmBlockerCode::MissingAttribute, i, attribute.name});
        }
        if (!hasValidGeometry(shape))
            blockers.append({ConfirmBlockerCode::InvalidGeometry, i, shapeTypeName(shape.type)});
    }

    if (project.imageLabels.mode == ImageLabelMode::Single && !project.imageLabels.options.isEmpty()) {
        const auto selected = std::count_if(project.imageLabels.options.cbegin(), project.imageLabels.options.cend(),
                                            [&document](const QString& option) {
                                                return document.imageLabel(option).value_or(false);
                                            });
        if (selected == 0)
            blockers.append({ConfirmBlockerCode::ImageLabelNotSelected, -1, QString()});
        else if (selected > 1)
            blockers.append({ConfirmBlockerCode::ImageLabelAmbiguous, -1, QString()});
    }
    return blockers;
}

} // namespace liteanno
