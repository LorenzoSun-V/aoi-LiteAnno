#include "ui/Canvas.h"

#include "ui/Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace liteanno::ui {

namespace {

constexpr double kHandleRadius = 5.0;  // widget pixels
constexpr double kCloseDistance = 8.0;  // a click this close to the first point closes a polygon
constexpr double kClickDistance = 3.0;  // a press released this close (widget pixels) is a click

double distance(const QPointF& a, const QPointF& b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

double distanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const double length2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (length2 == 0)
        return distance(p, a);
    const double t = std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / length2, 0.0, 1.0);
    return distance(p, a + t * ab);
}

} // namespace

Canvas::Canvas(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
    setAutoFillBackground(true);
    QPalette background = palette();
    background.setColor(QPalette::Window, QColor(0x2A, 0x2E, 0x33));
    setPalette(background);
}

void Canvas::setImage(const QImage& image)
{
    m_image = image;
    m_selected = -1;
    m_hidden.clear();
    m_hideAllPending = !m_shapesVisible;
    cancelDrawing();
    m_fitPending = true;
    fitToWindow();
}

void Canvas::setDocument(const AnnotationDocument* document, bool readOnly)
{
    // The window refreshes the same document after saves; a drag in progress survives that.
    const bool otherDocument = document != m_document || readOnly != m_readOnly;
    m_document = document;
    m_readOnly = readOnly;
    if (m_document && m_hideAllPending) {
        m_hideAllPending = false;
        for (const Shape& shape : m_document->shapes())
            m_hidden.append(shape.id);
    }
    if (!m_document || m_selected >= m_document->shapes().size()) {
        m_selected = -1;
        if (m_drag == Drag::MoveShape || m_drag == Drag::MoveVertex)
            m_drag = Drag::None;
    }
    if (otherDocument && (m_drag == Drag::MoveShape || m_drag == Drag::MoveVertex))
        m_drag = Drag::None;
    update();
}

void Canvas::setClassColors(const QHash<QString, QColor>& colors)
{
    m_classColors = colors;
    update();
}

void Canvas::setTool(Tool tool)
{
    cancelDrawing();
    m_tool = tool;
    updateCursor();
    update();
}

void Canvas::setSelectedShape(qsizetype index)
{
    bool shown = false;
    if (isHidden(index)) {
        m_hidden.removeAll(m_document->shapes()[index].id);
        shown = true;
    }
    if (m_selected != index) {
        m_selected = index;
        // A drag belongs to the shape it started on.
        if (m_drag == Drag::MoveShape || m_drag == Drag::MoveVertex) {
            m_drag = Drag::None;
            m_dragPoints.clear();
        }
    }
    // After the selection: the window rebuilds its list from it.
    if (shown)
        emit visibilityChanged();
    update();
}

bool Canvas::isHidden(qsizetype index) const
{
    return m_document && index >= 0 && index < m_document->shapes().size()
           && m_hidden.contains(m_document->shapes()[index].id);
}

bool Canvas::hideSelected()
{
    if (!m_document || m_selected < 0 || m_selected >= m_document->shapes().size())
        return false;
    if (m_drag == Drag::MoveShape || m_drag == Drag::MoveVertex) {
        m_drag = Drag::None;
        m_dragPoints.clear();
    }
    m_hidden.append(m_document->shapes()[m_selected].id);
    m_selected = -1;
    emit selectionChanged(-1);
    emit visibilityChanged();
    update();
    return true;
}

bool Canvas::showLastHidden()
{
    // Shapes deleted meanwhile are skipped.
    while (!m_hidden.isEmpty()) {
        const quint64 id = m_hidden.takeLast();
        if (m_document && std::any_of(m_document->shapes().cbegin(), m_document->shapes().cend(),
                                      [id](const Shape& shape) { return shape.id == id; })) {
            emit visibilityChanged();
            update();
            return true;
        }
    }
    return false;
}

