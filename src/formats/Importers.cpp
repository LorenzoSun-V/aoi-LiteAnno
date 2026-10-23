#include "formats/Importers.h"

#include "core/ShapeType.h"
#include "formats/JsonText.h"
#include "formats/MaskContours.h"

#include <QXmlStreamReader>

#include <QHash>
#include <QSet>

#include <algorithm>
#include <charconv>
#include <limits>
#include <cmath>
#include <map>
#include <string>

namespace liteanno::formats {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct FormatName {
    ImportFormat format;
    const char* name;
};

constexpr FormatName kFormatNames[] = {
    {ImportFormat::YoloDetection, "yolo-det"},
    {ImportFormat::YoloSegmentation, "yolo-seg"},
    {ImportFormat::YoloObb, "yolo-obb"},
    {ImportFormat::VocRectangle, "voc"},
    {ImportFormat::VocPolygon, "voc-polygon"},
    {ImportFormat::Dota, "dota"},
    {ImportFormat::CocoDetection, "coco-det"},
    {ImportFormat::CocoSegmentation, "coco-seg"},
    {ImportFormat::Mask, "mask"},
    {ImportFormat::YoloPose, "yolo-pose"},
    {ImportFormat::CocoPose, "coco-pose"},
};

// Python's float(): correctly rounded, an optional sign, surrounding spaces allowed. Infinity
// and NaN are refused, as JSON cannot hold them (format spec 13.5).
std::optional<double> parseFloat(const QString& text)
{
    std::string s = text.trimmed().toStdString();
    if (!s.empty() && s.front() == '+')
        s.erase(0, 1);
    double value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, std::chars_format::general);
    if (error != std::errc() || end != s.data() + s.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// Python's int() for a non-negative decimal number.
std::optional<int> parseIndex(const QString& text)
{
    std::string s = text.trimmed().toStdString();
    if (!s.empty() && s.front() == '+')
        s.erase(0, 1);
    int value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || error != std::errc() || end != s.data() + s.size() || value < 0)
        return std::nullopt;
    return value;
}

QStringList textLines(const QByteArray& bytes)
{
    QString text = QString::fromUtf8(bytes);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text.split(QLatin1Char('\n'));
}

QStringList fields(const QString& line)
{
    return line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

QString lineError(int line, const QString& message)
{
    return QStringLiteral(u"第 %1 行：%2").arg(line).arg(message);
}

Shape newShape(ShapeType type, const QString& label, PointList points)
{
    Shape shape;
    shape.type = type;
    shape.label = label;
    shape.points = std::move(points);
    return shape;
}

// "difficult" is not edited here; a new shape takes it from the object it was copied from.
void setUnedited(Shape& shape, bool difficult, std::optional<qint64> groupId = std::nullopt)
{
    shape.groupId = groupId;
    Json object = Json::object();
    if (difficult)
        object["difficult"] = true;
    if (!object.empty())
        shape.copiedObject = serializeJson(object);
}

void markDifficult(Shape& shape, bool difficult)
{
    setUnedited(shape, difficult);
}

// Python's math.degrees.
double degrees(double radians)
{
    return radians / (kPi / 180.0);
}

} // namespace

QString importFormatName(ImportFormat format)
{
    for (const FormatName& entry : kFormatNames) {
        if (entry.format == format)
            return QString::fromLatin1(entry.name);
    }
    return {};
}

std::optional<ImportFormat> importFormatFromName(const QString& name)
{
    for (const FormatName& entry : kFormatNames) {
        if (name == QLatin1String(entry.name))
            return entry.format;
    }
    return std::nullopt;
}

QList<ImportFormat> importFormats()
{
    QList<ImportFormat> formats;
    for (const FormatName& entry : kFormatNames)
        formats.append(entry.format);
    return formats;
}

QString importLabelSuffix(ImportFormat format)
{
    switch (format) {
    case ImportFormat::VocRectangle:
    case ImportFormat::VocPolygon:
        return QStringLiteral(".xml");
    case ImportFormat::Mask:
        return QStringLiteral(".png");
    case ImportFormat::CocoDetection:
    case ImportFormat::CocoSegmentation:
    case ImportFormat::CocoPose:
        return {};
    default:
        return QStringLiteral(".txt");
    }
}

bool importReadsOneFile(ImportFormat format)
{
    return format == ImportFormat::CocoDetection || format == ImportFormat::CocoSegmentation
           || format == ImportFormat::CocoPose;
}

bool importTakesPoseConfig(ImportFormat format)
{
    return format == ImportFormat::YoloPose || format == ImportFormat::CocoPose;
}

bool importNeedsClassNames(ImportFormat format)
{
    return format == ImportFormat::YoloDetection || format == ImportFormat::YoloSegmentation
           || format == ImportFormat::YoloObb;
}

double rotationTheta(const PointList& points)
{
    const double angle = std::atan2(points[1].y() - points[0].y(), points[1].x() - points[0].x());
    double angleDegrees = degrees(angle);
    if (angleDegrees < 0)
        angleDegrees += 360;
    return angleDegrees / 360 * (2 * kPi);
}

ImportedLabels parseYoloLabels(const QByteArray& bytes, ImportFormat format, const ClassNames& classes, int width,
                               int height)
{
    ImportedLabels result;
    const QStringList lines = textLines(bytes);
    for (int i = 0; i < lines.size(); ++i) {
        const QStringList parts = fields(lines[i]);
        if (parts.isEmpty())
            continue;
        const int lineNumber = i + 1;
        const std::optional<int> classIndex = parseIndex(parts[0]);
        if (!classIndex) {
            result.error = lineError(lineNumber, QStringLiteral(u"类别编号“%1”不是非负整数").arg(parts[0]));
            return result;
        }
        const QString label = classes.value(*classIndex);
        if (label.isEmpty()) {
            result.error = lineError(lineNumber, QStringLiteral(u"没有编号为 %1 的类别").arg(*classIndex));
            return result;
        }
        QList<double> values;
        for (qsizetype k = 1; k < parts.size(); ++k) {
            const std::optional<double> value = parseFloat(parts[k]);
            if (!value) {
                result.error = lineError(lineNumber, QStringLiteral(u"“%1”不是数字").arg(parts[k]));
                return result;
            }
            values.append(*value);
        }
        if (format == ImportFormat::YoloDetection) {
            if (values.size() != 4) {
                result.error = lineError(lineNumber, QStringLiteral(u"检测框应有 4 个数，这里有 %1 个").arg(values.size()));
                return result;
            }
            const double cx = values[0], cy = values[1], nw = values[2], nh = values[3];
            const double xmin = (cx - nw / 2) * width;
            const double ymin = (cy - nh / 2) * height;
            const double xmax = (cx + nw / 2) * width;
            const double ymax = (cy + nh / 2) * height;
            result.shapes.append(newShape(ShapeType::Rectangle, label,
                                          {{xmin, ymin}, {xmax, ymin}, {xmax, ymax}, {xmin, ymax}}));
        } else if (format == ImportFormat::YoloSegmentation) {
            if (values.size() % 2 != 0 || values.size() < 6) {
                result.error = lineError(lineNumber, QStringLiteral(u"分割应有至少 3 个点的成对坐标，这里有 %1 个数")
                                                         .arg(values.size()));
                return result;
            }
            PointList points;
            for (qsizetype k = 0; k < values.size(); k += 2)
                points.append({values[k] * width, values[k + 1] * height});
            result.shapes.append(newShape(ShapeType::Polygon, label, std::move(points)));
        } else {
            if (values.size() != 8) {
                result.error = lineError(lineNumber, QStringLiteral(u"旋转框应有 8 个数，这里有 %1 个").arg(values.size()));
                return result;
            }
            PointList points;
            for (qsizetype k = 0; k < 8; k += 2)
                points.append({values[k] * width, values[k + 1] * height});
            Shape shape = newShape(ShapeType::Rotation, label, std::move(points));
            shape.direction = rotationTheta(shape.points);
            result.shapes.append(std::move(shape));
        }
    }
    return result;
}

ImportedLabels parseYoloPoseLabels(const QByteArray& bytes, const PoseConfig& pose, int width, int height)
{
    ImportedLabels result;
    const QStringList lines = textLines(bytes);
    const int interval = pose.hasVisible ? 3 : 2;
    for (int i = 0; i < lines.size(); ++i) {
        const QStringList parts = fields(lines[i]);
        if (parts.isEmpty())
            continue;
        const int lineNumber = i + 1;
        const std::optional<int> classIndex = parseIndex(parts[0]);
        if (!classIndex) {
            result.error = lineError(lineNumber, QStringLiteral(u"类别编号“%1”不是非负整数").arg(parts[0]));
            return result;
        }
        if (*classIndex >= pose.classes.size()) {
            result.error = lineError(lineNumber, QStringLiteral(u"姿态配置文件里没有编号为 %1 的类别").arg(*classIndex));
            return result;
        }
        if (parts.size() < 5 || (parts.size() - 5) % interval != 0) {
            result.error = lineError(lineNumber, pose.hasVisible
                                                     ? QStringLiteral(u"应是类别、4 个框的数，再加每个关键点的 x y 可见性")
                                                     : QStringLiteral(u"应是类别、4 个框的数，再加每个关键点的 x y"));
            return result;
        }
        QList<double> values;
        for (qsizetype k = 1; k < parts.size(); ++k) {
            // The visibility is read with int(): a whole number.
            const bool flag = pose.hasVisible && k >= 5 && (k - 5) % 3 == 2;
            const std::optional<double> value = flag ? std::optional<double>(parseIndex(parts[k]).value_or(-1)) : parseFloat(parts[k]);
            if (!value || (flag && (*value < 0 || *value > 2))) {
                result.error = lineError(lineNumber, flag ? QStringLiteral(u"可见性“%1”应是 0、1 或 2").arg(parts[k])
                                                          : QStringLiteral(u"“%1”不是数字").arg(parts[k]));
                return result;
            }
            values.append(*value);
        }
        const auto& [label, names] = pose.classes[*classIndex];
        const qint64 groupId = i;
        // int(): toward zero.
        const double xmin = std::trunc((values[0] - values[2] / 2) * width);
        const double ymin = std::trunc((values[1] - values[3] / 2) * height);
        const double xmax = std::trunc((values[0] + values[2] / 2) * width);
        const double ymax = std::trunc((values[1] + values[3] / 2) * height);
        Shape box = newShape(ShapeType::Rectangle, label, {{xmin, ymin}, {xmax, ymin}, {xmax, ymax}, {xmin, ymax}});
        setUnedited(box, false, groupId);
        result.shapes.append(std::move(box));
        for (qsizetype k = 4, n = 0; k < values.size(); k += interval, ++n) {
            const double x = values[k] * width;
            const double y = values[k + 1] * height;
            const int flag = pose.hasVisible ? int(values[k + 2]) : 0;
            // With flags only flag 0 is a missing keypoint; upstream also drops a visible one at
            // (0, 0), which our own export writes as "0 0 2" (a deliberate difference).
            if (pose.hasVisible ? flag == 0 : (x == 0 && y == 0))
                continue;
            if (n >= names.size()) {
                // Upstream pads lines to the most keypoints a class has, with zeros only.
                result.error = lineError(lineNumber, QStringLiteral(u"类别“%1”只有 %2 个关键点，这里有更多").arg(label).arg(names.size()));
                return result;
            }
            Shape point = newShape(ShapeType::Point, names[n], {{x, y}});
            setUnedited(point, flag == 1, groupId);
            result.shapes.append(std::move(point));
        }
    }
    return result;
}

ImportedLabels parseDotaLabels(const QByteArray& bytes)
{
    ImportedLabels result;
    const QStringList lines = textLines(bytes);
    for (int i = 0; i < lines.size(); ++i) {
        const QStringList parts = fields(lines[i]);
        if (parts.isEmpty())
            continue;
        const int lineNumber = i + 1;
        if (parts.size() <= 2
            && (parts[0].startsWith(QLatin1String("imagesource:")) || parts[0].startsWith(QLatin1String("gsd:")))) {
            continue;
        }
        if (parts.size() != 9 && parts.size() != 10) {
            result.error = lineError(lineNumber, QStringLiteral(u"应为 8 个坐标、类别和可选的 difficult，这里有 %1 项")
                                                     .arg(parts.size()));
            return result;
        }
        PointList points;
        for (int k = 0; k < 8; k += 2) {
            const std::optional<double> x = parseFloat(parts[k]);
            const std::optional<double> y = parseFloat(parts[k + 1]);
            if (!x || !y) {
                result.error = lineError(lineNumber, QStringLiteral(u"“%1 %2”不是坐标").arg(parts[k], parts[k + 1]));
                return result;
            }
            points.append({*x, *y});
        }
        bool difficult = false;
        if (parts.size() == 10) {
            const std::optional<int> value = parseIndex(parts[9]);
            if (!value) {
                result.error = lineError(lineNumber, QStringLiteral(u"difficult“%1”不是 0 或正整数").arg(parts[9]));
                return result;
            }
            difficult = *value != 0;
        }
        Shape shape = newShape(ShapeType::Rotation, parts[8], std::move(points));
        shape.direction = rotationTheta(shape.points);
        markDifficult(shape, difficult);
        result.shapes.append(std::move(shape));
    }
    return result;
}

namespace {

// One element of the XML with the text of its leaf children, which is all VOC needs.
struct XmlNode {
    QString name;
    QString text;
    QList<XmlNode> children;

