#include "formats/MaskContours.h"

#include <algorithm>
#include <cmath>

// A port of the parts of OpenCV 5.0.0 that X-AnyLabeling's mask import goes through
// (third_party/opencv/README.md):
// - modules/imgproc/src/contours_new.cpp: ContourScanner_ (findNext, contourScan,
//   makeContour) and icvFetchContourEx, for RETR_EXTERNAL and CHAIN_APPROX_SIMPLE on an
//   8-bit image, and contourTreeToResults for the order of the results;
// - modules/geometry/src/geometry.cpp: arcLength;
// - modules/geometry/src/approx.cpp: approxPolyDP_ for integer points.
// The arithmetic is kept as it is there (float lengths, int differences), since the points
// written must be the same.

namespace liteanno::formats {

namespace {

constexpr int kDeltas[8][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 1}};
constexpr std::int8_t kNew = 0x02;
constexpr std::int8_t kRight = std::int8_t(0x80);
constexpr std::int8_t kFlags = std::int8_t(0xFE);

class ExternalScanner
{
public:
    ExternalScanner(const std::vector<std::uint8_t>& mask, int width, int height)
        : m_width(width + 2)
        , m_height(height + 2)
        , m_image(std::size_t(m_width) * std::size_t(m_height), 0)
    {
        // copyMakeBorder with one pixel of 0, then threshold to 0 and 1.
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x)
                m_image[index(x + 1, y + 1)] = mask[std::size_t(y) * std::size_t(width) + std::size_t(x)] != 0 ? 1 : 0;
        }
    }

    // In the order they are found.
    QList<IntPolygon> scan()
    {
        QList<IntPolygon> found;
        int x = 1;
        int y = 1;
        int lastX = 0;
        int lastY = 1;
        int prev = m_image[index(0, 1)];
        const int width = m_width - 1;
        const int height = m_height - 1;
        for (; y < height; ++y) {
            int p = 0;
            for (; x < width; ++x) {
                for (; x < width && (p = m_image[index(x, y)]) == prev; ++x) {
                }
                if (x >= width)
                    break;
                // contourScan: only outer borders not inside a border already followed.
                bool start = false;
                if (prev == 0 && p == 1) {
                    start = m_image[index(lastX, lastY)] <= 0;
                } else if (p == 0 && prev >= 1) {
                    if (prev & kFlags)
                        lastX = x - 1;
                }
                if (start) {
                    lastX = x;
                    found.append(follow(x, y));
                    // The scan goes on after the start point, with the pixel as followed.
                    prev = m_image[index(x, y)];
                    continue;
                }
                prev = p;
                if (prev & kFlags)
                    lastX = x;
            }
            lastX = 0;
            lastY = y + 1;
            x = 1;
            prev = 0;
        }
        return found;
    }

private:
    std::size_t index(int x, int y) const { return std::size_t(y) * std::size_t(m_width) + std::size_t(x); }
    std::ptrdiff_t delta(int s) const { return kDeltas[s % 8][0] + std::ptrdiff_t(kDeltas[s % 8][1]) * m_width; }

    // icvFetchContourEx for an outer border, without the points in the middle of straight
    // runs (CHAIN_APPROX_SIMPLE). Points are without the border pixel.
    IntPolygon follow(int startX, int startY)
    {
        IntPolygon points;
        QPoint pt(startX - 1, startY - 1);
        const std::ptrdiff_t i0 = std::ptrdiff_t(index(startX, startY));
        std::ptrdiff_t i1 = 0;
        std::ptrdiff_t i3 = 0;
        std::ptrdiff_t i4 = 0;
        int sEnd = 4;
        int s = sEnd;
        do {
            s = (s - 1) & 7;
            i1 = i0 + delta(s);
        } while (m_image[std::size_t(i1)] == 0 && s != sEnd);

        if (s == sEnd) {
            m_image[std::size_t(i0)] = kNew | kRight;
            points.append(pt);
            return points;
        }
        i3 = i0;
        int prevS = s ^ 4;
        for (;;) {
            sEnd = s;
            s = std::min(s, 15);
            while (s < 15) {
                ++s;
                i4 = i3 + delta(s);
                if (m_image[std::size_t(i4)] != 0)
                    break;
            }
            s &= 7;
            if (unsigned(s - 1) < unsigned(sEnd))
                m_image[std::size_t(i3)] = kNew | kRight;
            else if (m_image[std::size_t(i3)] == 1)
                m_image[std::size_t(i3)] = kNew;
            if (s != prevS)
                points.append(pt);
            prevS = s;
            pt += QPoint(kDeltas[s][0], kDeltas[s][1]);
            if (i4 == i0 && i3 == i1)
                break;
            i3 = i4;
            s = (s + 4) & 7;
        }
        return points;
    }

    const int m_width;
    const int m_height;
    std::vector<std::int8_t> m_image;
};

struct Range {
    int start = 0;
    int end = 0;
};

} // namespace

QList<IntPolygon> externalContours(const std::vector<std::uint8_t>& mask, int width, int height)
{
    if (width <= 0 || height <= 0)
        return {};
    QList<IntPolygon> found = ExternalScanner(mask, width, height).scan();
    // Each new contour becomes the first child of the root; the results list the children
    // from the first one on.
    std::reverse(found.begin(), found.end());
    return found;
}

double closedArcLength(const IntPolygon& contour)
{
    const qsizetype count = contour.size();
    if (count <= 1)
        return 0.;
    double perimeter = 0;
    float prevX = float(contour[count - 1].x());
    float prevY = float(contour[count - 1].y());
    for (const QPoint& point : contour) {
        const float px = float(point.x());
        const float py = float(point.y());
        const float dx = px - prevX;
        const float dy = py - prevY;
        perimeter += std::sqrt(dx * dx + dy * dy);
        prevX = px;
        prevY = py;
    }
    return perimeter;
}

