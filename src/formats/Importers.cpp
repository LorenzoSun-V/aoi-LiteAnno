#include "formats/Importers.h"

#include "core/ShapeType.h"

#include <QXmlStreamReader>

#include <charconv>
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
void markDifficult(Shape& shape, bool difficult)
{
    if (difficult)
        shape.copiedObject = QByteArrayLiteral("{\"difficult\": true}");
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
    return format == ImportFormat::VocRectangle || format == ImportFormat::VocPolygon ? QStringLiteral(".xml")
                                                                                        : QStringLiteral(".txt");
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

ImportedLabels parseYoloLabels(const QByteArray& bytes, ImportFormat format, const QStringList& classes, int width,
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
        if (*classIndex >= classes.size() || classes[*classIndex].isEmpty()) {
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
        const QString& label = classes[*classIndex];
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

} // namespace liteanno::formats