    const XmlNode* child(const QString& childName) const
    {
        for (const XmlNode& node : children) {
            if (node.name == childName)
                return &node;
        }
        return nullptr;
    }
    // ElementTree's find("a/b").
    const XmlNode* find(const QString& path) const
    {
        const XmlNode* node = this;
        for (const QString& part : path.split(QLatin1Char('/'))) {
            node = node->child(part);
            if (!node)
                return nullptr;
        }
        return node;
    }
};

bool readElement(QXmlStreamReader& reader, XmlNode& node)
{
    node.name = reader.name().toString();
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            XmlNode child;
            if (!readElement(reader, child))
                return false;
            node.children.append(std::move(child));
        } else if (reader.isCharacters()) {
            node.text += reader.text();
        } else if (reader.isEndElement()) {
            return true;
        }
    }
    return false;
}

std::optional<double> coordinate(const XmlNode& object, const QString& path, QString& error)
{
    const XmlNode* node = object.find(path);
    if (!node) {
        error = QStringLiteral(u"缺少 <%1>").arg(path);
        return std::nullopt;
    }
    const std::optional<double> value = parseFloat(node->text);
    if (!value)
        error = QStringLiteral(u"<%1> 的“%2”不是数字").arg(path, node->text.trimmed());
    return value;
}

} // namespace

