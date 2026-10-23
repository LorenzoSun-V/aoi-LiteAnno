#pragma once

#include "core/ReviewState.h"
#include "core/Shape.h"

#include <QList>
#include <QString>

#include <optional>

namespace liteanno {

// One entry of the top-level "flags" object: an image-level label (format spec 4.1, 13.1).
struct ImageLabel {
    QString name;
    std::optional<bool> value;  // nullopt when the file holds a non-boolean value
};

// The editable content of one per-image annotation file, independent of its JSON text.
//
// Edit operations apply the state transitions of format spec 5.2 and return false when
// they change nothing. Labels are never removed: project settings only decide which ones
// the interface offers (13.1).
class AnnotationDocument
{
public:
    AnnotationDocument() = default;
    // A document as read from disk, not modified. Shapes without an id get one.
    AnnotationDocument(QList<Shape> shapes, QList<ImageLabel> imageLabels, ReviewState review);

    const QList<Shape>& shapes() const { return m_shapes; }
    const QList<ImageLabel>& imageLabels() const { return m_imageLabels; }
    const ReviewState& review() const { return m_review; }
    std::optional<bool> imageLabel(const QString& name) const;

    // Content edits (format spec 4.2). They clear the confirmation and the skip state.
    // Only editable shapes can be changed; any shape can be removed.
    bool setShapePoints(qsizetype index, const PointList& points);
    // A rotated box turned: new corners and angle together.
    bool setShapeRotation(qsizetype index, const RotatedBox& box);
    bool setShapeLabel(qsizetype index, const QString& label);
    // "group_id": which pose (rectangle and keypoints) or instance the shape belongs to.
    bool setShapeGroupId(qsizetype index, std::optional<qint64> groupId);
    // Returns the index of the new shape, or -1 when it is not an editable shape this
    // build can create.
    qsizetype addShape(Shape shape);
    bool removeShape(qsizetype index);
    bool setImageLabel(const QString& name, bool value);

    // Edits of non-content fields. They keep the review state.
    bool setShapeDescription(qsizetype index, const QString& description);
    bool setShapeLocked(qsizetype index, bool locked);

    // Review operations. The caller checks the confirm conditions (format spec 5.3) first.
    bool confirm();
    bool skip(const QString& reason);  // a reason is required
    bool unskip();

    // Undo and redo: takes the shapes, image-level labels and review state of `state`, an
    // earlier copy of this document (format spec 5.2). `matchesSaved` says that this content
    // is exactly what was last read or saved, so nothing needs writing.
    void restoreContent(const AnnotationDocument& state, bool matchesSaved);

    // Whether anything changed since the document was read or last saved.
    bool isModified() const { return m_modified; }
    // Whether an operation set "checked" or the skip state since the document was read or
    // last saved. Writers update those fields only in that case (13.3).
    bool reviewStateWritten() const { return m_reviewStateWritten; }
    void markSaved();
    // The file now holds other content than this document, whose review state was `saved`;
    // the next save writes this content, review state included where it differs.
    void markDiffersFromSaved(const ReviewState& saved);

private:
    bool isEditable(qsizetype index) const;
    void contentEdited();

    QList<Shape> m_shapes;
    QList<ImageLabel> m_imageLabels;
    ReviewState m_review;
    quint64 m_nextShapeId = 1;
    bool m_modified = false;
    bool m_reviewStateWritten = false;
};

} // namespace liteanno
