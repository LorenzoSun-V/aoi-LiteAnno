#include "core/AnnotationDocument.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace liteanno {

AnnotationDocument::AnnotationDocument(QList<Shape> shapes, QList<ImageLabel> imageLabels, ReviewState review)
    : m_shapes(std::move(shapes))
    , m_imageLabels(std::move(imageLabels))
    , m_review(std::move(review))
{
    for (const Shape& shape : std::as_const(m_shapes))
        m_nextShapeId = std::max(m_nextShapeId, shape.id + 1);
    for (Shape& shape : m_shapes) {
        if (shape.id == 0)
            shape.id = m_nextShapeId++;
    }
}

std::optional<bool> AnnotationDocument::imageLabel(const QString& name) const
{
    for (const ImageLabel& label : m_imageLabels) {
        if (label.name == name)
            return label.value;
    }
    return std::nullopt;
}

bool AnnotationDocument::setShapePoints(qsizetype index, const PointList& points)
{
    if (!isEditable(index))
        return false;
    Shape& shape = m_shapes[index];
    if (!acceptsPointCount(shape.type, points.size()) || !allFinite(points))
        return false;
    const PointList normalized = shape.type == ShapeType::Rectangle ? canonicalRectangle(points) : points;
    if (exactlyEqual(shape.points, normalized))
        return false;
    shape.points = normalized;
    contentEdited();
    return true;
}

bool AnnotationDocument::setShapeRotation(qsizetype index, const RotatedBox& box)
{
    if (!isEditable(index))
        return false;
    Shape& shape = m_shapes[index];
    if (shape.type != ShapeType::Rotation || box.points.size() != 4 || !allFinite(box.points)
        || !std::isfinite(box.direction))
        return false;
    if (exactlyEqual(shape.points, box.points) && shape.direction == box.direction)
        return false;
    shape.points = box.points;
    shape.direction = box.direction;
    contentEdited();
    return true;
}

bool AnnotationDocument::setShapeLabel(qsizetype index, const QString& label)
{
    if (!isEditable(index) || m_shapes[index].label == label)
        return false;
    m_shapes[index].label = label;
    contentEdited();
    return true;
}

bool AnnotationDocument::setShapeGroupId(qsizetype index, std::optional<qint64> groupId)
{
    if (!isEditable(index) || m_shapes[index].groupId == groupId)
        return false;
    m_shapes[index].groupId = groupId;
    contentEdited();
    return true;
}

qsizetype AnnotationDocument::addKeypoint(Shape point)
{
    qsizetype rectangle = -1;
    double smallest = 0;
    if (point.type == ShapeType::Point && point.points.size() == 1 && !point.groupId) {
        const QPointF at = point.points.front();
        for (qsizetype i = 0; i < m_shapes.size(); ++i) {
            const Shape& shape = m_shapes[i];
            if (shape.kind != ShapeKind::Editable || shape.type != ShapeType::Rectangle || shape.points.isEmpty()
                || (!shape.groupId && shape.locked)) {
                continue;
            }
            double left = shape.points[0].x(), right = left, top = shape.points[0].y(), bottom = top;
            for (const QPointF& p : shape.points) {
                left = std::min(left, p.x());
                right = std::max(right, p.x());
                top = std::min(top, p.y());
                bottom = std::max(bottom, p.y());
            }
            const double area = (right - left) * (bottom - top);
            if (at.x() >= left && at.x() <= right && at.y() >= top && at.y() <= bottom
                && (rectangle < 0 || area < smallest)) {
                rectangle = i;
                smallest = area;
            }
        }
    }
    if (rectangle < 0)
        return addShape(std::move(point));

    std::optional<qint64> group = m_shapes[rectangle].groupId;
    if (!group) {
        qint64 next = 0;
        for (const Shape& shape : m_shapes) {
            if (shape.groupId && *shape.groupId < std::numeric_limits<qint64>::max())
                next = std::max(next, *shape.groupId + 1);
        }
        group = next;
    }
    point.groupId = group;
    const std::optional<qint64> before = m_shapes[rectangle].groupId;
    m_shapes[rectangle].groupId = group;
    const qsizetype index = addShape(std::move(point));
    if (index < 0)
        m_shapes[rectangle].groupId = before;
    return index;
}

