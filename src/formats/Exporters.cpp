#include "formats/Exporters.h"

#include "core/FormatVersion.h"
#include "core/Shape.h"
#include "formats/JsonText.h"
#include "formats/PolygonRaster.h"

#include <QFileInfo>
#include <QIODevice>

#include <charconv>
#include <cmath>
#include <limits>

namespace liteanno::formats {

namespace {

PyNumber numberOf(const Json& value)
{
    if (value.is_number_integer() || value.is_number_unsigned())
        return {value.get<double>(), true};
    if (value.is_number_float())
        return {value.get<double>(), false};
    if (value.is_boolean())
        return {value.get<bool>() ? 1.0 : 0.0, true};
    return {};
}

PyNumber integer(double value)
{
    return {value, true};
}

// Python's min(a, b) and max(a, b): the first argument unless the second is smaller (larger).
PyNumber pyMin(PyNumber a, PyNumber b)
{
    return b.value < a.value ? b : a;
}

PyNumber pyMax(PyNumber a, PyNumber b)
{
    return b.value > a.value ? b : a;
}

PyNumber pySub(PyNumber a, PyNumber b)
{
    return {a.value - b.value, a.isInteger && b.isInteger};
}

PyNumber pyMul(PyNumber a, PyNumber b)
{
    return {a.value * b.value, a.isInteger && b.isInteger};
}

// int(x): truncates toward zero.
qint64 pyInt(PyNumber number)
{
    return qint64(std::trunc(number.value));
}

std::string pyStr(PyNumber number)
{
    if (number.isInteger)
        return std::to_string(qint64(number.value));
    return pythonFloatRepr(number.value);
}

Json pyJson(PyNumber number)
{
    if (number.isInteger)
        return Json(qint64(number.value));
    return Json(number.value);
}

std::string fixed6(double value)
{
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 6);
    return std::string(buffer, converted.ptr);
}

using Point = std::array<PyNumber, 2>;
using Points = QList<Point>;

// LabelConverter.clamp_points: into [0, width - 1] x [0, height - 1].
Points clamped(const Points& points, int width, int height)
{
    Points result;
    result.reserve(points.size());
    for (const Point& p : points) {
        result.append({pyMax(integer(0), pyMin(p[0], integer(width - 1))),
                       pyMax(integer(0), pyMin(p[1], integer(height - 1)))});
    }
    return result;
}

// utils/shape.py rectangle_from_diagonal.
Points fromDiagonal(const Points& points)
{
    const Point& a = points[0];
    const Point& b = points[1];
    return {{a[0], a[1]}, {b[0], a[1]}, {b[0], b[1]}, {a[0], b[1]}};
}

// The corners of a rectangle with corners 0 and 2 opposite, as the converters use them. A
// rectangle is its bounding box (format spec 4.2): when the four points are in another order,
// they are put in upstream's order first; otherwise they are used as they are, as upstream does.
Points rectangleCorners(const Points& points)
{
    if (points.size() == 2)
        return fromDiagonal(points);
    if (points.size() != 4)
        return points;
    PyNumber xmin = points[0][0], ymin = points[0][1], xmax = points[0][0], ymax = points[0][1];
    for (const Point& p : points) {
        xmin = pyMin(xmin, p[0]);
        ymin = pyMin(ymin, p[1]);
        xmax = pyMax(xmax, p[0]);
        ymax = pyMax(ymax, p[1]);
    }
    const auto spans = [](double a, double b, double low, double high) {
        return (a == low && b == high) || (a == high && b == low);
    };
    if (spans(points[0][0].value, points[2][0].value, xmin.value, xmax.value)
        && spans(points[0][1].value, points[2][1].value, ymin.value, ymax.value))
        return points;
    return {{xmin, ymin}, {xmax, ymin}, {xmax, ymax}, {xmin, ymax}};
}

bool takesShape(ExportFormat format, const QString& type)
{
    switch (format) {
    case ExportFormat::YoloDetection:
    case ExportFormat::VocRectangle:
    case ExportFormat::CocoDetection:
        return type == QLatin1String("rectangle");
    case ExportFormat::YoloSegmentation:
    case ExportFormat::CocoSegmentation:
        return type == QLatin1String("polygon");
    case ExportFormat::VocPolygon:
        return type == QLatin1String("rectangle") || type == QLatin1String("polygon");
    }
    return false;
}

QString formatText(ExportFormat format)
{
    switch (format) {
    case ExportFormat::YoloDetection:
        return QStringLiteral(u"YOLO 检测");
    case ExportFormat::YoloSegmentation:
        return QStringLiteral(u"YOLO 分割");
    case ExportFormat::VocRectangle:
        return QStringLiteral(u"VOC 矩形");
    case ExportFormat::VocPolygon:
        return QStringLiteral(u"VOC 矩形和多边形");
    case ExportFormat::CocoDetection:
        return QStringLiteral(u"COCO 检测");
    case ExportFormat::CocoSegmentation:
        return QStringLiteral(u"COCO 分割");
    }
    return QString();
}

std::string escapeXml(const QString& text)
{
    // xml.dom.minidom _write_data (Python 3.12).
    std::string out;
    for (const char c : text.toStdString()) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

void element(std::string& out, int depth, const char* name, const std::string& text)
{
    out.append(size_t(depth) * 2, ' ');
    out += '<';
    out += name;
    if (text.empty()) {
        out += "/>\n";
        return;
    }
    out += '>';
    out += text;
    out += "</";
    out += name;
    out += ">\n";
}

void open(std::string& out, int depth, const char* name)
{
    out.append(size_t(depth) * 2, ' ');
    out += '<';
    out += name;
    out += ">\n";
}

void close(std::string& out, int depth, const char* name)
{
    out.append(size_t(depth) * 2, ' ');
    out += "</";
    out += name;
    out += ">\n";
}

} // namespace

QString exportFormatName(ExportFormat format)
{
    switch (format) {
    case ExportFormat::YoloDetection:
        return QStringLiteral("yolo-det");
    case ExportFormat::YoloSegmentation:
        return QStringLiteral("yolo-seg");
    case ExportFormat::VocRectangle:
        return QStringLiteral("voc");
    case ExportFormat::VocPolygon:
        return QStringLiteral("voc-polygon");
    case ExportFormat::CocoDetection:
        return QStringLiteral("coco-det");
    case ExportFormat::CocoSegmentation:
        return QStringLiteral("coco-seg");
    }
    return QString();
}

std::optional<ExportFormat> exportFormatFromName(const QString& name)
{
    for (ExportFormat format : exportFormats()) {
        if (exportFormatName(format) == name)
            return format;
    }
    return std::nullopt;
}

QList<ExportFormat> exportFormats()
{
    return {ExportFormat::YoloDetection, ExportFormat::YoloSegmentation, ExportFormat::VocRectangle,
            ExportFormat::VocPolygon,    ExportFormat::CocoDetection,    ExportFormat::CocoSegmentation};
}

bool exportNeedsClassIds(ExportFormat format)
{
    return format != ExportFormat::VocRectangle && format != ExportFormat::VocPolygon;
}

ExportImage exportImageOf(const QString& path, const QByteArray& bytes, const AnnotationFile& file)
{
    ExportImage image;
    image.path = path;
    image.width = file.image().width;
    image.height = file.image().height;
    const JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok || !parsed.value.is_object())
        return image;
    const Json& json = parsed.value;
    // Format spec 13.3: a size that is not a positive integer reads as missing; the image's own
    // size is used then.
    if (const auto it = json.find("imageWidth"); it != json.end() && it->is_number_integer() && it->get<qint64>() > 0
                                                  && it->get<qint64>() <= std::numeric_limits<int>::max())
        image.width = int(it->get<qint64>());
    if (const auto it = json.find("imageHeight"); it != json.end() && it->is_number_integer() && it->get<qint64>() > 0
                                                   && it->get<qint64>() <= std::numeric_limits<int>::max())
        image.height = int(it->get<qint64>());
    const auto shapesIt = json.find("shapes");
    if (shapesIt == json.end() || !shapesIt->is_array())
        return image;
    const QList<Shape>& modelled = file.document().shapes();
    for (std::size_t i = 0; i < shapesIt->size(); ++i) {
        const Json& object = (*shapesIt)[i];
        ExportShape shape;
        shape.recognized = qsizetype(i) < modelled.size() && modelled[qsizetype(i)].kind != ShapeKind::Unrecognized;
        if (!object.is_object()) {
            shape.recognized = false;
            image.shapes.append(shape);
            continue;
        }
        if (const auto it = object.find("label"); it != object.end() && it->is_string())
            shape.label = QString::fromStdString(it->get_ref<const Json::string_t&>());
        // Upstream needs the key; a missing or null "shape_type" is not exported.
        if (const auto it = object.find("shape_type"); it != object.end() && it->is_string())
            shape.type = QString::fromStdString(it->get_ref<const Json::string_t&>());
        if (const auto it = object.find("points"); it != object.end() && it->is_array()) {
            for (const Json& point : *it) {
                if (point.is_array() && point.size() == 2)
                    shape.points.append({numberOf(point[0]), numberOf(point[1])});
            }
        }
        if (const auto it = object.find("difficult"); it != object.end())
            shape.difficult = int(pyInt(numberOf(*it)));
        if (const auto it = object.find("group_id"); it != object.end() && !it->is_null()) {
            if (it->is_number())
                shape.groupId = pyInt(numberOf(*it));
            else if (it->is_string()) {
                bool ok = false;
                const qint64 value = QString::fromStdString(it->get_ref<const Json::string_t&>()).trimmed().toLongLong(&ok);
                if (ok)
                    shape.groupId = value;
            }
        }
        image.shapes.append(shape);
    }
    return image;
}