IntPolygon approximateClosedPolygon(const IntPolygon& contour, double eps)
{
    const int count0 = int(contour.size());
    if (count0 == 0)
        return {};
    const QPoint* src = contour.constData();
    std::vector<QPoint> dst(static_cast<std::size_t>(count0));
    std::vector<Range> stack;
    int count = count0;
    int newCount = 0;
    int pos = 0;
    Range slice;
    Range rightSlice;
    QPoint startPt(-1000000, -1000000);
    QPoint endPt(0, 0);
    QPoint pt(0, 0);
    bool leEps = false;
    const auto readPt = [&](QPoint& out, int& at) {
        out = src[at];
        if (++at >= count)
            at = 0;
    };
    const auto readDstPt = [&](QPoint& out, int& at) {
        out = dst[std::size_t(at)];
        if (++at >= count)
            at = 0;
    };

    eps *= eps;

    // 1. Find approximately two farthest points of the contour.
    rightSlice.start = 0;
    for (int i = 0; i < 3; ++i) {
        double maxDist = 0;
        pos = (pos + rightSlice.start) % count;
        readPt(startPt, pos);
        for (int j = 1; j < count; ++j) {
            readPt(pt, pos);
            const double dx = pt.x() - startPt.x();
            const double dy = pt.y() - startPt.y();
            const double dist = dx * dx + dy * dy;
            if (dist > maxDist) {
                maxDist = dist;
                rightSlice.start = j;
            }
        }
        leEps = maxDist <= eps;
    }

    // 2. Initialize the stack.
    if (!leEps) {
        rightSlice.end = slice.start = pos % count;
        slice.end = rightSlice.start = (rightSlice.start + slice.start) % count;
        stack.push_back(rightSlice);
        stack.push_back(slice);
    } else {
        dst[std::size_t(newCount++)] = startPt;
    }

    // 3. Run the recursive process.
    while (!stack.empty()) {
        slice = stack.back();
        stack.pop_back();
        endPt = src[slice.end];
        pos = slice.start;
        readPt(startPt, pos);
        if (pos != slice.end) {
            double maxDist2MulSegmentLen2 = 0;
            const double dx = endPt.x() - startPt.x();
            const double dy = endPt.y() - startPt.y();
            const double segmentLen2 = dx * dx + dy * dy;
            while (pos != slice.end) {
                readPt(pt, pos);
                const double projection = ((pt.x() - startPt.x()) * dx + (pt.y() - startPt.y()) * dy);
                double dist2MulSegmentLen2 = 0;
                if (projection < 0) {
                    dist2MulSegmentLen2 = ((pt.x() - startPt.x()) * (pt.x() - startPt.x())
                                           + (pt.y() - startPt.y()) * (pt.y() - startPt.y()))
                                          * segmentLen2;
                } else if (projection > segmentLen2) {
                    dist2MulSegmentLen2 = ((pt.x() - endPt.x()) * (pt.x() - endPt.x())
                                           + (pt.y() - endPt.y()) * (pt.y() - endPt.y()))
                                          * segmentLen2;
                } else {
                    const double dist = ((pt.y() - startPt.y()) * dx - (pt.x() - startPt.x()) * dy);
                    dist2MulSegmentLen2 = dist * dist;
                }
                if (dist2MulSegmentLen2 > maxDist2MulSegmentLen2) {
                    maxDist2MulSegmentLen2 = dist2MulSegmentLen2;
                    rightSlice.start = (pos + count - 1) % count;
                }
            }
            leEps = maxDist2MulSegmentLen2 <= eps * segmentLen2;
        } else {
            leEps = true;
            startPt = src[slice.start];
        }
        if (leEps) {
            dst[std::size_t(newCount++)] = startPt;
        } else {
            rightSlice.end = slice.end;
            slice.end = rightSlice.start;
            stack.push_back(rightSlice);
            stack.push_back(slice);
        }
    }

    // Last stage: remove extra points on the [almost] straight lines.
    count = newCount;
    pos = count - 1;
    readDstPt(startPt, pos);
    int wpos = pos;
    readDstPt(pt, pos);
    for (int i = 0; i < count && newCount > 2; ++i) {
        readDstPt(endPt, pos);
        const double dx = endPt.x() - startPt.x();
        const double dy = endPt.y() - startPt.y();
        const double dist = std::fabs((pt.x() - startPt.x()) * dy - (pt.y() - startPt.y()) * dx);
        const double successiveInnerProduct =
            (pt.x() - startPt.x()) * (endPt.x() - pt.x()) + (pt.y() - startPt.y()) * (endPt.y() - pt.y());
        if (dist * dist <= 0.5 * eps * (dx * dx + dy * dy) && dx != 0 && dy != 0 && successiveInnerProduct >= 0) {
            --newCount;
            dst[std::size_t(wpos)] = startPt = endPt;
            if (++wpos >= count)
                wpos = 0;
            readDstPt(pt, pos);
            ++i;
            continue;
        }
        dst[std::size_t(wpos)] = startPt = pt;
        if (++wpos >= count)
            wpos = 0;
        pt = endPt;
    }
    return IntPolygon(dst.begin(), dst.begin() + newCount);
}

QList<IntPolygon> maskPolygons(const std::vector<std::uint8_t>& mask, int width, int height)
{
    QList<IntPolygon> polygons;
    for (const IntPolygon& contour : externalContours(mask, width, height)) {
        if (contour.size() < 3)
            continue;
        const IntPolygon approx = approximateClosedPolygon(contour, 0.001 * closedArcLength(contour));
        if (approx.size() < 3)
            continue;
        polygons.append(approx);
    }
    return polygons;
}

} // namespace liteanno::formats