qsizetype AnnotationDocument::addShape(Shape shape)
{
    if (shape.kind != ShapeKind::Editable || !isEditableShapeType(shape.type)
        || !acceptsPointCount(shape.type, shape.points.size()) || !allFinite(shape.points)) {
        return -1;
    }
    if (!std::isfinite(shape.direction))
        return -1;
    if (shape.type == ShapeType::Rectangle)
        shape.points = canonicalRectangle(shape.points);
    shape.id = m_nextShapeId++;
    m_shapes.append(std::move(shape));
    contentEdited();
    return m_shapes.size() - 1;
}

bool AnnotationDocument::removeShape(qsizetype index)
{
    if (index < 0 || index >= m_shapes.size())
        return false;
    m_shapes.removeAt(index);
    contentEdited();
    return true;
}

bool AnnotationDocument::setImageLabel(const QString& name, bool value)
{
    auto it = std::find_if(m_imageLabels.begin(), m_imageLabels.end(),
                           [&name](const ImageLabel& label) { return label.name == name; });
    if (it == m_imageLabels.end()) {
        m_imageLabels.append(ImageLabel{name, value});
    } else {
        if (it->value == value)
            return false;
        it->value = value;
    }
    contentEdited();
    return true;
}

bool AnnotationDocument::setShapeDescription(qsizetype index, const QString& description)
{
    if (!isEditable(index) || m_shapes[index].description == description)
        return false;
    m_shapes[index].description = description;
    m_modified = true;
    return true;
}

bool AnnotationDocument::setShapeLocked(qsizetype index, bool locked)
{
    if (!isEditable(index) || m_shapes[index].locked == locked)
        return false;
    m_shapes[index].locked = locked;
    m_modified = true;
    return true;
}

bool AnnotationDocument::confirm()
{
    if (m_review.checked && m_review.skip == SkipState::None)
        return false;
    m_review.checked = true;
    m_review.skip = SkipState::None;
    m_review.skipReason.clear();
    m_reviewStateWritten = true;
    m_modified = true;
    return true;
}

bool AnnotationDocument::skip(const QString& reason)
{
    if (reason.trimmed().isEmpty())
        return false;
    if (!m_review.checked && m_review.skip == SkipState::Skipped && m_review.skipReason == reason)
        return false;
    m_review.checked = false;
    m_review.skip = SkipState::Skipped;
    m_review.skipReason = reason;
    m_reviewStateWritten = true;
    m_modified = true;
    return true;
}

bool AnnotationDocument::unskip()
{
    if (m_review.skip == SkipState::None)
        return false;
    m_review.skip = SkipState::None;
    m_review.skipReason.clear();
    m_reviewStateWritten = true;
    m_modified = true;
    return true;
}

void AnnotationDocument::restoreContent(const AnnotationDocument& state, bool matchesSaved)
{
    const bool reviewChanged = m_review.checked != state.m_review.checked || m_review.skip != state.m_review.skip
                               || m_review.skipReason != state.m_review.skipReason;
    m_shapes = state.m_shapes;
    m_imageLabels = state.m_imageLabels;
    m_review = state.m_review;
    // Ids given out since `state` was copied may still name shapes in the saved file.
    m_nextShapeId = std::max(m_nextShapeId, state.m_nextShapeId);
    if (matchesSaved) {
        m_modified = false;
        m_reviewStateWritten = false;
    } else {
        m_modified = true;
        m_reviewStateWritten = m_reviewStateWritten || state.m_reviewStateWritten || reviewChanged;
    }
}

void AnnotationDocument::markDiffersFromSaved(const ReviewState& saved)
{
    m_modified = true;
    if (m_review.checked != saved.checked || m_review.skip != saved.skip || m_review.skipReason != saved.skipReason)
        m_reviewStateWritten = true;
}

void AnnotationDocument::markSaved()
{
    m_modified = false;
    m_reviewStateWritten = false;
}

bool AnnotationDocument::isEditable(qsizetype index) const
{
    return index >= 0 && index < m_shapes.size() && m_shapes[index].kind == ShapeKind::Editable;
}

void AnnotationDocument::contentEdited()
{
    // Format spec 5.2: a content edit clears the confirmation and the skip state.
    // An unknown status is not a skip state and stays as it is.
    m_review.checked = false;
    if (m_review.skip == SkipState::Skipped) {
        m_review.skip = SkipState::None;
        m_review.skipReason.clear();
    }
    m_reviewStateWritten = true;
    m_modified = true;
}

} // namespace liteanno
