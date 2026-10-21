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

} // namespace liteanno