QList<ExportIssue> unconvertibleShapes(const ExportImage& image, ExportFormat format,
                                       const QHash<QString, int>& classIds)
{
    QList<ExportIssue> issues;
    for (int i = 0; i < image.shapes.size(); ++i) {
        const ExportShape& shape = image.shapes[i];
        QString reason;
        if (!shape.recognized)
            reason = QStringLiteral(u"无法识别的对象");
        else if (shape.type.isEmpty())
            reason = QStringLiteral(u"缺少 shape_type");
        else if (!takesShape(format, shape.type))
            reason = QStringLiteral(u"%1 格式不能导出 %2").arg(formatText(format), shape.type);
        else if (shape.type == QLatin1String("rectangle") && shape.points.size() != 2 && shape.points.size() != 4)
            reason = QStringLiteral(u"矩形的点数不对");
        else if (shape.type == QLatin1String("polygon") && shape.points.size() < 3)
            reason = QStringLiteral(u"多边形少于 3 个点");
        else if (exportNeedsClassIds(format) && !classIds.contains(shape.label))
            reason = QStringLiteral(u"类别“%1”不在项目的类别列表中").arg(shape.label);
        if (!reason.isEmpty())
            issues.append({i, reason});
    }
    return issues;
}

int clampedShapes(const ExportImage& image, ExportFormat format)
{
    int count = 0;
    for (const ExportShape& shape : image.shapes) {
        if (!takesShape(format, shape.type))
            continue;
        for (const Point& p : shape.points) {
            if (p[0].value < 0 || p[0].value > image.width || p[1].value < 0 || p[1].value > image.height) {
                ++count;
                break;
            }
        }
    }
    return count;
}