void Canvas::setShapesVisible(bool visible)
{
    m_shapesVisible = visible;
    m_hideAllPending = false;
    m_hidden.clear();
    if (!visible && m_document) {
        for (const Shape& shape : m_document->shapes())
            m_hidden.append(shape.id);
        if (m_selected >= 0) {
            if (m_drag == Drag::MoveShape || m_drag == Drag::MoveVertex) {
                m_drag = Drag::None;
                m_dragPoints.clear();
            }
            m_selected = -1;
            emit selectionChanged(-1);
        }
    }
    emit visibilityChanged();
    update();
}

void Canvas::setLabelsVisible(bool visible)
{
    m_labelsVisible = visible;
    update();
}

QTransform Canvas::transform() const
{
    return QTransform(m_scale, 0, 0, m_scale, m_offset.x(), m_offset.y());
}

QPointF Canvas::imageToWidget(const QPointF& point) const
{
    return transform().map(point);
}

QPointF Canvas::widgetToImage(const QPointF& point) const
{
    return QPointF((point.x() - m_offset.x()) / m_scale, (point.y() - m_offset.y()) / m_scale);
}

void Canvas::fitToWindow()
{
    if (m_image.isNull() || width() <= 0 || height() <= 0) {
        update();
        return;
    }
    const double margin = 10.0;
    m_scale = std::min((width() - 2 * margin) / m_image.width(), (height() - 2 * margin) / m_image.height());
    m_scale = std::max(m_scale, 0.01);
    m_offset = QPointF((width() - m_image.width() * m_scale) / 2, (height() - m_image.height() * m_scale) / 2);
    m_fitPending = false;
    update();
    emit zoomChanged(m_scale);
    reportCursor();
}

void Canvas::zoomAround(const QPointF& anchor, double factor)
{
    if (m_image.isNull())
        return;
    const QPointF imagePoint = widgetToImage(anchor);
    // Fitting a very wide or a tiny image can go beyond the limits; from there a step may
    // stay where it is, but never go the other way.
    m_scale = std::clamp(m_scale * factor, std::min(0.02, m_scale), std::max(64.0, m_scale));
    m_offset = anchor - imagePoint * m_scale;
    m_fitPending = false;
    update();
    emit zoomChanged(m_scale);
    reportCursor();
}

void Canvas::zoomBy(double factor)
{
    zoomAround(QPointF(width() / 2.0, height() / 2.0), factor);
}

void Canvas::zoomToOriginalSize()
{
    if (m_scale > 0)
        zoomBy(1.0 / m_scale);
}

bool Canvas::removeLastPolygonPoint()
{
    if (m_polygonPoints.isEmpty())
        return false;
    m_polygonPoints.removeLast();
    update();
    return true;
}

void Canvas::moveSelectedBy(const QPointF& delta)
{
    if (!canEdit(m_selected) || m_drag != Drag::None)
        return;
    PointList points = m_document->shapes()[m_selected].points;
    for (QPointF& point : points) {
        point += delta;
        if (point.x() < 0 || point.y() < 0 || point.x() > m_image.width() || point.y() > m_image.height())
            return;
    }
    emit shapePointsChanged(m_selected, points);
}

bool Canvas::rotateSelectedBy(double degrees)
{
    if (!canEdit(m_selected) || m_drag != Drag::None)
        return false;
    const Shape& shape = m_document->shapes()[m_selected];
    if (shape.type != ShapeType::Rotation || shape.points.size() != 4)
        return false;
    emit shapeRotated(m_selected, rotateBox({shape.points, fittingDirection({shape.points, shape.direction})}, radians(degrees)));
    return true;
}

void Canvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_fitPending)
        fitToWindow();
}

QPointF Canvas::clampToImage(const QPointF& point) const
{
    if (m_image.isNull())
        return point;
    return QPointF(std::clamp(point.x(), 0.0, double(m_image.width())),
                   std::clamp(point.y(), 0.0, double(m_image.height())));
}