ImportedLabels parseVocAnnotation(const QByteArray& bytes, ImportFormat format, int width, int height)
{
    ImportedLabels result;
    QXmlStreamReader reader(bytes);
    XmlNode root;
    while (!reader.atEnd() && !reader.isStartElement())
        reader.readNext();
    if (!reader.isStartElement() || !readElement(reader, root) || reader.hasError()) {
        result.error = QStringLiteral(u"不是有效的 XML：%1").arg(reader.errorString());
        return result;
    }
    const XmlNode* sizeWidth = root.find(QStringLiteral("size/width"));
    const XmlNode* sizeHeight = root.find(QStringLiteral("size/height"));
    if (!sizeWidth || !sizeHeight) {
        result.error = QStringLiteral(u"缺少 <size>");
        return result;
    }
    const std::optional<int> xmlWidth = parseIndex(sizeWidth->text);
    const std::optional<int> xmlHeight = parseIndex(sizeHeight->text);
    if (!xmlWidth || !xmlHeight || *xmlWidth != width || *xmlHeight != height) {
        result.error = QStringLiteral(u"XML 里的尺寸 %1×%2 和图片的 %3×%4 不一致")
                           .arg(sizeWidth->text.trimmed(), sizeHeight->text.trimmed())
                           .arg(width)
                           .arg(height);
        return result;
    }
    int objectIndex = 0;
    for (const XmlNode& object : root.children) {
        if (object.name != QLatin1String("object"))
            continue;
        ++objectIndex;
        const auto fail = [&result, objectIndex](const QString& message) {
            result.error = QStringLiteral(u"第 %1 个 <object>：%2").arg(objectIndex).arg(message);
        };
        const XmlNode* name = object.child(QStringLiteral("name"));
        if (!name || name->text.trimmed().isEmpty()) {
            fail(QStringLiteral(u"缺少类别 <name>"));
            return result;
        }
        bool difficult = false;
        if (const XmlNode* node = object.child(QStringLiteral("difficult"))) {
            const std::optional<int> value = parseIndex(node->text);
            if (!value) {
                fail(QStringLiteral(u"<difficult> 的“%1”不是 0 或正整数").arg(node->text.trimmed()));
                return result;
            }
            difficult = *value != 0;
        }
        QString error;
        Shape shape;
        const XmlNode* polygon = object.child(QStringLiteral("polygon"));
        if (polygon && format == ImportFormat::VocPolygon) {
            const qsizetype count = polygon->children.size() / 2;
            PointList points;
            for (qsizetype k = 1; k <= count; ++k) {
                const std::optional<double> x = coordinate(object, QStringLiteral("polygon/x%1").arg(k), error);
                const std::optional<double> y = x ? coordinate(object, QStringLiteral("polygon/y%1").arg(k), error)
                                                  : std::nullopt;
                if (!x || !y) {
                    fail(error);
                    return result;
                }
                points.append({*x, *y});
            }
            if (points.size() < 3) {
                fail(QStringLiteral(u"多边形少于 3 个点"));
                return result;
            }
            shape = newShape(ShapeType::Polygon, name->text, std::move(points));
        } else if (object.child(QStringLiteral("bndbox"))) {
            std::optional<double> box[4];
            const char* const tags[] = {"bndbox/xmin", "bndbox/ymin", "bndbox/xmax", "bndbox/ymax"};
            for (int k = 0; k < 4; ++k) {
                box[k] = coordinate(object, QLatin1String(tags[k]), error);
                if (!box[k]) {
                    fail(error);
                    return result;
                }
            }
            const double xmin = *box[0], ymin = *box[1], xmax = *box[2], ymax = *box[3];
            shape = newShape(ShapeType::Rectangle, name->text,
                             {{xmin, ymin}, {xmax, ymin}, {xmax, ymax}, {xmin, ymax}});
        } else {
            fail(polygon ? QStringLiteral(u"只有多边形，没有 <bndbox>；要导入多边形请选 voc-polygon")
                         : QStringLiteral(u"没有 <bndbox>"));
            return result;
        }
        markDifficult(shape, difficult);
        result.shapes.append(std::move(shape));
    }
    return result;
}