QByteArray yoloLabels(const ExportImage& image, ExportFormat format, const QHash<QString, int>& classIds)
{
    std::string out;
    const double width = image.width;
    const double height = image.height;
    for (const ExportShape& shape : image.shapes) {
        if (!takesShape(format, shape.type) || !classIds.contains(shape.label))
            continue;
        Points points = clamped(shape.points, image.width, image.height);
        const int classId = classIds.value(shape.label);
        if (format == ExportFormat::YoloDetection) {
            points = rectangleCorners(points);
            const double xCenter = (points[0][0].value + points[2][0].value) / (2 * width);
            const double yCenter = (points[0][1].value + points[2][1].value) / (2 * height);
            const double w = std::abs(points[2][0].value - points[0][0].value) / width;
            const double h = std::abs(points[2][1].value - points[0][1].value) / height;
            out += std::to_string(classId) + ' ' + fixed6(xCenter) + ' ' + fixed6(yCenter) + ' ' + fixed6(w) + ' '
                   + fixed6(h) + '\n';
        } else {
            if (points.size() < 3)
                continue;
            out += std::to_string(classId) + ' ';
            for (qsizetype i = 0; i < points.size(); ++i) {
                if (i > 0)
                    out += ' ';
                out += pythonFloatRepr(points[i][0].value / width) + ' ' + pythonFloatRepr(points[i][1].value / height);
            }
            out += '\n';
        }
    }
    return QByteArray(out.data(), qsizetype(out.size()));
}