bool Canvas::canEdit(qsizetype index) const
{
    if (m_readOnly || !m_document || index < 0 || index >= m_document->shapes().size())
        return false;
    const Shape& shape = m_document->shapes()[index];
    return shape.kind == ShapeKind::Editable && !shape.locked && !isHidden(index);
}

PointList Canvas::displayedPoints(qsizetype index) const
{
    if (index == m_selected && (m_drag == Drag::MoveShape || m_drag == Drag::MoveVertex))
        return m_dragPoints;
    return m_document->shapes()[index].points;
}

qsizetype Canvas::shapeAt(const QPointF& imagePoint) const
{
    if (!m_document)
        return -1;
    const double tolerance = kHandleRadius / m_scale;
    const QList<Shape>& shapes = m_document->shapes();
    // The shape drawn last is on top.
    for (qsizetype i = shapes.size() - 1; i >= 0; --i) {
        const Shape& shape = shapes[i];
        const PointList& points = shape.points;
        if (points.isEmpty() || m_hidden.contains(shape.id))
            continue;
        switch (shape.type) {
        case ShapeType::Rectangle:
        case ShapeType::Polygon:
        case ShapeType::Rotation:
        case ShapeType::Quadrilateral:
        case ShapeType::Cuboid:
            if (points.size() >= 3 && QPolygonF(points).containsPoint(imagePoint, Qt::OddEvenFill))
                return i;
            break;
        case ShapeType::Circle:
            if (points.size() == 2 && distance(imagePoint, points[0]) <= distance(points[0], points[1]))
                return i;
            break;
        default:
            break;
        }
        for (qsizetype p = 0; p < points.size(); ++p) {
            if (distance(imagePoint, points[p]) <= tolerance)
                return i;
            if (p + 1 < points.size() && distanceToSegment(imagePoint, points[p], points[p + 1]) <= tolerance)
                return i;
        }
    }
    return -1;
}

int Canvas::vertexAt(qsizetype shape, const QPointF& widgetPoint) const
{
    if (!canEdit(shape))
        return -1;
    const PointList& points = m_document->shapes()[shape].points;
    for (int i = 0; i < points.size(); ++i) {
        if (distance(imageToWidget(points[i]), widgetPoint) <= kHandleRadius + 2)
            return i;
    }
    return -1;
}

void Canvas::cancelDrawing()
{
    m_polygonPoints.clear();
    m_clickStart.reset();
    m_drag = Drag::None;
    updateCursor();
    update();
}