namespace {

QString utf8(const std::string& text)
{
    return QString::fromStdString(text);
}

// A dict key in Python: 1 and 1.0 are the same key.
QString keyOf(const Json& value)
{
    if (value.is_number())
        return QString::number(value.get<double>(), 'g', 17);
    return utf8(value.dump());
}

std::optional<int> integerValue(const Json& value)
{
    if (value.is_number_integer() || value.is_number_unsigned()) {
        const auto number = value.get<qint64>();
        if (number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max())
            return int(number);
    }
    return std::nullopt;
}

// bool(int(str(ignore))): an integer, or a string holding one.
std::optional<bool> ignoreFlag(const Json& value)
{
    if (value.is_number_integer() || value.is_number_unsigned())
        return value.get<qint64>() != 0;
    if (value.is_string()) {
        const std::optional<int> number = parseIndex(utf8(value.get<std::string>()));
        if (number)
            return *number != 0;
        // A negative number is still an int to Python.
        QString text = utf8(value.get<std::string>()).trimmed();
        if (text.startsWith(QLatin1Char('-'))) {
            const std::optional<int> negative = parseIndex(text.mid(1));
            if (negative)
                return *negative != 0;
        }
    }
    return std::nullopt;
}

std::optional<double> numberValue(const Json& value)
{
    if (value.is_number())
        return value.get<double>();
    return std::nullopt;
}

} // namespace