QByteArray vocAnnotation(const ExportImage& image, ExportFormat format, const QString& folder)
{
    std::string out = "<?xml version=\"1.0\" ?>\n<annotation>\n";
    element(out, 1, "folder", escapeXml(folder));
    element(out, 1, "filename", escapeXml(QFileInfo(image.path).fileName()));
    open(out, 1, "size");
    element(out, 2, "width", std::to_string(image.width));
    element(out, 2, "height", std::to_string(image.height));
    // cv2.imread(..., IMREAD_COLOR) always gives three channels.
    element(out, 2, "depth", "3");
    close(out, 1, "size");
    open(out, 1, "source");
    element(out, 2, "database", "https://github.com/CVHub520/X-AnyLabeling");
    close(out, 1, "source");
    for (const ExportShape& shape : image.shapes) {
        Points points = clamped(shape.points, image.width, image.height);
        open(out, 1, "object");
        element(out, 2, "name", escapeXml(shape.label));
        element(out, 2, "pose", "Unspecified");
        element(out, 2, "truncated", "0");
        element(out, 2, "occluded", "0");
        element(out, 2, "difficult", std::to_string(shape.difficult));
        const bool rectangle = shape.type == QLatin1String("rectangle");
        const bool polygon = shape.type == QLatin1String("polygon") && format == ExportFormat::VocPolygon
                             && points.size() >= 3;
        if (rectangle || polygon) {
            if (rectangle && points.size() == 2)
                points = fromDiagonal(points);
            PyNumber xmin = points[0][0], ymin = points[0][1], xmax = points[0][0], ymax = points[0][1];
            for (const Point& p : points) {
                // min() and max() over the values, first one kept on ties.
                xmin = pyMin(xmin, p[0]);
                ymin = pyMin(ymin, p[1]);
                xmax = pyMax(xmax, p[0]);
                ymax = pyMax(ymax, p[1]);
            }
            open(out, 2, "bndbox");
            element(out, 3, "xmin", std::to_string(pyInt(xmin)));
            element(out, 3, "ymin", std::to_string(pyInt(ymin)));
            element(out, 3, "xmax", std::to_string(pyInt(xmax)));
            element(out, 3, "ymax", std::to_string(pyInt(ymax)));
            close(out, 2, "bndbox");
            if (polygon) {
                open(out, 2, "polygon");
                for (qsizetype i = 0; i < points.size(); ++i) {
                    const std::string n = std::to_string(i + 1);
                    element(out, 3, ("x" + n).c_str(), pyStr(points[i][0]));
                    element(out, 3, ("y" + n).c_str(), pyStr(points[i][1]));
                }
                close(out, 2, "polygon");
            }
        }
        close(out, 1, "object");
    }
    out += "</annotation>\n";
    return QByteArray(out.data(), qsizetype(out.size()));
}

struct CocoWriter::Data {
    ExportFormat format;
    QList<std::pair<int, QString>> categories;
    CocoInfo info;
    QIODevice* out = nullptr;
    QIODevice* annotations = nullptr;
    qint64 imageId = 0;
    qint64 annotationId = 0;
    bool ok = true;

    void write(QIODevice* device, const std::string& text)
    {
        if (ok && device->write(text.data(), qint64(text.size())) != qint64(text.size()))
            ok = false;
    }

    // One element of a top-level array, at json.dump(indent=4)'s place.
    void element(QIODevice* device, qint64 index, const Json& value)
    {
        const QByteArray bytes = serializeJson(value, 4, 2);
        write(device, std::string(index == 0 ? "[\n        " : ",\n        ") + bytes.toStdString());
    }
};

CocoWriter::CocoWriter(ExportFormat format, QList<std::pair<int, QString>> categories, CocoInfo info)
    : d(std::make_unique<Data>())
{
    d->format = format;
    d->categories = std::move(categories);
    d->info = std::move(info);
}

CocoWriter::~CocoWriter() = default;

QString CocoWriter::fileName(ExportFormat format)
{
    return format == ExportFormat::CocoSegmentation ? QStringLiteral("coco_instance_segmentation.json")
                                                    : QStringLiteral("coco_detection.json");
}