void Canvas::updateCursor()
{
    // As in X-AnyLabeling: an open hand while Space is held, a closed one while panning.
    if (m_drag == Drag::Pan)
        setCursor(Qt::ClosedHandCursor);
    else if (m_spaceHeld)
        setCursor(Qt::OpenHandCursor);
    else
        setCursor(m_tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
}

bool Canvas::finishBox(const QPointF& start, const QPointF& end)
{
    if (m_tool == Tool::Line || m_tool == Tool::Circle) {
        // Lines and circles need only some length.
        if (distance(start, end) * m_scale < 2)
            return false;
        Shape shape;
        shape.type = m_tool == Tool::Line ? ShapeType::Line : ShapeType::Circle;
        shape.points = {start, end};
        emit shapeDrawn(shape);
        return true;
    }
    if (std::abs(end.x() - start.x()) * m_scale < 2 || std::abs(end.y() - start.y()) * m_scale < 2)
        return false;
    // A new rotated box starts upright with angle 0, as upstream draws it.
    Shape shape;
    shape.type = m_tool == Tool::Rotation ? ShapeType::Rotation : ShapeType::Rectangle;
    shape.points = canonicalRectangle({start, end});
    emit shapeDrawn(shape);
    return true;
}

void Canvas::finishPolygon()
{
    const bool strip = m_tool == Tool::LineStrip;
    if (m_polygonPoints.size() >= (strip ? 2 : 3)) {
        Shape shape;
        shape.type = strip ? ShapeType::LineStrip : ShapeType::Polygon;
        shape.points = m_polygonPoints;
        m_polygonPoints.clear();
        update();
        emit shapeDrawn(shape);
        return;
    }
    cancelDrawing();
}

void Canvas::mousePressEvent(QMouseEvent* event)
{
    const QPointF widgetPoint = event->position();
    m_pressWidget = widgetPoint;
    m_pressImage = widgetToImage(widgetPoint);
    m_currentImage = m_pressImage;

    if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton && m_spaceHeld)) {
        m_drag = Drag::Pan;
        m_panStart = widgetPoint;
        m_panButton = event->button();
        updateCursor();
        return;
    }
    if (event->button() != Qt::LeftButton || m_image.isNull())
        return;

    switch (m_tool) {
    case Tool::Rectangle:
    case Tool::Rotation:
    case Tool::Line:
    case Tool::Circle:
        if (m_readOnly)
            break;
        // Clicked corners, as upstream: the second click ends what the first one began.
        if (m_clickStart) {
            if (finishBox(*m_clickStart, clampToImage(m_pressImage))) {
                m_clickStart.reset();
                update();
            }
            break;
        }
        m_drag = Drag::DrawRectangle;
        break;
    case Tool::Point:
        if (!m_readOnly) {
            Shape shape;
            shape.type = ShapeType::Point;
            shape.points = {clampToImage(m_pressImage)};
            emit shapeDrawn(shape);
        }
        break;
    case Tool::Polygon:
    case Tool::LineStrip: {
        if (m_readOnly)
            break;
        const QPointF point = clampToImage(m_pressImage);
        if (m_tool == Tool::Polygon && m_polygonPoints.size() >= 3
            && distance(imageToWidget(m_polygonPoints.first()), widgetPoint) <= kCloseDistance) {
            finishPolygon();
            break;
        }
        m_polygonPoints.append(point);
        update();
        break;
    }
    case Tool::Select: {
        const int vertex = vertexAt(m_selected, widgetPoint);
        if (vertex >= 0) {
            m_drag = Drag::MoveVertex;
            m_dragVertex = vertex;
            m_dragPoints = m_document->shapes()[m_selected].points;
            break;
        }
        const qsizetype hit = shapeAt(m_pressImage);
        if (hit != m_selected) {
            m_selected = hit;
            emit selectionChanged(hit);
            update();
        }
        if (canEdit(hit)) {
            m_drag = Drag::MoveShape;
            m_dragPoints = m_document->shapes()[hit].points;
        }
        break;
    }
    }
}

void Canvas::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF widgetPoint = event->position();
    const QPointF imagePoint = widgetToImage(widgetPoint);
    m_mouseWidget = widgetPoint;
    m_mouseOver = true;
    reportCursor();
    if ((m_drag == Drag::MoveShape || m_drag == Drag::MoveVertex)
        && (!m_document || m_selected < 0 || m_selected >= m_document->shapes().size())) {
        m_drag = Drag::None;
        m_dragPoints.clear();
    }
    switch (m_drag) {
    case Drag::Pan:
        m_offset += widgetPoint - m_pressWidget;
        m_pressWidget = widgetPoint;
        m_fitPending = false;
        reportCursor();
        break;
    case Drag::DrawRectangle:
        m_currentImage = clampToImage(imagePoint);
        break;
    case Drag::MoveShape: {
        const PointList& original = m_document->shapes()[m_selected].points;
        QPointF delta = imagePoint - m_pressImage;
        // Keep the whole shape inside the image.
        double minX = original[0].x(), maxX = minX, minY = original[0].y(), maxY = minY;
        for (const QPointF& p : original) {
            minX = std::min(minX, p.x());
            maxX = std::max(maxX, p.x());
            minY = std::min(minY, p.y());
            maxY = std::max(maxY, p.y());
        }
        delta.setX(std::clamp(delta.x(), -minX, std::max(-minX, m_image.width() - maxX)));
        delta.setY(std::clamp(delta.y(), -minY, std::max(-minY, m_image.height() - maxY)));
        m_dragPoints = original;
        for (QPointF& p : m_dragPoints)
            p += delta;
        break;
    }
    case Drag::MoveVertex: {
        const Shape& shape = m_document->shapes()[m_selected];
        const QPointF point = clampToImage(imagePoint);
        if (shape.type == ShapeType::Rotation && shape.points.size() == 4) {
            // As upstream, the corners of a rotated box may leave the image.
            m_dragPoints = moveBoxCorner({shape.points, fittingDirection({shape.points, shape.direction})}, m_dragVertex,
                                         imagePoint);
        } else if (shape.type == ShapeType::Rectangle && shape.points.size() == 4) {
            // Corners are stored in order; the opposite corner stays.
            m_dragPoints = canonicalRectangle({shape.points[(m_dragVertex + 2) % 4], point});
        } else {
            m_dragPoints = shape.points;
            m_dragPoints[m_dragVertex] = point;
        }
        break;
    }
    case Drag::None:
        m_currentImage = clampToImage(imagePoint);
        if (clicksPoints() && m_polygonPoints.isEmpty())
            return;
        break;
    }
    update();
}