CocoLabels parseCocoLabels(const QByteArray& bytes, ImportFormat format, const PoseConfig* pose)
{
    CocoLabels result;
    const JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok) {
        result.error = QStringLiteral(u"不是有效的 JSON：%1").arg(parsed.error);
        return result;
    }
    const Json& root = parsed.value;
    const auto array = [&root](const char* key) -> const Json* {
        if (!root.is_object())
            return nullptr;
        const auto it = root.find(key);
        return it != root.end() && it->is_array() ? &*it : nullptr;
    };
    const Json* images = array("images");
    const Json* annotations = array("annotations");
    const Json* categories = array("categories");
    if (!images || !annotations || !categories) {
        result.error = QStringLiteral(u"不是 COCO 标注文件：缺少 images、annotations 或 categories 数组");
        return result;
    }

    QHash<QString, QString> categoryNames;
    QHash<QString, QStringList> keypointNames;  // CocoPose, by category name
    for (const Json& category : *categories) {
        if (!category.is_object() || !category.contains("id") || !category.contains("name") || !category["name"].is_string()) {
            result.error = QStringLiteral(u"categories 里有缺少 id 或 name 的类别");
            return result;
        }
        const QString name = utf8(category["name"].get<std::string>());
        categoryNames.insert(keyOf(category["id"]), name);
        if (format != ImportFormat::CocoPose)
            continue;
        if (pose) {
            if (const QStringList* names = pose->keypoints(name))
                keypointNames.insert(name, *names);
            continue;
        }
        const auto keypoints = category.find("keypoints");
        bool ok = keypoints != category.end() && keypoints->is_array() && !keypoints->empty();
        QStringList names;
        for (size_t k = 0; ok && k < keypoints->size(); ++k) {
            ok = (*keypoints)[k].is_string();
            if (ok)
                names.append(utf8((*keypoints)[k].get<std::string>()));
        }
        if (!ok || QSet<QString>(names.cbegin(), names.cend()).size() != names.size()) {
            result.error = QStringLiteral(u"类别“%1”的 keypoints 应是不重复的关键点名称；也可以选择姿态配置文件").arg(name);
            return result;
        }
        keypointNames.insert(name, names);
    }

    QHash<QString, qsizetype> imageIndex;
    for (const Json& image : *images) {
        if (!image.is_object() || !image.contains("id") || !image.contains("file_name")
            || !image["file_name"].is_string()) {
            result.error = QStringLiteral(u"images 里有缺少 id 或 file_name 的图片");
            return result;
        }
        const QString key = keyOf(image["id"]);
        CocoImageLabels entry;
        entry.fileName = utf8(image["file_name"].get<std::string>());
        if (imageIndex.contains(key)) {
            result.error = QStringLiteral(u"图片 id %1 出现了两次（%2）").arg(utf8(image["id"].dump()), entry.fileName);
            return result;
        }
        const std::optional<int> width = image.contains("width") ? integerValue(image["width"]) : std::nullopt;
        const std::optional<int> height = image.contains("height") ? integerValue(image["height"]) : std::nullopt;
        if (!width || !height || *width <= 0 || *height <= 0) {
            result.error = QStringLiteral(u"图片 %1 缺少有效的 width、height").arg(entry.fileName);
            return result;
        }
        entry.width = *width;
        entry.height = *height;
        imageIndex.insert(key, result.images.size());
        result.images.append(std::move(entry));
    }

    // Per image, the number of annotations made of several polygons so far (upstream's
    // group ids, from 1).
    QHash<qsizetype, qint64> groups;
    QHash<qsizetype, qint64> poseGroups;  // per image, from 0 (upstream's image_ids counter)
    int annotationNumber = 0;
    for (const Json& annotation : *annotations) {
        ++annotationNumber;
        if (!annotation.is_object() || !annotation.contains("image_id")) {
            result.error = QStringLiteral(u"第 %1 个标注没有 image_id").arg(annotationNumber);
            return result;
        }
        const auto imageIt = imageIndex.constFind(keyOf(annotation["image_id"]));
        if (imageIt == imageIndex.cend()) {
            result.error = QStringLiteral(u"第 %1 个标注的 image_id %2 不在 images 里")
                               .arg(annotationNumber)
                               .arg(utf8(annotation["image_id"].dump()));
            return result;
        }
        const qsizetype index = *imageIt;
        ImportedLabels& labels = result.images[index].labels;
        if (!labels.error.isEmpty())
            continue;
        const auto fail = [&labels, annotationNumber](const QString& message) {
            labels.error = QStringLiteral(u"第 %1 个标注：%2").arg(annotationNumber).arg(message);
        };
        const auto categoryIt =
            annotation.contains("category_id") ? categoryNames.constFind(keyOf(annotation["category_id"])) : categoryNames.cend();
        if (categoryIt == categoryNames.cend()) {
            fail(QStringLiteral(u"category_id 不在 categories 里"));
            continue;
        }
        const QString& label = *categoryIt;
        bool difficult = false;
        if (annotation.contains("ignore")) {
            const std::optional<bool> flag = ignoreFlag(annotation["ignore"]);
            if (!flag) {
                fail(QStringLiteral(u"ignore 的值 %1 不是整数").arg(utf8(annotation["ignore"].dump())));
                continue;
            }
            difficult = *flag;
        }
        if (format == ImportFormat::CocoDetection || format == ImportFormat::CocoPose) {
            const auto bbox = annotation.find("bbox");
            std::optional<double> values[4];
            bool ok = bbox != annotation.end() && bbox->is_array() && bbox->size() == 4;
            for (size_t k = 0; ok && k < 4; ++k) {
                values[k] = numberValue((*bbox)[k]);
                ok = values[k].has_value();
            }
            if (!ok) {
                fail(QStringLiteral(u"bbox 应是 4 个数"));
                continue;
            }
            const double xmin = *values[0], ymin = *values[1];
            const double xmax = xmin + *values[2], ymax = ymin + *values[3];
            Shape shape = newShape(ShapeType::Rectangle, label, {{xmin, ymin}, {xmax, ymin}, {xmax, ymax}, {xmin, ymax}});
            if (format == ImportFormat::CocoDetection) {
                setUnedited(shape, difficult);
                labels.shapes.append(std::move(shape));
                continue;
            }
            // Pose: the annotation's number in its image, from 0, joins the box and its points.
            const auto names = keypointNames.constFind(label);
            if (names == keypointNames.cend()) {
                fail(QStringLiteral(u"姿态配置文件里没有类别“%1”").arg(label));
                continue;
            }
            const auto keypoints = annotation.find("keypoints");
            const size_t count = keypoints != annotation.end() && keypoints->is_array() ? keypoints->size() : 0;
            const size_t n = size_t(names->size());
            if (count != 3 * n && count != 2 * n) {
                fail(QStringLiteral(u"keypoints 应是 %1 个关键点的 x y 可见性（或 x y）").arg(n));
                continue;
            }
            const size_t interval = count == 3 * n ? 3 : 2;
            QList<Shape> points;
            QList<bool> pointFlags;  // this annotation's own, also when it fails half way
            for (size_t k = 0; k < count; k += interval) {
                const std::optional<double> x = numberValue((*keypoints)[k]);
                const std::optional<double> y = numberValue((*keypoints)[k + 1]);
                const std::optional<double> flag = interval == 3 ? numberValue((*keypoints)[k + 2]) : 0.0;
                if (!x || !y || !flag || (*flag != 0 && *flag != 1 && *flag != 2)) {
                    fail(QStringLiteral(u"keypoints 里有不是数字的值，或可见性不是 0、1、2"));
                    break;
                }
                if (*x == 0 && *y == 0 && *flag == 0)
                    continue;
                Shape point = newShape(ShapeType::Point, (*names)[qsizetype(k / interval)], {{*x, *y}});
                points.append(std::move(point));
                pointFlags.append(*flag == 1);
            }
            if (!labels.error.isEmpty())
                continue;
            const qint64 groupId = poseGroups[index]++;
            setUnedited(shape, difficult, groupId);
            labels.shapes.append(std::move(shape));
            for (qsizetype k = 0; k < points.size(); ++k) {
                setUnedited(points[k], pointFlags[k], groupId);
                labels.shapes.append(std::move(points[k]));
            }
            continue;
        }

        const auto segmentation = annotation.find("segmentation");
        if (segmentation == annotation.end() || segmentation->is_object()) {
            fail(segmentation == annotation.end() ? QStringLiteral(u"没有 segmentation（只有检测框请用 coco-det 导入）")
                                                  : QStringLiteral(u"RLE 格式的分割不能导入，只能导入多边形"));
            continue;
        }
        if (!segmentation->is_array() || segmentation->empty()) {
            fail(QStringLiteral(u"segmentation 里没有多边形（只有检测框请用 coco-det 导入）"));
            continue;
        }
        QList<PointList> polygons;
        for (const Json& polygon : *segmentation) {
            if (polygon.is_object()) {
                fail(QStringLiteral(u"RLE 格式的分割不能导入，只能导入多边形"));
                break;
            }
            if (!polygon.is_array() || polygon.size() < 6 || polygon.size() % 2 != 0) {
                fail(QStringLiteral(u"多边形应有至少 3 个点的成对坐标"));
                break;
            }
            PointList points;
            for (size_t k = 0; k < polygon.size(); k += 2) {
                const std::optional<double> x = numberValue(polygon[k]);
                const std::optional<double> y = numberValue(polygon[k + 1]);
                if (!x || !y) {
                    fail(QStringLiteral(u"多边形的坐标不是数字"));
                    break;
                }
                // Repeated points are left out, as upstream does (exact comparison).
                const bool seen = std::any_of(points.cbegin(), points.cend(), [&](const QPointF& point) {
                    return point.x() == *x && point.y() == *y;
                });
                if (!seen)
                    points.append(QPointF(*x, *y));
            }
            if (!labels.error.isEmpty())
                break;
            if (points.size() < 3) {
                fail(QStringLiteral(u"多边形去掉重复的点后少于 3 个点"));
                break;
            }
            polygons.append(std::move(points));
        }
        if (!labels.error.isEmpty())
            continue;
        std::optional<qint64> groupId;
        if (polygons.size() > 1)
            groupId = ++groups[index];
        for (PointList& points : polygons) {
            Shape shape = newShape(ShapeType::Polygon, label, std::move(points));
            setUnedited(shape, difficult, groupId);
            labels.shapes.append(std::move(shape));
        }
    }
    return result;
}

