#include "formats/AnnotationFile.h"

#include "core/FormatVersion.h"
#include "core/Version.h"
#include "formats/JsonText.h"

#include <QHash>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace liteanno::formats {

namespace {

struct OriginalShape {
    std::size_t baseIndex = 0;  // position in the "shapes" array of the base JSON
    Shape typed;                // the shape as read, to tell which fields an edit changed
};

struct TypedContent {
    QList<Shape> shapes;
    QList<ImageLabel> imageLabels;
    ReviewState review;
};

QString fromJson(const Json& value)
{
    return QString::fromStdString(value.get_ref<const Json::string_t&>());
}

std::string toJson(const QString& text)
{
    return text.toStdString();
}

// One reference token of a JSON pointer (RFC 6901).
QString pointerToken(const QString& key)
{
    QString token = key;
    token.replace(QLatin1Char('~'), QLatin1String("~0"));
    token.replace(QLatin1Char('/'), QLatin1String("~1"));
    return token;
}

QString pointer(const char* key)
{
    return QLatin1Char('/') + QLatin1String(key);
}

bool isPositiveInteger(const Json& value)
{
    if (value.is_number_unsigned())
        return value.get<Json::number_unsigned_t>() > 0;
    if (value.is_number_integer())
        return value.get<Json::number_integer_t>() > 0;
    if (value.is_number_float()) {
        const double number = value.get<double>();
        return number >= 1 && number == std::floor(number);
    }
    return false;
}

// Upstream requires a list of integer pairs, or the whole file fails to open
// (shape.py, load_from_dict).
bool isValidKieLinking(const Json& value)
{
    if (!value.is_array())
        return false;
    return std::all_of(value.begin(), value.end(), [](const Json& pair) {
        return pair.is_array() && pair.size() == 2 && pair[0].is_number_integer() && pair[1].is_number_integer();
    });
}

// New and edited coordinates are always written as decimals, as upstream does (13.5).
Json pointsToJson(const PointList& points)
{
    Json array = Json::array();
    for (const QPointF& point : points)
        array.push_back(Json::array({point.x(), point.y()}));
    return array;
}

bool jsonPointsEqual(const Json& value, const PointList& points)
{
    if (!value.is_array() || value.size() != static_cast<std::size_t>(points.size()))
        return false;
    for (qsizetype i = 0; i < points.size(); ++i) {
        const Json& point = value[static_cast<std::size_t>(i)];
        if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number())
            return false;
        if (point[0].get<double>() != points[i].x() || point[1].get<double>() != points[i].y())
            return false;
    }
    return true;
}

// Field order of upstream Shape.to_dict() (shape.py).
Json newShapeObject(const Shape& shape)
{
    Json object = Json::object();
    object["label"] = toJson(shape.label);
    object["score"] = nullptr;
    object["points"] = pointsToJson(shape.points);
    object["group_id"] = nullptr;
    object["description"] = toJson(shape.description);
    object["difficult"] = false;
    object["shape_type"] = toJson(shapeTypeName(shape.type));
    object["flags"] = Json::object();
    object["attributes"] = Json::object();
    object["kie_linking"] = Json::array();
    if (shape.locked)
        object["locked"] = true;
    return object;
}

void applyShapeEdits(Json& object, const Shape& before, const Shape& after)
{
    // Format spec 4.5: a save completes a missing shape_type (read as a polygon, 13.5). An
    // existing null is a value the edit did not touch and stays as it is (13.3).
    if (!object.contains("shape_type"))
        object["shape_type"] = toJson(shapeTypeName(after.type));
    if (after.label != before.label)
        object["label"] = toJson(after.label);
    // Format spec 4.5: rectangles are written as four corner points, also when only another
    // part of the file was edited.
    const bool pointsEdited = !exactlyEqual(after.points, before.points);
    const bool rectangleNotCanonical =
        after.type == ShapeType::Rectangle && !jsonPointsEqual(object.at("points"), after.points);
    if (pointsEdited || rectangleNotCanonical)
        object["points"] = pointsToJson(after.points);
    if (after.description != before.description)
        object["description"] = toJson(after.description);
    if (after.locked != before.locked) {
        // Upstream writes "locked" only when it is true.
        if (after.locked)
            object["locked"] = true;
        else
            object.erase("locked");
    }
}

} // namespace

