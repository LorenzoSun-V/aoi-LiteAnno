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
        Rotation,
        Point,      // one click
        Line,       // dragged from one end to the other
        LineStrip,  // clicked point by point, finished like a polygon
        Circle,     // dragged from the centre out
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
    // Selecting a hidden shape shows it again.
    void setSelectedShape(qsizetype index);

    // What is shown; nothing of it is saved. Hidden shapes are not drawn and cannot be
    // clicked. A new image starts with every shape shown, unless all shapes are switched off.
    // "S": hides the selected shape.
    bool hideSelected();
    // "W": shows the shape hidden last.
    bool showLastHidden();
    // Ctrl+H: shows or hides all shapes; stays so for the next images.
    void setShapesVisible(bool visible);
    bool shapesVisible() const { return m_shapesVisible; }
    bool isHidden(qsizetype index) const;
    // Ctrl+L: the class names next to the shapes; big names can cover small boxes.
    void setLabelsVisible(bool visible);
    bool labelsVisible() const { return m_labelsVisible; }
    // How thick the outlines are drawn, in screen pixels at any zoom; the selected shape's
    // is one pixel thicker. 4 by default, as X-AnyLabeling 4.2.0's shape.line_width.
    static constexpr double kDefaultLineWidth = 4.0;
    static constexpr double kMinLineWidth = 0.5;
    static constexpr double kMaxLineWidth = 20.0;
    // Kept between kMinLineWidth and kMaxLineWidth.
    void setLineWidth(double width);
    double lineWidth() const { return m_lineWidth; }

    // Image and widget coordinates.
    QPointF imageToWidget(const QPointF& point) const;
    QPointF widgetToImage(const QPointF& point) const;
    void fitToWindow();
    // Zooms by `factor` around the middle of the widget.
    void zoomBy(double factor);
    // One image pixel per screen pixel, keeping the middle where it is.
    void zoomToOriginalSize();
    double zoom() const { return m_scale; }
    QSize imageSize() const { return m_image.size(); }
    // The colour shapes of the class are drawn in.
    QColor colorFor(const QString& label) const;
    // Takes back the last point of a polygon being drawn (Ctrl+Z while drawing, as in
    // X-AnyLabeling). False when no polygon is being drawn.
    bool removeLastPolygonPoint();

signals:
    void shapeDrawn(const liteanno::Shape& shape);
    void shapePointsChanged(qsizetype index, const liteanno::PointList& points);
    // Z/X/C/V turned the selected rotated box.
    void shapeRotated(qsizetype index, const liteanno::RotatedBox& box);
    // A shape was hidden or shown.
    void visibilityChanged();
    void selectionChanged(qsizetype index);
    // The user asked to change the label of the selected shape (double click).
    void labelEditRequested(qsizetype index);
    void deleteRequested(qsizetype index);
    // A right click (not a pan): the shape under it, if any, is selected first.
    void contextMenuRequested(const QPoint& globalPosition);
    // The image is shown at another scale (1 = one image pixel per screen pixel).
    void zoomChanged(double scale);
    // The mouse is over the image at `imagePoint`, or (`inside` false) has left it.
    void cursorMoved(const QPointF& imagePoint, bool inside);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class Drag {
        None,
        Pan,
        DrawRectangle,  // also rotated boxes, lines and circles: whatever is dragged
        MoveShape,
        MoveVertex,
    };

    QTransform transform() const;
    QPointF clampToImage(const QPointF& point) const;
    qsizetype shapeAt(const QPointF& imagePoint) const;
    int vertexAt(qsizetype shape, const QPointF& widgetPoint) const;
    bool canEdit(qsizetype index) const;
    PointList displayedPoints(qsizetype index) const;
    // Finishes the polygon or line strip being clicked.
    void finishPolygon();
    // The tool clicks its points one by one.
    bool clicksPoints() const { return m_tool == Tool::Polygon || m_tool == Tool::LineStrip; }
    void zoomAround(const QPointF& anchor, double factor);
    // Arrow keys: the selected shape moves one image pixel, unless that leaves the image.
    void moveSelectedBy(const QPointF& delta);
    // Z/X/C/V: turns the selected rotated box by `degrees`, as upstream's keys do.
    bool rotateSelectedBy(double degrees);
    void cancelDrawing();
    // Rectangle, rotated box, line or circle from `start` to `end`; false if too small to keep.
    bool finishBox(const QPointF& start, const QPointF& end);
    void updateCursor();
    // Sends cursorMoved for the mouse's last position, after the view moved or zoomed under it.
    void reportCursor();
    void paintShape(QPainter& painter, const Shape& shape, const PointList& points, bool selected) const;

    QImage m_image;
    QPointF m_mouseWidget;  // the mouse's last position over the canvas
    bool m_mouseOver = false;
    const AnnotationDocument* m_document = nullptr;
    bool m_readOnly = false;
    QHash<QString, QColor> m_classColors;
    Tool m_tool = Tool::Select;
    qsizetype m_selected = -1;

    double m_scale = 1.0;
    QPointF m_offset;  // widget position of the image origin
    bool m_fitPending = true;
    bool m_spaceHeld = false;  // Space + left button pans, as in X-AnyLabeling

    Drag m_drag = Drag::None;
    QPointF m_pressWidget;
    QPointF m_panStart;  // where the pan's button went down
    Qt::MouseButton m_panButton = Qt::NoButton;
    QPointF m_pressImage;
    QPointF m_currentImage;
    int m_dragVertex = -1;
    PointList m_dragPoints;      // the selected shape's points during a move
    PointList m_polygonPoints;   // a polygon being drawn
    std::optional<QPointF> m_clickStart;  // first corner of a box begun by a click

    QList<quint64> m_hidden;  // ids of hidden shapes, in the order they were hidden
    bool m_shapesVisible = true;
    bool m_hideAllPending = false;  // all shapes are off: hide those of the next document
    bool m_labelsVisible = true;
    double m_lineWidth = kDefaultLineWidth;
};

} // namespace liteanno::ui
