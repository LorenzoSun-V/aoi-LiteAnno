#include "core/Geometry.h"

#include <algorithm>
#include <cmath>

namespace liteanno {

bool exactlyEqual(const PointList& a, const PointList& b)
{
    return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend(), [](const QPointF& p, const QPointF& q) {
        return p.x() == q.x() && p.y() == q.y();
    });
}

bool allFinite(const PointList& points)
{
    return std::all_of(points.cbegin(), points.cend(),
                       [](const QPointF& point) { return std::isfinite(point.x()) && std::isfinite(point.y()); });
}

PointList canonicalRectangle(const PointList& points)
{
    if (points.isEmpty())
        return {};
    double xmin = points.first().x();
    double xmax = xmin;
    double ymin = points.first().y();
    double ymax = ymin;
    for (const QPointF& point : points) {
        xmin = std::min(xmin, point.x());
        xmax = std::max(xmax, point.x());
        ymin = std::min(ymin, point.y());
        ymax = std::max(ymax, point.y());
    }
    return {QPointF(xmin, ymin), QPointF(xmax, ymin), QPointF(xmax, ymax), QPointF(xmin, ymax)};
}

namespace {

constexpr double kPi = 3.141592653589793;

// canvas.py, rotate_point.
QPointF rotatePoint(const QPointF& p, const QPointF& center, double theta)
{
    const QPointF order = p - center;
    const double cosTheta = std::cos(theta);
    const double sinTheta = std::sin(theta);
    const double x = cosTheta * order.x() + sinTheta * order.y();
    const double y = -sinTheta * order.x() + cosTheta * order.y();
    return QPointF(center.x() + x, center.y() + y);
}

// canvas.py, get_cross_point.
QPointF crossPoint(double a1, double b1, double a2, double b2)
{
    return QPointF((b2 - b1) / (a1 - a2), (a1 * b2 - a2 * b1) / (a1 - a2));
}

} // namespace

double pythonModulo(double value, double divisor)
{
    double result = std::fmod(value, divisor);
    if (result != 0) {
        if ((divisor < 0) != (result < 0))
            result += divisor;
    } else {
        result = std::copysign(0.0, divisor);
    }
    return result;
}

double radians(double degrees)
{
    return degrees * (kPi / 180.0);
}

RotatedBox rotateBox(const RotatedBox& box, double theta)
{
    if (box.points.size() != 4)
        return box;
    const QPointF center((box.points[0].x() + box.points[2].x()) / 2, (box.points[0].y() + box.points[2].y()) / 2);
    RotatedBox turned;
    for (const QPointF& point : box.points)
        turned.points.append(rotatePoint(point, center, theta));
    turned.direction = pythonModulo(box.direction - theta, 2 * kPi);
    return turned;
}

PointList moveBoxCorner(const RotatedBox& box, int index, const QPointF& position)
{
    PointList points = box.points;
    if (points.size() != 4 || index < 0 || index > 3)
        return points;
    const QPointF p3 = points[(index + 2) % 4];
    const QPointF& p1 = position;
    QPointF p2;
    QPointF p4;
    const double a1 = std::tan(box.direction);
    if (a1 == 0) {
        if (index % 2 == 0) {
            p2 = QPointF(p3.x(), p1.y());
            p4 = QPointF(p1.x(), p3.y());
        } else {
            p4 = QPointF(p3.x(), p1.y());
            p2 = QPointF(p1.x(), p3.y());
        }
    } else {
        const double a3 = a1;
        const double a2 = -1 / a1;
        const double a4 = -1 / a1;
        const double b1 = p1.y() - a1 * p1.x();
        const double b2 = p1.y() - a2 * p1.x();
        const double b3 = p3.y() - a1 * p3.x();
        const double b4 = p3.y() - a2 * p3.x();
        if (index % 2 == 0) {
            p2 = crossPoint(a1, b1, a4, b4);
            p4 = crossPoint(a2, b2, a3, b3);
        } else {
            p4 = crossPoint(a1, b1, a4, b4);
            p2 = crossPoint(a2, b2, a3, b3);
        }
    }
    // shape.move_vertex_by(index, pos - point): the corner moves by the difference.
    points[index] = points[index] + (position - points[index]);
    points[(index + 1) % 4] = p2;
    points[(index + 3) % 4] = p4;
    return points;
}

} // namespace liteanno