struct AnnotationFile::Data {
    ImageInfo image;
    bool isNew = false;
    // The file content as read or last saved; edits are applied to a copy of it.
    Json base;
    QHash<quint64, OriginalShape> originalShapes;
    QList<ImageLabel> originalImageLabels;
    AnnotationDocument document;
    QList<Diagnostic> diagnostics;
    ReadOnlyReason readOnlyReason = ReadOnlyReason::None;
    bool sizeCorrectionPending = false;

    void addDiagnostic(DiagnosticCode code, const QString& location, const QString& detail = QString())
    {
        diagnostics.append(Diagnostic{code, location, detail});
    }

    void makeReadOnly(ReadOnlyReason reason)
    {
        if (readOnlyReason == ReadOnlyReason::None || reason < readOnlyReason)
            readOnlyReason = reason;
    }

    TypedContent readBase(const QList<quint64>& shapeIds);
    Shape readShape(const Json& item, std::size_t index);
    void readLiteanno(const Json& liteanno, ReviewState& review);
    bool imageDimensionMismatches(const char* key, int actual);

    Json buildOutput() const;
    void applyImageLabels(Json& out) const;
    void writeImageDimension(Json& out, const char* key, int actual) const;
};

TypedContent AnnotationFile::Data::readBase(const QList<quint64>& shapeIds)
{
    diagnostics.clear();
    readOnlyReason = ReadOnlyReason::None;
    originalShapes.clear();
    TypedContent content;

    bool hasShapes = false;
    const auto shapesIt = base.find("shapes");
    if (shapesIt == base.end()) {
        addDiagnostic(DiagnosticCode::MissingShapes, pointer("shapes"));
    } else if (!shapesIt->is_array()) {
        addDiagnostic(DiagnosticCode::ShapesNotArray, pointer("shapes"));
        makeReadOnly(ReadOnlyReason::JsonError);
    } else {
        hasShapes = !shapesIt->empty();
        for (std::size_t i = 0; i < shapesIt->size(); ++i) {
            Shape shape = readShape((*shapesIt)[i], i);
            const auto position = static_cast<qsizetype>(i);
            shape.id = position < shapeIds.size() ? shapeIds[position] : static_cast<quint64>(i + 1);
            originalShapes.insert(shape.id, OriginalShape{i, shape});
            content.shapes.append(std::move(shape));
        }
    }

    if (!base.contains("version"))
        addDiagnostic(DiagnosticCode::MissingVersion, pointer("version"));

    const auto flagsIt = base.find("flags");
    if (flagsIt != base.end()) {
        if (flagsIt->is_object()) {
            for (auto it = flagsIt->begin(); it != flagsIt->end(); ++it) {
                ImageLabel label{QString::fromStdString(it.key()), std::nullopt};
                if (it.value().is_boolean()) {
                    label.value = it.value().get<bool>();
                } else {
                    addDiagnostic(DiagnosticCode::InvalidImageLabelValue,
                                  QLatin1String("/flags/") + pointerToken(label.name));
                }
                content.imageLabels.append(label);
            }
        } else {
            addDiagnostic(DiagnosticCode::InvalidFlags, pointer("flags"));
        }
    }
    originalImageLabels = content.imageLabels;

    const auto checkedIt = base.find("checked");
    if (checkedIt != base.end()) {
        if (checkedIt->is_boolean())
            content.review.checked = checkedIt->get<bool>();
        else
            addDiagnostic(DiagnosticCode::InvalidChecked, pointer("checked"));
    }

    const auto liteannoIt = base.find("liteanno");
    if (liteannoIt != base.end()) {
        if (liteannoIt->is_object())
            readLiteanno(*liteannoIt, content.review);
        else
            addDiagnostic(DiagnosticCode::InvalidLiteanno, pointer("liteanno"));
    }

    const auto imagePathIt = base.find("imagePath");
    if (imagePathIt == base.end() || !imagePathIt->is_string()) {
        addDiagnostic(DiagnosticCode::MissingImagePath, pointer("imagePath"));
    } else if (!image.fileName.isEmpty() && fromJson(*imagePathIt) != image.fileName) {
        addDiagnostic(DiagnosticCode::ImagePathMismatch, pointer("imagePath"), fromJson(*imagePathIt));
    }

    const auto imageDataIt = base.find("imageData");
    if (imageDataIt == base.end())
        addDiagnostic(DiagnosticCode::MissingImageData, pointer("imageData"));
    else if (!imageDataIt->is_null())
        addDiagnostic(DiagnosticCode::EmbeddedImageData, pointer("imageData"));

    const bool heightMismatch = imageDimensionMismatches("imageHeight", image.height);
    const bool widthMismatch = imageDimensionMismatches("imageWidth", image.width);
    if (heightMismatch || widthMismatch)
        makeReadOnly(ReadOnlyReason::ImageSizeMismatch);

    // Format spec 13.4: shapes from another tool on an image with an EXIF orientation may be
    // in the unrotated frame.
    if (image.exifOrientation >= 2 && image.exifOrientation <= 8 && hasShapes && liteannoIt == base.end()) {
        addDiagnostic(DiagnosticCode::ExifOrientationUnconfirmed, QString(), QString::number(image.exifOrientation));
        makeReadOnly(ReadOnlyReason::ExifOrientationUnconfirmed);
    }

    return content;
}

