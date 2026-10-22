#pragma once

#include <utility>
#include <vector>

namespace liteanno::formats {

using RasterPolygon = std::vector<std::pair<double, double>>;

// The area X-AnyLabeling 4.2.0 writes for a COCO segmentation (label_converter.py,
// calculate_polygon_area): the polygons are shifted onto the integer grid, rounded half to
// even, filled one by one with OpenCV's cv2.fillPoly (8-connected outline included) and the
// pixels counted. Polygons with fewer than 3 points are ignored.
double rasterizedPolygonArea(const std::vector<RasterPolygon>& polygons);

} // namespace liteanno::formats