void Canvas::mouseReleaseEvent(QMouseEvent* event)
{
    const Drag drag = m_drag;
    m_drag = Drag::None;
    // A right click that did not pan: the shape under it is selected and its menu asked for,
    // as in X-AnyLabeling. Not while a shape is being drawn.
    if (drag == Drag::Pan && m_panButton == Qt::RightButton && event->button() == Qt::RightButton
        && distance(event->position(), m_panStart) <= kClickDistance && m_polygonPoints.isEmpty() && !m_clickStart
        && !m_image.isNull()) {
        const qsizetype hit = shapeAt(widgetToImage(event->position()));
        if (hit != m_selected) {
            m_selected = hit;
            emit selectionChanged(hit);
        }
        updateCursor();  // the pan that was not one is over
        update();
        emit contextMenuRequested(event->globalPosition().toPoint());
        return;
    }
    switch (drag) {
    case Drag::DrawRectangle: {
        const QPointF end = clampToImage(widgetToImage(event->position()));
        const QPointF start = clampToImage(m_pressImage);
        // Released where it was pressed: a click, so the next click sets the other corner.
        // Otherwise the drag drew it.
        if (distance(event->position(), m_pressWidget) <= kClickDistance)
            m_clickStart = start;
        else
            finishBox(start, end);
        break;
    }
    case Drag::Pan:
        updateCursor();
        break;
    case Drag::MoveShape:
    case Drag::MoveVertex: {
        const PointList points = m_dragPoints;
        m_dragPoints.clear();
        if (m_document && m_selected >= 0 && m_selected < m_document->shapes().size() && !points.isEmpty()
            && !exactlyEqual(points, m_document->shapes()[m_selected].points)) {
            const Shape& shape = m_document->shapes()[m_selected];
            const double direction = shape.type == ShapeType::Rotation ? fittingDirection({shape.points, shape.direction})
                                                                       : shape.direction;
            // A rotated box that had no fitting angle gets the one its corners were moved by.
            if (direction != shape.direction)
                emit shapeRotated(m_selected, {points, direction});
            else
                emit shapePointsChanged(m_selected, points);
        }
        break;
    }
    default:
        break;
    }
    update();
}

void Canvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    // Qt sends a quick second click as a double click: for clicked corners it is a click.
    if (m_tool == Tool::Rectangle || m_tool == Tool::Rotation || m_tool == Tool::Line || m_tool == Tool::Circle) {
        mousePressEvent(event);
        return;
    }
    if (clicksPoints()) {
        // The double click's first press already added its point.
        finishPolygon();
        return;
    }
    if (m_tool == Tool::Select && m_selected >= 0 && !m_readOnly)
        emit labelEditRequested(m_selected);
}