Shape AnnotationFile::Data::readShape(const Json& item, std::size_t index)
{
    Shape shape;
    const QString location = QStringLiteral("/shapes/%1").arg(index);
    if (!item.is_object()) {
        shape.kind = ShapeKind::Unrecognized;
        addDiagnostic(DiagnosticCode::ShapeUnrecognized, location, QStringLiteral("not an object"));
        return shape;
    }

    QString problem;  // the first reason the object is unrecognized
    const auto unrecognized = [&problem](const char* reason) {
        if (problem.isEmpty())
            problem = QString::fromLatin1(reason);
    };

    const auto labelIt = item.find("label");
    if (labelIt == item.end())
        unrecognized("missing label");
    else if (!labelIt->is_string())
        unrecognized("label is not a string");
    else
        shape.label = fromJson(*labelIt);

    std::optional<ShapeType> type;
    const auto typeIt = item.find("shape_type");
    if (typeIt == item.end() || typeIt->is_null()) {
        // Upstream's Shape treats a missing or null shape_type as a polygon.
        type = ShapeType::Polygon;
        addDiagnostic(DiagnosticCode::ShapeTypeDefaulted, location + QLatin1String("/shape_type"));
    } else if (!typeIt->is_string()) {
        unrecognized("shape_type is not a string");
    } else {
        type = shapeTypeFromName(fromJson(*typeIt));
        if (!type)
            unrecognized("unknown shape_type");
    }

    PointList points;
    const auto pointsIt = item.find("points");
    if (pointsIt == item.end() || !pointsIt->is_array() || pointsIt->empty()) {
        unrecognized("points missing or empty");
    } else {
        for (const Json& point : *pointsIt) {
            if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) {
                unrecognized("a point is not two numbers");
                points.clear();
                break;
            }
            points.append(QPointF(point[0].get<double>(), point[1].get<double>()));
        }
    }

    const auto kieLinkingIt = item.find("kie_linking");
    if (kieLinkingIt != item.end() && !isValidKieLinking(*kieLinkingIt))
        unrecognized("kie_linking is not a list of integer pairs");

    if (problem.isEmpty() && !acceptsPointCount(*type, points.size()))
        unrecognized("wrong number of points for the shape_type");

    if (type)
        shape.type = *type;
    shape.points = points;
    if (!problem.isEmpty()) {
        shape.kind = ShapeKind::Unrecognized;
        addDiagnostic(DiagnosticCode::ShapeUnrecognized, location, problem);
        return shape;
    }
    if (isEditableShapeType(shape.type)) {
        if (shape.type == ShapeType::Rectangle)
            shape.points = canonicalRectangle(points);
    } else {
        shape.kind = ShapeKind::PreserveOnly;
        addDiagnostic(DiagnosticCode::ShapePreserveOnly, location, shapeTypeName(shape.type));
    }

    const auto descriptionIt = item.find("description");
    if (descriptionIt != item.end()) {
        if (descriptionIt->is_string())
            shape.description = fromJson(*descriptionIt);
        else
            addDiagnostic(DiagnosticCode::InvalidShapeField, location + QLatin1String("/description"));
    }
    const auto lockedIt = item.find("locked");
    if (lockedIt != item.end()) {
        if (lockedIt->is_boolean())
            shape.locked = lockedIt->get<bool>();
        else
            addDiagnostic(DiagnosticCode::InvalidShapeField, location + QLatin1String("/locked"));
    }
    const auto attributesIt = item.find("attributes");
    if (attributesIt != item.end()) {
        if (attributesIt->is_object()) {
            // Only string values count as filled in (format spec 5.3); others stay in the file.
            for (auto it = attributesIt->begin(); it != attributesIt->end(); ++it) {
                if (it->is_string())
                    shape.attributes.insert(QString::fromStdString(it.key()), fromJson(*it));
            }
        } else if (!attributesIt->is_null()) {
            addDiagnostic(DiagnosticCode::InvalidShapeField, location + QLatin1String("/attributes"));
        }
    }
    return shape;
}