bool CocoWriter::begin(QIODevice* out, QIODevice* annotations)
{
    d->out = out;
    d->annotations = annotations;
    const bool segmentation = d->format == ExportFormat::CocoSegmentation;
    Json info = Json::object();
    info["year"] = d->info.year;
    info["version"] = d->info.version.toStdString();
    info["description"] = "COCO Label Conversion";
    info["contributor"] = "aoi-LiteAnno";
    info["url"] = "";
    info["date_created"] = d->info.dateCreated.toStdString();
    Json licenses = Json::array();
    licenses.push_back(Json{{"id", 1}, {"url", ""}, {"name", ""}});
    // Upstream numbers its classes from 1 and adds "_background_" as 0 for segmentation; the
    // project's ids are used here as they are, so 0 can be a class and there is no background.
    Json categories = Json::array();
    for (const auto& [id, name] : std::as_const(d->categories)) {
        Json category = Json::object();
        category["id"] = id;  // classes[].id as they are (architecture 11), not upstream's index + 1
        category["name"] = name.toStdString();
        category["supercategory"] = segmentation ? Json(nullptr) : Json("");
        categories.push_back(std::move(category));
    }
    d->write(out, "{\n    \"info\": " + serializeJson(info, 4, 1).toStdString() + ",\n    \"licenses\": "
                      + serializeJson(licenses, 4, 1).toStdString() + ",\n    \"categories\": "
                      + serializeJson(categories, 4, 1).toStdString() + ",\n    \"images\": ");
    return d->ok;
}

bool CocoWriter::add(const ExportImage& image, const QString& fileName, const QHash<QString, int>& classIds)
{
    Json entry = Json::object();
    entry["license"] = 0;
    entry["url"] = nullptr;
    entry["file_name"] = fileName.toStdString();
    entry["height"] = image.height;
    entry["width"] = image.width;
    entry["date_captured"] = nullptr;
    entry["id"] = d->imageId;
    d->element(d->out, d->imageId, entry);

    if (d->format == ExportFormat::CocoDetection) {
        for (const ExportShape& shape : image.shapes) {
            if (shape.type != QLatin1String("rectangle") || !classIds.contains(shape.label))
                continue;
            const Points points = rectangleCorners(clamped(shape.points, image.width, image.height));
            const PyNumber xMin = pyMin(points[0][0], points[2][0]);
            const PyNumber yMin = pyMin(points[0][1], points[2][1]);
            const PyNumber xMax = pyMax(points[0][0], points[2][0]);
            const PyNumber yMax = pyMax(points[0][1], points[2][1]);
            const PyNumber width = pySub(xMax, xMin);
            const PyNumber height = pySub(yMax, yMin);
            Json annotation = Json::object();
            annotation["id"] = d->annotationId;
            annotation["image_id"] = d->imageId;
            annotation["category_id"] = classIds.value(shape.label);
            annotation["bbox"] = Json::array({pyJson(xMin), pyJson(yMin), pyJson(width), pyJson(height)});
            annotation["area"] = pyJson(pyMul(width, height));
            annotation["iscrowd"] = 0;
            annotation["ignore"] = shape.difficult;
            annotation["segmentation"] = Json::array();
            d->element(d->annotations, d->annotationId, annotation);
            ++d->annotationId;
        }
    } else {
        // Polygons of one label and group_id are one instance; without a group_id each is
        // its own.
        struct Instance {
            QString label;
            int difficult = 0;
            QList<Points> segmentation;
        };
        QList<Instance> instances;
        QHash<QPair<QString, qint64>, qsizetype> grouped;
        for (const ExportShape& shape : image.shapes) {
            if (shape.type != QLatin1String("polygon") || !classIds.contains(shape.label))
                continue;
            qsizetype index = -1;
            if (shape.groupId)
                index = grouped.value({shape.label, *shape.groupId}, -1);
            if (index < 0) {
                index = instances.size();
                instances.append({shape.label, shape.difficult, {}});
                if (shape.groupId)
                    grouped.insert({shape.label, *shape.groupId}, index);
            }
            instances[index].segmentation.append(clamped(shape.points, image.width, image.height));
        }
        for (const Instance& instance : std::as_const(instances)) {
            Json segmentation = Json::array();
            std::vector<RasterPolygon> polygons;
            double xMin = 0, yMin = 0, xMax = 0, yMax = 0;
            bool first = true;
            for (const Points& points : instance.segmentation) {
                Json flat = Json::array();
                RasterPolygon polygon;
                for (const Point& p : points) {
                    flat.push_back(pyJson(p[0]));
                    flat.push_back(pyJson(p[1]));
                    polygon.emplace_back(p[0].value, p[1].value);
                    if (first) {
                        xMin = xMax = p[0].value;
                        yMin = yMax = p[1].value;
                        first = false;
                    }
                    xMin = std::min(xMin, p[0].value);
                    xMax = std::max(xMax, p[0].value);
                    yMin = std::min(yMin, p[1].value);
                    yMax = std::max(yMax, p[1].value);
                }
                segmentation.push_back(std::move(flat));
                polygons.push_back(std::move(polygon));
            }
            // get_min_enclosing_bbox: the integer pixels the points fall in.
            const double x0 = std::floor(xMin), y0 = std::floor(yMin);
            Json annotation = Json::object();
            annotation["id"] = d->annotationId;
            annotation["image_id"] = d->imageId;
            annotation["category_id"] = classIds.value(instance.label);
            annotation["segmentation"] = std::move(segmentation);
            annotation["area"] = rasterizedPolygonArea(polygons);
            annotation["bbox"] = Json::array({x0, y0, std::floor(xMax) - x0 + 1, std::floor(yMax) - y0 + 1});
            annotation["iscrowd"] = 0;
            annotation["ignore"] = instance.difficult;
            d->element(d->annotations, d->annotationId, annotation);
            ++d->annotationId;
        }
    }
    ++d->imageId;
    return d->ok;
}

