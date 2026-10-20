#pragma once

#include <QList>
#include <QPointF>

namespace liteanno {

// Image coordinates: origin at the top-left corner, x to the right, y down, in pixels
// (format spec 4.2).
using PointList = QList<QPointF>;

// Bit-exact comparison. QPointF::operator== is fuzzy and would hide small edits.
bool exactlyEqual(const PointList& a, const PointList& b);

// The corners of the bounding box in the order upstream writes rectangles:
// (xmin, ymin), (xmax, ymin), (xmax, ymax), (xmin, ymax). Accepts the legacy two-point
// diagonal form and four points in any order. Returns an empty list for empty input.
PointList canonicalRectangle(const PointList& points);

} // namespace liteanno