void AnnotationFile::Data::readLiteanno(const Json& liteanno, ReviewState& review)
{
    const auto versionIt = liteanno.find("formatVersion");
    if (versionIt == liteanno.end()) {
        addDiagnostic(DiagnosticCode::MissingFormatVersion, QStringLiteral("/liteanno/formatVersion"));
    } else {
        const int major = versionIt->is_string() ? formatMajorVersion(versionIt->get_ref<const Json::string_t&>()) : -1;
        if (major < 0 || major > kFormatVersionMajor) {
            addDiagnostic(DiagnosticCode::UnsupportedFormatVersion, QStringLiteral("/liteanno/formatVersion"),
                          versionIt->is_string() ? fromJson(*versionIt) : QString());
            makeReadOnly(ReadOnlyReason::UnsupportedFormatVersion);
        }
    }

    const auto statusIt = liteanno.find("status");
    if (statusIt != liteanno.end()) {
        if (statusIt->is_string() && statusIt->get_ref<const Json::string_t&>() == "skipped") {
            review.skip = SkipState::Skipped;
        } else {
            review.skip = SkipState::Unknown;
            addDiagnostic(DiagnosticCode::UnknownStatus, QStringLiteral("/liteanno/status"));
        }
    }

    const auto reasonIt = liteanno.find("skipReason");
    if (reasonIt != liteanno.end()) {
        if (reasonIt->is_string())
            review.skipReason = fromJson(*reasonIt);
        else
            addDiagnostic(DiagnosticCode::InvalidSkipReason, QStringLiteral("/liteanno/skipReason"));
    }
}

bool AnnotationFile::Data::imageDimensionMismatches(const char* key, int actual)
{
    const auto it = base.find(key);
    if (it == base.end()) {
        addDiagnostic(DiagnosticCode::MissingImageSize, pointer(key));
        return false;
    }
    // 13.3: a value that is not a positive integer counts as missing.
    if (!isPositiveInteger(*it)) {
        addDiagnostic(DiagnosticCode::InvalidImageSize, pointer(key));
        return false;
    }
    if (actual > 0 && it->get<double>() != actual) {
        addDiagnostic(DiagnosticCode::ImageSizeMismatch, pointer(key),
                      QStringLiteral("file %1, image %2").arg(it->get<double>()).arg(actual));
        return true;
    }
    return false;
}