MaskMapping parseMaskMapping(const QByteArray& bytes)
{
    MaskMapping mapping;
    const JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok) {
        mapping.error = QStringLiteral(u"不是有效的 JSON：%1").arg(parsed.error);
        return mapping;
    }
    const Json& root = parsed.value;
    const auto type = root.is_object() ? root.find("type") : root.end();
    const auto colors = root.is_object() ? root.find("colors") : root.end();
    if (type == root.end() || !type->is_string() || colors == root.end() || !colors->is_object()) {
        mapping.error = QStringLiteral(u"映射文件应有 \"type\" 和 \"colors\"");
        return mapping;
    }
    const std::string typeName = type->get<std::string>();
    if (typeName != "grayscale" && typeName != "rgb") {
        mapping.error = QStringLiteral(u"\"type\" 应是 grayscale 或 rgb");
        return mapping;
    }
    mapping.grayscale = typeName == "grayscale";
    const auto byte = [](const Json& value) -> std::optional<int> {
        const std::optional<double> number = numberValue(value);
        if (!number || *number != std::floor(*number) || *number < 0 || *number > 255)
            return std::nullopt;
        return int(*number);
    };
    for (auto it = colors->begin(); it != colors->end(); ++it) {
        const QString name = utf8(it.key());
        if (mapping.grayscale) {
            const std::optional<int> value = byte(it.value());
            if (!value) {
                mapping.error = QStringLiteral(u"类别“%1”的灰度值应是 0～255 的整数").arg(name);
                return mapping;
            }
            mapping.values.append({name, *value});
        } else {
            std::array<int, 3> color{};
            bool ok = it.value().is_array() && it.value().size() == 3;
            for (size_t k = 0; ok && k < 3; ++k) {
                const std::optional<int> value = byte(it.value()[k]);
                ok = value.has_value();
                if (ok)
                    color[k] = *value;
            }
            if (!ok) {
                mapping.error = QStringLiteral(u"类别“%1”的颜色应是 [R, G, B]，每个 0～255").arg(name);
                return mapping;
            }
            mapping.colors.append({name, color});
        }
    }
    return mapping;
}