bool CocoWriter::finish()
{
    d->write(d->out, d->imageId == 0 ? "[]" : "\n    ]");
    d->write(d->out, ",\n    \"annotations\": ");
    if (d->annotationId == 0) {
        d->write(d->out, "[]");
    } else {
        // The annotations, copied over in pieces.
        if (!d->annotations->seek(0))
            d->ok = false;
        while (d->ok && !d->annotations->atEnd()) {
            const QByteArray chunk = d->annotations->read(1 << 20);
            if (chunk.isEmpty())
                break;
            d->write(d->out, chunk.toStdString());
        }
        d->write(d->out, "\n    ]");
    }
    if (d->format == ExportFormat::CocoSegmentation)
        d->write(d->out, ",\n    \"type\": \"instances\"");
    d->write(d->out, "\n}");
    return d->ok;
}

QByteArray exportManifestToBytes(const ExportManifest& manifest)
{
    Json json = Json::object();
    json["formatVersion"] = kFormatVersion;
    json["projectId"] = manifest.projectId.toStdString();
    json["exportedAt"] = manifest.exportedAt.toStdString();
    json["appVersion"] = manifest.appVersion.toStdString();
    Json scope = Json::object();
    scope["kind"] = manifest.wholeProject ? "project" : "batches";
    Json batches = Json::array();
    for (const auto& [number, batchId] : manifest.batches)
        batches.push_back(Json{{"number", number}, {"batchId", batchId.toStdString()}});
    scope["batches"] = std::move(batches);
    scope["scanComplete"] = manifest.scanComplete;
    json["scope"] = std::move(scope);
    json["format"] = exportFormatName(manifest.format).toStdString();
    json["options"] = Json{{"skipUnconvertible", manifest.skipUnconvertible}, {"copyImages", manifest.copyImages}};
    Json categories = Json::array();
    for (const auto& [id, name] : manifest.categories)
        categories.push_back(Json{{"id", id}, {"name", name.toStdString()}});
    json["categories"] = std::move(categories);
    Json counts = Json::object();
    for (const auto& [name, count] : manifest.counts)
        counts[name.toStdString()] = count;
    json["counts"] = std::move(counts);
    Json items = Json::array();
    for (const ExportManifest::Item& item : manifest.items) {
        Json entry = Json::object();
        entry["image"] = item.image.toStdString();
        entry["result"] = item.result.toStdString();
        if (!item.status.isEmpty())
            entry["status"] = item.status.toStdString();
        if (!item.reason.isEmpty())
            entry["reason"] = item.reason.toStdString();
        if (item.clampedObjects > 0)
            entry["clampedObjects"] = item.clampedObjects;
        items.push_back(std::move(entry));
    }
    json["items"] = std::move(items);
    return serializeJson(json);
}

} // namespace liteanno::formats