Json AnnotationFile::Data::buildOutput() const
{
    Json out = base;

    Json shapes = Json::array();
    for (const Shape& shape : document.shapes()) {
        const auto original = originalShapes.constFind(shape.id);
        if (original == originalShapes.cend()) {
            shapes.push_back(newShapeObject(shape));
            continue;
        }
        Json object = base.at("shapes").at(original->baseIndex);
        if (shape.kind == ShapeKind::Editable)
            applyShapeEdits(object, original->typed, shape);
        shapes.push_back(std::move(object));
    }
    out["shapes"] = std::move(shapes);

    // Format spec 4.5: the version of the program that saved the file.
    out["version"] = kAppVersion;

    applyImageLabels(out);

    if (document.reviewStateWritten() || !out.contains("checked"))
        out["checked"] = document.review().checked;

    if (!image.fileName.isEmpty()) {
        const auto imagePathIt = out.find("imagePath");
        if (imagePathIt == out.end() || !imagePathIt->is_string() || fromJson(*imagePathIt) != image.fileName)
            out["imagePath"] = toJson(image.fileName);
    } else if (!out.contains("imagePath")) {
        out["imagePath"] = "";
    }

    if (!out.contains("imageData"))
        out["imageData"] = nullptr;

    writeImageDimension(out, "imageHeight", image.height);
    writeImageDimension(out, "imageWidth", image.width);

    Json& liteanno = out["liteanno"];
    if (!liteanno.is_object())
        liteanno = Json::object();
    liteanno["formatVersion"] = kFormatVersion;
    if (document.reviewStateWritten()) {
        switch (document.review().skip) {
        case SkipState::None:
            liteanno.erase("status");
            liteanno.erase("skipReason");
            break;
        case SkipState::Skipped:
            liteanno["status"] = "skipped";
            liteanno["skipReason"] = toJson(document.review().skipReason);
            break;
        case SkipState::Unknown:
            break;
        }
    }
    return out;
}

// Format spec 13.1: existing keys are never removed; only labels the user changed are written.
void AnnotationFile::Data::applyImageLabels(Json& out) const
{
    const QList<ImageLabel>& labels = document.imageLabels();
    const auto changed = [this](const ImageLabel& label) {
        for (const ImageLabel& before : originalImageLabels) {
            if (before.name == label.name)
                return before.value != label.value;
        }
        return true;
    };

    const auto flagsIt = out.find("flags");
    if (flagsIt != out.end() && flagsIt->is_object()) {
        for (const ImageLabel& label : labels) {
            if (label.value && changed(label))
                (*flagsIt)[toJson(label.name)] = *label.value;
        }
        return;
    }
    // Missing, or not an object: an invalid value stays until the user sets a label (13.3).
    if (flagsIt != out.end() && std::none_of(labels.begin(), labels.end(), changed))
        return;
    Json flags = Json::object();
    for (const ImageLabel& label : labels) {
        if (label.value)
            flags[toJson(label.name)] = *label.value;
    }
    out["flags"] = std::move(flags);
}

void AnnotationFile::Data::writeImageDimension(Json& out, const char* key, int actual) const
{
    if (actual <= 0)
        return;
    const auto it = out.find(key);
    if (it == out.end() || !isPositiveInteger(*it) || sizeCorrectionPending)
        out[key] = actual;
}

AnnotationFile::AnnotationFile(std::unique_ptr<Data> data)
    : d(std::move(data))
{
}

AnnotationFile::AnnotationFile(AnnotationFile&& other) noexcept = default;
AnnotationFile& AnnotationFile::operator=(AnnotationFile&& other) noexcept = default;
AnnotationFile::~AnnotationFile() = default;

AnnotationFile AnnotationFile::fromBytes(const QByteArray& bytes, const ImageInfo& image)
{
    auto data = std::make_unique<Data>();
    data->image = image;

    JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok) {
        const QString detail = parsed.byteOffset >= 0
            ? QStringLiteral("%1 (byte %2)").arg(parsed.error).arg(parsed.byteOffset)
            : parsed.error;
        data->addDiagnostic(DiagnosticCode::JsonError, QString(), detail);
        data->makeReadOnly(ReadOnlyReason::JsonError);
        return AnnotationFile(std::move(data));
    }
    if (!parsed.value.is_object()) {
        data->addDiagnostic(DiagnosticCode::TopLevelNotObject, QString());
        data->makeReadOnly(ReadOnlyReason::JsonError);
        return AnnotationFile(std::move(data));
    }

    data->base = std::move(parsed.value);
    TypedContent content = data->readBase({});
    data->document = AnnotationDocument(std::move(content.shapes), std::move(content.imageLabels),
                                        std::move(content.review));
    return AnnotationFile(std::move(data));
}

