// The polygon filling below is a port of OpenCV 5.0.0's cv::fillPoly for an 8-bit, one
// channel image with LINE_8 and no shift (modules/imgproc/src/drawing.cpp: clipLine,
// LineIterator, Line, CollectPolyEdges, FillEdgeCollection), so that areas are the same
// pixel counts X-AnyLabeling computes with it.
//
// OpenCV is licensed under the Apache License, Version 2.0; see
// third_party/opencv/LICENSE. Copyright (C) 2000-2022, Intel Corporation, all rights
// reserved; Copyright (C) 2009-2011, Willow Garage Inc.; Copyright (C) 2015-2023, OpenCV
// Foundation and other contributors.

#include "formats/PolygonRaster.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

namespace liteanno::formats {

namespace {

enum { XY_SHIFT = 16, XY_ONE = 1 << XY_SHIFT };

struct Point64 {
    int64_t x = 0;
    int64_t y = 0;
};

struct Mask {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
    unsigned char color = 1;

    void set(int x, int y) { pixels[size_t(y) * size_t(width) + size_t(x)] = color; }
    void hline(int y, int x1, int x2)
    {
        std::fill(pixels.begin() + std::ptrdiff_t(size_t(y) * size_t(width) + size_t(x1)),
                  pixels.begin() + std::ptrdiff_t(size_t(y) * size_t(width) + size_t(x2) + 1), color);
    }
};

struct PolyEdge {
    int y0 = 0;
    int y1 = 0;
    int64_t x = 0;
    int64_t dx = 0;
    PolyEdge* next = nullptr;
};

// cv::clipLine(Size2l, Point2l&, Point2l&).
bool clipLine(int64_t width, int64_t height, Point64& pt1, Point64& pt2)
{
    int c1, c2;
    const int64_t right = width - 1, bottom = height - 1;
    if (width <= 0 || height <= 0)
        return false;
    int64_t &x1 = pt1.x, &y1 = pt1.y, &x2 = pt2.x, &y2 = pt2.y;
    c1 = (x1 < 0) + (x1 > right) * 2 + (y1 < 0) * 4 + (y1 > bottom) * 8;
    c2 = (x2 < 0) + (x2 > right) * 2 + (y2 < 0) * 4 + (y2 > bottom) * 8;
    if ((c1 & c2) == 0 && (c1 | c2) != 0) {
        int64_t a;
        if (c1 & 12) {
            a = c1 < 8 ? 0 : bottom;
            x1 += (int64_t)((double)(a - y1) * (x2 - x1) / (y2 - y1));
            y1 = a;
            c1 = (x1 < 0) + (x1 > right) * 2;
        }
        if (c2 & 12) {
            a = c2 < 8 ? 0 : bottom;
            x2 += (int64_t)((double)(a - y2) * (x2 - x1) / (y2 - y1));
            y2 = a;
            c2 = (x2 < 0) + (x2 > right) * 2;
        }
        if ((c1 & c2) == 0 && (c1 | c2) != 0) {
            if (c1) {
                a = c1 == 1 ? 0 : right;
                y1 += (int64_t)((double)(a - x1) * (y2 - y1) / (x2 - x1));
                x1 = a;
                c1 = 0;
            }
            if (c2) {
                a = c2 == 1 ? 0 : right;
                y2 += (int64_t)((double)(a - x2) * (y2 - y1) / (x2 - x1));
                x2 = a;
                c2 = 0;
            }
        }
    }
    return (c1 | c2) == 0;
}

// cv::Line with connectivity 8: LineIterator(img, pt1, pt2, 8, leftToRight = true).
void drawLine(Mask& img, Point64 a, Point64 b)
{
    // The int Points of LineIterator::init.
    int x1 = int(a.x), y1 = int(a.y), x2 = int(b.x), y2 = int(b.y);
    if ((unsigned)x1 >= (unsigned)img.width || (unsigned)x2 >= (unsigned)img.width
        || (unsigned)y1 >= (unsigned)img.height || (unsigned)y2 >= (unsigned)img.height) {
        Point64 p1{x1, y1}, p2{x2, y2};
        if (!clipLine(img.width, img.height, p1, p2))
            return;
        x1 = int(p1.x);
        y1 = int(p1.y);
        x2 = int(p2.x);
        y2 = int(p2.y);
    }
    int deltaX = 1, deltaY = 1;
    int dx = x2 - x1;
    int dy = y2 - y1;
    if (dx < 0) {
        dx = -dx;
        dy = -dy;
        x1 = x2;
        y1 = y2;
    }
    if (dy < 0) {
        dy = -dy;
        deltaY = -1;
    }
    const bool vert = dy > dx;
    if (vert) {
        std::swap(dx, dy);
        std::swap(deltaX, deltaY);
    }
    int err = dx - (dy + dy);
    const int plusDelta = dx + dx;
    const int minusDelta = -(dy + dy);
    int minusShift = deltaX;
    int plusShift = 0;
    int minusStep = 0;
    int plusStep = deltaY;
    const int count = dx + 1;
    if (vert) {
        std::swap(plusStep, plusShift);
        std::swap(minusStep, minusShift);
    }
    int x = x1, y = y1;
    for (int i = 0; i < count; ++i) {
        img.set(x, y);
        const int mask = err < 0 ? -1 : 0;
        err += minusDelta + (plusDelta & mask);
        x += minusShift + (plusShift & mask);
        y += minusStep + (plusStep & mask);
    }
}

void collectPolyEdges(Mask& img, const std::vector<Point64>& v, std::vector<PolyEdge>& edges)
{
    const int count = int(v.size());
    Point64 pt0 = v[size_t(count - 1)], pt1;
    pt0.x = pt0.x << XY_SHIFT;
    edges.reserve(edges.size() + size_t(count));
    for (int i = 0; i < count; i++, pt0 = pt1) {
        Point64 t0, t1;
        PolyEdge edge;
        pt1 = v[size_t(i)];
        pt1.x = pt1.x << XY_SHIFT;
        Point64 pt0c(pt0), pt1c(pt1);

        t0.y = pt0.y;
        t1.y = pt1.y;
        t0.x = (pt0.x + (XY_ONE >> 1)) >> XY_SHIFT;
        t1.x = (pt1.x + (XY_ONE >> 1)) >> XY_SHIFT;
        drawLine(img, t0, t1);

        if ((uint64_t)t0.x >= (uint64_t)img.width || (uint64_t)t1.x >= (uint64_t)img.width
            || (uint64_t)t0.y >= (uint64_t)img.height || (uint64_t)t1.y >= (uint64_t)img.height) {
            clipLine(img.width, img.height, t0, t1);
            if (t0.y != t1.y) {
                pt0c.y = t0.y;
                pt1c.y = t1.y;
            }
        }
        pt0c.x = t0.x << XY_SHIFT;
        pt1c.x = t1.x << XY_SHIFT;

        if (pt0.y == pt1.y)
            continue;
        edge.dx = (pt1c.x - pt0c.x) / (pt1c.y - pt0c.y);
        if (pt0.y < pt1.y) {
            edge.y0 = int(pt0.y);
            edge.y1 = int(pt1.y);
            edge.x = pt0c.x + (pt0.y - pt0c.y) * edge.dx;
        } else {
            edge.y0 = int(pt1.y);
            edge.y1 = int(pt0.y);
            edge.x = pt1c.x + (pt1.y - pt1c.y) * edge.dx;
        }
        edges.push_back(edge);
    }
}

void fillEdgeCollection(Mask& img, std::vector<PolyEdge>& edges)
{
    PolyEdge tmp;
    int i, y, total = int(edges.size());
    PolyEdge* e;
    int y_max = INT_MIN, y_min = INT_MAX;
    int64_t x_max = int64_t(0xFFFFFFFFFFFFFFFF), x_min = 0x7FFFFFFFFFFFFFFF;
    const int delta = XY_ONE - 1;
    if (total < 2)
        return;
    for (i = 0; i < total; i++) {
        PolyEdge& e1 = edges[size_t(i)];
        const int64_t x1 = e1.x + (e1.y1 - e1.y0) * e1.dx;
        y_min = std::min(y_min, e1.y0);
        y_max = std::max(y_max, e1.y1);
        x_min = std::min(x_min, e1.x);
        x_max = std::max(x_max, e1.x);
        x_min = std::min(x_min, x1);
        x_max = std::max(x_max, x1);
    }
    if (y_max < 0 || y_min >= img.height || x_max < 0 || x_min >= ((int64_t)img.width << XY_SHIFT))
        return;
    std::sort(edges.begin(), edges.end(), [](const PolyEdge& e1, const PolyEdge& e2) {
        return e1.y0 - e2.y0 ? e1.y0 < e2.y0 : e1.x - e2.x ? e1.x < e2.x : e1.dx < e2.dx;
    });

    tmp.y0 = INT_MAX;
    edges.push_back(tmp);
    i = 0;
    tmp.next = nullptr;
    e = &edges[size_t(i)];
    y_max = std::min(y_max, img.height);

    for (y = e->y0; y < y_max; y++) {
        PolyEdge *last, *prelast, *keep_prelast;
        int draw = 0;
        const int clipline = y < 0;
        prelast = &tmp;
        last = tmp.next;
        while (last || e->y0 == y) {
            if (last && last->y1 == y) {
                prelast->next = last->next;
                last = last->next;
                continue;
            }
            keep_prelast = prelast;
            if (last && (e->y0 > y || last->x < e->x)) {
                prelast = last;
                last = last->next;
            } else if (i < total) {
                prelast->next = e;
                e->next = last;
                prelast = e;
                e = &edges[size_t(++i)];
            } else {
                break;
            }
            if (draw) {
                if (!clipline) {
                    int x1, x2;
                    if (keep_prelast->x > prelast->x) {
                        x1 = int((prelast->x + delta) >> XY_SHIFT);
                        x2 = int(keep_prelast->x >> XY_SHIFT);
                    } else {
                        x1 = int((keep_prelast->x + delta) >> XY_SHIFT);
                        x2 = int(prelast->x >> XY_SHIFT);
                    }
                    if (x1 < img.width && x2 >= 0) {
                        if (x1 < 0)
                            x1 = 0;
                        if (x2 >= img.width)
                            x2 = img.width - 1;
                        if (x1 <= x2)
                            img.hline(y, x1, x2);
                    }
                }
                keep_prelast->x += keep_prelast->dx;
                prelast->x += prelast->dx;
            }
            draw ^= 1;
        }

        keep_prelast = nullptr;
        do {
            prelast = &tmp;
            last = tmp.next;
            PolyEdge* last_exchange = nullptr;
            while (last != keep_prelast && last->next != nullptr) {
                PolyEdge* te = last->next;
                if (last->x > te->x) {
                    prelast->next = te;
                    last->next = te->next;
                    te->next = last;
                    prelast = te;
                    last_exchange = prelast;
                } else {
                    prelast = last;
                    last = te;
                }
            }
            if (last_exchange == nullptr)
                break;
            keep_prelast = last_exchange;
        } while (keep_prelast != tmp.next && keep_prelast != &tmp);
    }
}

} // namespace

double rasterizedPolygonArea(const std::vector<RasterPolygon>& polygons)
{
    std::vector<const RasterPolygon*> valid;
    for (const RasterPolygon& polygon : polygons) {
        if (polygon.size() >= 3)
            valid.push_back(&polygon);
    }
    if (valid.empty())
        return 0.0;
    double minX = valid.front()->front().first, maxX = minX;
    double minY = valid.front()->front().second, maxY = minY;
    for (const RasterPolygon* polygon : valid) {
        for (const auto& [x, y] : *polygon) {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    const double offsetX = -std::floor(minX);
    const double offsetY = -std::floor(minY);
    Mask mask;
    mask.height = std::max(1, int(std::ceil(maxY) - std::floor(minY)));
    mask.width = std::max(1, int(std::ceil(maxX) - std::floor(minX)));
    mask.pixels.assign(size_t(mask.width) * size_t(mask.height), 0);
    for (const RasterPolygon* polygon : valid) {
        std::vector<Point64> points;
        points.reserve(polygon->size());
        // np.round rounds half to even, as nearbyint does in the default rounding mode.
        for (const auto& [x, y] : *polygon)
            points.push_back({int64_t(int32_t(std::nearbyint(x + offsetX))), int64_t(int32_t(std::nearbyint(y + offsetY)))});
        std::vector<PolyEdge> edges;
        collectPolyEdges(mask, points, edges);
        fillEdgeCollection(mask, edges);
    }
    return double(std::count(mask.pixels.begin(), mask.pixels.end(), 1));
}

void fillPolygon(std::vector<unsigned char>& pixels, int width, int height,
                 const std::vector<std::pair<int, int>>& points, unsigned char value)
{
    if (points.empty() || width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height))
        return;
    Mask mask;
    mask.width = width;
    mask.height = height;
    mask.pixels.swap(pixels);
    mask.color = value;
    std::vector<Point64> converted;
    converted.reserve(points.size());
    for (const auto& [x, y] : points)
        converted.push_back({x, y});
    std::vector<PolyEdge> edges;
    collectPolyEdges(mask, converted, edges);
    fillEdgeCollection(mask, edges);
    mask.pixels.swap(pixels);
}

} // namespace liteanno::formats
