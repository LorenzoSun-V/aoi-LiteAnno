#pragma once

#include "core/AnnotationDocument.h"

#include <QImage>
#include <QPointF>
#include <QTransform>
#include <QWidget>

#include <optional>

namespace liteanno::ui {

// Shows one image with its shapes and turns mouse input into edits. The canvas never edits
// the document itself: it emits what the user drew or moved, and the window applies it
// through the DocumentSession, which records the undo step.
class Canvas : public QWidget
{
    Q_OBJECT

public:
    enum class Tool {
        Select,
        Rectangle,
        Polygon,
    };

    explicit Canvas(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    // The shapes to draw; call again after every change of the document.
    void setDocument(const AnnotationDocument* document, bool readOnly);
    const AnnotationDocument* document() const { return m_document; }
    void setClassColors(const QHash<QString, QColor>& colors);

    Tool tool() const { return m_tool; }
    void setTool(Tool tool);

    qsizetype selectedShape() const { return m_selected; }
    void setSelectedShape(qsizetype index);

    // Image and widget coordinates.
    QPointF imageToWidget(const QPointF& point) const;
    QPointF widgetToImage(const QPointF& point) const;
    void fitToWindow();
    double zoom() const { return m_scale; }

signals:
    void shapeDrawn(const liteanno::Shape& shape);
    void shapePointsChanged(qsizetype index, const liteanno::PointList& points);
    void selectionChanged(qsizetype index);
    // The user asked to change the label of the selected shape (double click).
    void labelEditRequested(qsizetype index);
    void deleteRequested(qsizetype index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class Drag {
        None,
        Pan,
        DrawRectangle,
        MoveShape,
        MoveVertex,
    };

    QTransform transform() const;
    QPointF clampToImage(const QPointF& point) const;
    qsizetype shapeAt(const QPointF& imagePoint) const;
    int vertexAt(qsizetype shape, const QPointF& widgetPoint) const;
    bool canEdit(qsizetype index) const;
    PointList displayedPoints(qsizetype index) const;
    void finishPolygon();
    void cancelDrawing();
    QColor colorFor(const QString& label) const;
    void paintShape(QPainter& painter, const Shape& shape, const PointList& points, bool selected) const;

    QImage m_image;
    const AnnotationDocument* m_document = nullptr;
    bool m_readOnly = false;
    QHash<QString, QColor> m_classColors;
    Tool m_tool = Tool::Select;
    qsizetype m_selected = -1;

    double m_scale = 1.0;
    QPointF m_offset;  // widget position of the image origin
    bool m_fitPending = true;

    Drag m_drag = Drag::None;
    QPointF m_pressWidget;
    QPointF m_pressImage;
    QPointF m_currentImage;
    int m_dragVertex = -1;
    PointList m_dragPoints;      // the selected shape's points during a move
    PointList m_polygonPoints;   // a polygon being drawn
};

} // namespace liteanno::ui