ImportedLabels maskLabels(const RgbImage& mask, const MaskMapping& mapping)
{
    ImportedLabels result;
    const std::size_t count = std::size_t(mask.width) * std::size_t(mask.height);
    std::vector<std::uint8_t> binary(count);
    const auto addPolygons = [&result, &binary, &mask](const QString& label) {
        for (const IntPolygon& polygon : maskPolygons(binary, mask.width, mask.height)) {
            PointList points;
            for (const QPoint& point : polygon)
                points.append(QPointF(point.x(), point.y()));
            result.shapes.append(newShape(ShapeType::Polygon, label, std::move(points)));
        }
    };
    if (mapping.grayscale) {
        std::vector<std::uint8_t> gray(count);
        bool present[256] = {};
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* pixel = &mask.rgb[i * 3];
            if (pixel[0] != pixel[1] || pixel[1] != pixel[2]) {
                result.error = QStringLiteral(u"灰度掩膜里有彩色像素（第 %1 行第 %2 列）")
                                   .arg(i / std::size_t(mask.width) + 1)
                                   .arg(i % std::size_t(mask.width) + 1);
                return result;
            }
            gray[i] = pixel[0];
            present[pixel[0]] = true;
        }
        // color_to_label = {v: k}: a value named twice belongs to the later class.
        QString names[256];
        for (const auto& entry : mapping.values)
            names[entry.second] = entry.first;
        bool mapped[256] = {};
        for (const auto& entry : mapping.values)
            mapped[entry.second] = true;
        for (int value = 0; value < 256; ++value) {
            if (!present[value] || !mapped[value])
                continue;
            for (std::size_t i = 0; i < count; ++i)
                binary[i] = gray[i] == value ? 1 : 0;
            addPolygons(names[value]);
        }
    } else {
        for (const auto& entry : mapping.colors) {
            const std::array<int, 3>& color = entry.second;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t* pixel = &mask.rgb[i * 3];
                binary[i] = pixel[0] == color[0] && pixel[1] == color[1] && pixel[2] == color[2] ? 1 : 0;
            }
            addPolygons(entry.first);
        }
    }
    return result;
}

} // namespace liteanno::formats