void Canvas::wheelEvent(QWheelEvent* event)
{
    if (m_image.isNull())
        return;
    event->accept();
    // As in X-AnyLabeling: Ctrl + wheel zooms, the wheel alone scrolls.
    if (event->modifiers() & Qt::ControlModifier) {
        const double steps = event->angleDelta().y() / 120.0;
        if (steps != 0)
            zoomAround(event->position(), std::pow(1.1, steps));
        return;
    }
    const QPoint pixels = event->pixelDelta();
    const QPointF step = pixels.isNull() ? QPointF(event->angleDelta()) / 2.0 : QPointF(pixels);
    if (step.isNull())
        return;
    m_offset += step;
    m_fitPending = false;
    update();
    reportCursor();
}

void Canvas::keyPressEvent(QKeyEvent* event)
{
    // Ctrl+Z also reaches here when the undo action is off because nothing is to undo yet.
    if (event->matches(QKeySequence::Undo) && removeLastPolygonPoint())
        return;
    switch (event->key()) {
    case Qt::Key_Space:
        if (!event->isAutoRepeat()) {
            m_spaceHeld = true;
            updateCursor();
        }
        return;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (m_tool == Tool::Select && m_selected >= 0) {
            const int key = event->key();
            moveSelectedBy(QPointF(key == Qt::Key_Left ? -1 : key == Qt::Key_Right ? 1 : 0,
                                   key == Qt::Key_Up ? -1 : key == Qt::Key_Down ? 1 : 0));
            return;
        }
        break;
    // Upstream's keys for turning a rotated box: Z and V by 1°, X and C by 0.1°.
    case Qt::Key_Z:
    case Qt::Key_X:
    case Qt::Key_C:
    case Qt::Key_V:
        if (m_tool == Tool::Select && event->modifiers() == Qt::NoModifier) {
            const int key = event->key();
            rotateSelectedBy(key == Qt::Key_Z ? 1.0 : key == Qt::Key_X ? 0.1 : key == Qt::Key_C ? -0.1 : -1.0);
            return;
        }
        break;
    case Qt::Key_Escape:
        if (!m_polygonPoints.isEmpty() || m_clickStart || m_drag != Drag::None)
            cancelDrawing();
        else if (m_selected >= 0) {
            m_selected = -1;
            emit selectionChanged(-1);
            update();
        }
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (clicksPoints()) {
            finishPolygon();
            return;
        }
        break;
    case Qt::Key_Backspace:
        if (removeLastPolygonPoint())
            return;
        break;
    case Qt::Key_Delete:
        if (m_selected >= 0 && !m_readOnly) {
            emit deleteRequested(m_selected);
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void Canvas::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        updateCursor();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void Canvas::focusOutEvent(QFocusEvent* event)
{
    m_spaceHeld = false;
    updateCursor();
    QWidget::focusOutEvent(event);
}

void Canvas::leaveEvent(QEvent* event)
{
    m_mouseOver = false;
    reportCursor();
    QWidget::leaveEvent(event);
}

void Canvas::reportCursor()
{
    if (!m_mouseOver || m_image.isNull()) {
        emit cursorMoved(QPointF(), false);
        return;
    }
    const QPointF imagePoint = widgetToImage(m_mouseWidget);
    emit cursorMoved(imagePoint, QRectF(QPointF(0, 0), QSizeF(m_image.size())).contains(imagePoint));
}

void Canvas::setLineWidth(double width)
{
    width = std::isfinite(width) ? std::clamp(width, kMinLineWidth, kMaxLineWidth) : kDefaultLineWidth;
    if (width == m_lineWidth)
        return;
    m_lineWidth = width;
    update();
}

QColor Canvas::colorFor(const QString& label) const
{
    const auto it = m_classColors.constFind(label);
    if (it != m_classColors.cend() && it->isValid())
        return *it;
    return QColor::fromHsv(int(qHash(label) % 360), 200, 230);
}

void Canvas::paintShape(QPainter& painter, const Shape& shape, const PointList& points, bool selected) const
{
    if (points.isEmpty())
        return;
    QColor color = shape.kind == ShapeKind::Editable ? colorFor(shape.label) : QColor(160, 160, 160);
    QPen pen(color, selected ? m_lineWidth + 1 : m_lineWidth);
    pen.setCosmetic(true);
    if (shape.kind != ShapeKind::Editable)
        pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    // Only the selected shape is tinted, so that the image under the others stays clear.
    QColor fill = color;
    fill.setAlpha(selected ? 56 : 0);

    const QTransform t = transform();
    QPolygonF polygon;
    for (const QPointF& p : points)
        polygon.append(t.map(p));

    switch (shape.type) {
    case ShapeType::Rectangle:
    case ShapeType::Polygon:
    case ShapeType::Rotation:
    case ShapeType::Quadrilateral:
    case ShapeType::Cuboid:
        painter.setBrush(fill);
        painter.drawPolygon(polygon);
        break;
    case ShapeType::Circle:
        if (polygon.size() == 2) {
            const double radius = distance(polygon[0], polygon[1]);
            painter.setBrush(fill);
            painter.drawEllipse(polygon[0], radius, radius);
        }
        break;
    case ShapeType::Line:
    case ShapeType::LineStrip:
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(polygon);
        break;
    case ShapeType::Point:
        painter.setBrush(color);
        painter.drawEllipse(polygon[0], 4, 4);
        break;
    }

    if (m_labelsVisible && !shape.label.isEmpty()) {
        // The label on a tag of the class colour above the shape's top left corner, in dark
        // or white text, whichever contrasts more with the colour.
        const QFontMetricsF metrics(painter.font());
        const QPointF corner = polygon.boundingRect().topLeft();
        const QRectF tag(corner.x() - pen.widthF() / 2, corner.y() - metrics.height() - 4,
                         metrics.horizontalAdvance(shape.label) + 10, metrics.height() + 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(tag, 2, 2);
        painter.setPen(readableTextOn(color));
        painter.drawText(tag, Qt::AlignCenter, shape.label);
    }
    if (selected) {
        painter.setPen(QPen(color, 1.5));
        painter.setBrush(Qt::white);
        for (const QPointF& p : polygon)
            painter.drawRect(QRectF(p - QPointF(kHandleRadius - 1, kHandleRadius - 1),
                                    QSizeF(2 * kHandleRadius - 2, 2 * kHandleRadius - 2)));
    }
}

void Canvas::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    if (m_image.isNull())
        return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, m_scale < 1.0);
    painter.setTransform(transform());
    painter.drawImage(QPointF(0, 0), m_image);
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (m_document) {
        const QList<Shape>& shapes = m_document->shapes();
        for (qsizetype i = 0; i < shapes.size(); ++i) {
            if (!m_hidden.contains(shapes[i].id))
                paintShape(painter, shapes[i], displayedPoints(i), i == m_selected);
        }
    }

    QPen pen(QColor(255, 220, 0), m_lineWidth);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (m_drag == Drag::DrawRectangle || m_clickStart) {
        const QPointF start = imageToWidget(m_clickStart ? *m_clickStart : clampToImage(m_pressImage));
        const QPointF end = imageToWidget(m_currentImage);
        if (m_tool == Tool::Line)
            painter.drawLine(start, end);
        else if (m_tool == Tool::Circle)
            painter.drawEllipse(start, distance(start, end), distance(start, end));
        else
            painter.drawRect(QRectF(start, end).normalized());
    }
    if (!m_polygonPoints.isEmpty()) {
        QPolygonF line;
        for (const QPointF& p : m_polygonPoints)
            line.append(imageToWidget(p));
        line.append(imageToWidget(m_currentImage));
        painter.drawPolyline(line);
        for (const QPointF& p : m_polygonPoints)
            painter.drawEllipse(imageToWidget(p), 3, 3);
    }
}

} // namespace liteanno::ui