AnnotationFile AnnotationFile::createNew(const ImageInfo& image, const QStringList& imageLabelOptions)
{
    auto data = std::make_unique<Data>();
    data->image = image;
    data->isNew = true;

    // Upstream's key order (schema.py, create_xlabel_template), then the liteanno extension.
    Json base = Json::object();
    base["version"] = kAppVersion;
    Json flags = Json::object();
    for (const QString& option : imageLabelOptions)
        flags[toJson(option)] = false;
    base["flags"] = std::move(flags);
    base["checked"] = false;
    base["shapes"] = Json::array();
    base["imagePath"] = toJson(image.fileName);
    base["imageData"] = nullptr;
    if (image.height > 0 && image.width > 0) {
        base["imageHeight"] = image.height;
        base["imageWidth"] = image.width;
    }
    Json liteanno = Json::object();
    liteanno["formatVersion"] = kFormatVersion;
    base["liteanno"] = std::move(liteanno);

    data->base = std::move(base);
    TypedContent content = data->readBase({});
    data->document = AnnotationDocument(std::move(content.shapes), std::move(content.imageLabels),
                                        std::move(content.review));
    return AnnotationFile(std::move(data));
}

AnnotationFile AnnotationFile::unreadable(const ImageInfo& image, const QString& error)
{
    auto data = std::make_unique<Data>();
    data->image = image;
    data->addDiagnostic(DiagnosticCode::FileUnreadable, QString(), error);
    data->makeReadOnly(ReadOnlyReason::Unreadable);
    return AnnotationFile(std::move(data));
}

const ImageInfo& AnnotationFile::image() const
{
    return d->image;
}

bool AnnotationFile::isNew() const
{
    return d->isNew;
}

ReadOnlyReason AnnotationFile::readOnlyReason() const
{
    return d->readOnlyReason;
}

const QList<Diagnostic>& AnnotationFile::diagnostics() const
{
    return d->diagnostics;
}

AnnotationDocument& AnnotationFile::document()
{
    return d->document;
}

const AnnotationDocument& AnnotationFile::document() const
{
    return d->document;
}

bool AnnotationFile::correctImageSize()
{
    if (d->readOnlyReason != ReadOnlyReason::ImageSizeMismatch)
        return false;
    d->sizeCorrectionPending = true;
    d->readOnlyReason = ReadOnlyReason::None;
    return true;
}

bool AnnotationFile::needsWrite() const
{
    return !isReadOnly() && (d->document.isModified() || d->sizeCorrectionPending);
}

QByteArray AnnotationFile::toBytes() const
{
    return serializeJson(d->buildOutput());
}

void AnnotationFile::markSaved()
{
    d->base = d->buildOutput();
    d->isNew = false;
    d->sizeCorrectionPending = false;
    QList<quint64> shapeIds;
    for (const Shape& shape : d->document.shapes())
        shapeIds.append(shape.id);
    // Refresh the originals and the diagnostics; the document already holds this content.
    d->readBase(shapeIds);
    d->document.markSaved();
}

const char* readOnlyReasonName(ReadOnlyReason reason)
{
    switch (reason) {
    case ReadOnlyReason::None: return "None";
    case ReadOnlyReason::Unreadable: return "Unreadable";
    case ReadOnlyReason::JsonError: return "JsonError";
    case ReadOnlyReason::UnsupportedFormatVersion: return "UnsupportedFormatVersion";
    case ReadOnlyReason::ExifOrientationUnconfirmed: return "ExifOrientationUnconfirmed";
    case ReadOnlyReason::ImageSizeMismatch: return "ImageSizeMismatch";
    }
    return "Unknown";
}

} // namespace liteanno::formats
