#pragma once

#include <QPoint>
#include <QList>

#include <cstdint>
#include <vector>

namespace liteanno::formats {

using IntPolygon = QList<QPoint>;

// The polygons X-AnyLabeling 4.2.0 makes of one class in a mask
// (LabelConverter.get_contours_and_labels): the outer borders of the regions where `mask` is
// not 0 (cv2.findContours, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE), each with at least 3 points,
// simplified with cv2.approxPolyDP at 0.001 times its length, those with fewer than 3 points
// left out. In OpenCV's order, which lists the regions found last first. `mask` holds
// width × height values, row by row.
QList<IntPolygon> maskPolygons(const std::vector<std::uint8_t>& mask, int width, int height);

// The steps on their own, for the tests.
QList<IntPolygon> externalContours(const std::vector<std::uint8_t>& mask, int width, int height);
double closedArcLength(const IntPolygon& contour);
IntPolygon approximateClosedPolygon(const IntPolygon& contour, double epsilon);

} // namespace liteanno::formats
