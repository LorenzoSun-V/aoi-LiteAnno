#pragma once

#include <QList>
#include <QPointF>

namespace liteanno {

// Image coordinates: origin at the top-left corner, x to the right, y down, in pixels
// (format spec 4.2).
using PointList = QList<QPointF>;

// Bit-exact comparison. QPointF::operator== is fuzzy and would hide small edits.
bool exactlyEqual(const PointList& a, const PointList& b);

// True when every coordinate is a finite number. JSON has no NaN or infinity, so other
// points cannot be saved (format spec 4.2, 13.5).
bool allFinite(const PointList& points);

// The corners of the bounding box in the order upstream writes rectangles:
// (xmin, ymin), (xmax, ymin), (xmax, ymax), (xmin, ymax). Accepts the legacy two-point
// diagonal form and four points in any order. Returns an empty list for empty input.
PointList canonicalRectangle(const PointList& points);

// Rotated boxes (shape_type "rotation"). Upstream keeps four corners and an angle,
// "direction", in radians in [0, 2π). These follow upstream's canvas.py step by step, so
// that a box edited here holds the numbers upstream would write.
struct RotatedBox {
    PointList points;
    double direction = 0;
};

// The box turned by `theta` radians about the middle of corners 0 and 2, as the keys
// Z/X/C/V do (canvas.py, bounded_rotate_shapes). Expects four corners.
RotatedBox rotateBox(const RotatedBox& box, double theta);

// The box with corner `index` moved to `position`: the opposite corner stays and the sides
// keep the box's angle (canvas.py, bounded_move_vertex with get_adjoint_points).
PointList moveBoxCorner(const RotatedBox& box, int index, const QPointF& position);

// The box's angle as its corners show it. Turning an upright box (canvas.py) takes its top
// side, corners 0 to 1, along the angle, so that side gives it. `box.direction` is returned
// when it fits that side, so boxes upstream wrote keep their exact numbers; a file without
// "direction" (read as 0) or with one that does not fit gets the angle of the corners.
double fittingDirection(const RotatedBox& box);

// Python's float modulo: the result has the sign of `divisor`.
double pythonModulo(double value, double divisor);
// Python's math.radians.
double radians(double degrees);

} // namespace liteanno
