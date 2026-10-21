#include "formats/ProjectFile.h"

#include "core/FormatVersion.h"
#include "formats/JsonText.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <cstdint>
#include <limits>

namespace liteanno::formats {

namespace {

// Thrown inside the parser only; parseProjectFile turns it into a result.
struct FieldError {
    QString field;
    QString error;
};

[[noreturn]] void fail(const QString& field, const char* error)
{
    throw FieldError{field, QString::fromLatin1(error)};
}

QString text(const Json& value)
{
    return QString::fromStdString(value.get_ref<const Json::string_t&>());
}

QString child(const QString& field, const std::string& key)
{
    QString token = QString::fromStdString(key);
    token.replace(QLatin1Char('~'), QLatin1String("~0"));
    token.replace(QLatin1Char('/'), QLatin1String("~1"));
    return field + QLatin1Char('/') + token;
}

QString child(const QString& field, std::size_t index)
{
    return field + QLatin1Char('/') + QString::number(index);
}

QString requireString(const Json& value, const QString& field, bool allowEmpty = false)
{
    if (!value.is_string())
        fail(field, "must be a string");
    const QString result = text(value);
    if (!allowEmpty && result.isEmpty())
        fail(field, "must not be empty");
    return result;
}

// An integer from 0 to INT_MAX; JSON numbers with a fraction part do not count.
std::optional<int> nonNegativeInt(const Json& value)
{
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            return static_cast<int>(number);
    }
    return std::nullopt;
}

const Json& requireArray(const Json& value, const QString& field)
{
    if (!value.is_array())
        fail(field, "must be an array");
    return value;
}

const Json& requireObject(const Json& value, const QString& field)
{
    if (!value.is_object())
        fail(field, "must be an object");
    return value;
}

// Non-empty strings without duplicates.
QStringList uniqueStrings(const Json& value, const QString& field)
{
    QStringList result;
    QSet<QString> seen;
    const Json& array = requireArray(value, field);
    for (std::size_t i = 0; i < array.size(); ++i) {
        const QString item = requireString(array[i], child(field, i));
        if (seen.contains(item))
            fail(child(field, i), "duplicate value");
        seen.insert(item);
        result.append(item);
    }
    return result;
}

QList<ShapeType> readShapeTypes(const Json& value, const QString& field)
{
    QList<ShapeType> result;
    const QStringList names = uniqueStrings(value, field);
    for (qsizetype i = 0; i < names.size(); ++i) {
        const std::optional<ShapeType> type = shapeTypeFromName(names[i]);
        if (!type || !isEditableShapeType(*type))
            fail(child(field, static_cast<std::size_t>(i)), "not an editable shape type");
        result.append(*type);
    }
    return result;
}

QList<ClassDef> readClasses(const Json& value, const QString& field)
{
    static const QRegularExpression colorPattern(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    QList<ClassDef> result;
    QSet<int> ids;
    QSet<QString> names;
    const Json& array = requireArray(value, field);
    for (std::size_t i = 0; i < array.size(); ++i) {
        const QString itemField = child(field, i);
        const Json& item = requireObject(array[i], itemField);
        ClassDef def;

        const auto idIt = item.find("id");
        if (idIt == item.end())
            fail(child(itemField, "id"), "is required");
        const std::optional<int> id = nonNegativeInt(*idIt);
        if (!id)
            fail(child(itemField, "id"), "must be a non-negative integer");
        if (ids.contains(*id))
            fail(child(itemField, "id"), "duplicate class id");
        ids.insert(*id);
        def.id = *id;

        const auto nameIt = item.find("name");
        if (nameIt == item.end())
            fail(child(itemField, "name"), "is required");
        def.name = requireString(*nameIt, child(itemField, "name"));
        if (names.contains(def.name))
            fail(child(itemField, "name"), "duplicate class name");
        names.insert(def.name);

        const auto colorIt = item.find("color");
        if (colorIt != item.end() && !colorIt->is_null()) {
            def.color = requireString(*colorIt, child(itemField, "color"));
            if (!colorPattern.match(def.color).hasMatch())
                fail(child(itemField, "color"), "must be a color like #E53935");
        }
        result.append(def);
    }
    return result;
}

ImageLabelConfig readImageLabels(const Json& value, const QString& field)
{
    ImageLabelConfig result;
    const Json& object = requireObject(value, field);
    const auto modeIt = object.find("mode");
    if (modeIt == object.end())
        fail(child(field, "mode"), "is required");
    const QString mode = requireString(*modeIt, child(field, "mode"));
    if (mode == QLatin1String("single"))
        result.mode = ImageLabelMode::Single;
    else if (mode == QLatin1String("multi"))
        result.mode = ImageLabelMode::Multi;
    else
        fail(child(field, "mode"), "must be \"single\" or \"multi\"");
    const auto optionsIt = object.find("options");
    if (optionsIt == object.end())
        fail(child(field, "options"), "is required");
    result.options = uniqueStrings(*optionsIt, child(field, "options"));
    return result;
}

QHash<QString, QList<AttributeDef>> readAttributes(const Json& value, const QString& field)
{
    QHash<QString, QList<AttributeDef>> result;
    const Json& object = requireObject(value, field);
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString classField = child(field, it.key());
        const Json& array = requireArray(*it, classField);
        QList<AttributeDef> defs;
        QSet<QString> names;
        for (std::size_t i = 0; i < array.size(); ++i) {
            const QString itemField = child(classField, i);
            const Json& item = requireObject(array[i], itemField);
            AttributeDef def;
            const auto nameIt = item.find("name");
            if (nameIt == item.end())
                fail(child(itemField, "name"), "is required");
            def.name = requireString(*nameIt, child(itemField, "name"));
            if (names.contains(def.name))
                fail(child(itemField, "name"), "duplicate attribute name");
            names.insert(def.name);
            const auto optionsIt = item.find("options");
            if (optionsIt != item.end())
                def.options = uniqueStrings(*optionsIt, child(itemField, "options"));
            const auto requiredIt = item.find("required");
            if (requiredIt != item.end()) {
                if (!requiredIt->is_boolean())
                    fail(child(itemField, "required"), "must be true or false");
                def.required = requiredIt->get<bool>();
            }
            defs.append(def);
        }
        result.insert(QString::fromStdString(it.key()), defs);
    }
    return result;
}

ProjectConfig readProject(const Json& root)
{
    ProjectConfig config;
    requireObject(root, QString());

    const auto versionIt = root.find("formatVersion");
    if (versionIt == root.end())
        fail(QStringLiteral("/formatVersion"), "is required");
    config.formatVersion = requireString(*versionIt, QStringLiteral("/formatVersion"));
    const int major = formatMajorVersion(versionIt->get_ref<const Json::string_t&>());
    if (major < 0)
        fail(QStringLiteral("/formatVersion"), "must have the form major.minor");
    if (major > kFormatVersionMajor)
        fail(QStringLiteral("/formatVersion"), "was written by a newer version of the program");

    const auto idIt = root.find("projectId");
    if (idIt == root.end())
        fail(QStringLiteral("/projectId"), "is required");
    config.projectId = requireString(*idIt, QStringLiteral("/projectId"));
    if (QUuid::fromString(config.projectId).isNull() || config.projectId.size() != 36)
        fail(QStringLiteral("/projectId"), "must be a UUID such as 3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33");
    config.projectId = config.projectId.toLower();

    const auto nameIt = root.find("name");
    if (nameIt != root.end())
        config.name = requireString(*nameIt, QStringLiteral("/name"), true);

    const auto createdIt = root.find("createdAt");
    if (createdIt != root.end()) {
        config.createdAt = requireString(*createdIt, QStringLiteral("/createdAt"));
        const QDateTime time = QDateTime::fromString(config.createdAt, Qt::ISODateWithMs);
        if (!time.isValid() || time.timeSpec() == Qt::LocalTime)
            fail(QStringLiteral("/createdAt"), "must be an ISO 8601 time with a time zone");
    }

    const auto shapeTypesIt = root.find("shapeTypes");
    if (shapeTypesIt != root.end() && !shapeTypesIt->is_null())
        config.shapeTypes = readShapeTypes(*shapeTypesIt, QStringLiteral("/shapeTypes"));

    const auto classesIt = root.find("classes");
    if (classesIt != root.end() && !classesIt->is_null())
        config.classes = readClasses(*classesIt, QStringLiteral("/classes"));

    const auto imageLabelsIt = root.find("imageLabels");
    if (imageLabelsIt != root.end() && !imageLabelsIt->is_null())
        config.imageLabels = readImageLabels(*imageLabelsIt, QStringLiteral("/imageLabels"));

    const auto attributesIt = root.find("attributes");
    if (attributesIt != root.end() && !attributesIt->is_null())
        config.attributes = readAttributes(*attributesIt, QStringLiteral("/attributes"));

    const auto batchSizeIt = root.find("batchSize");
    if (batchSizeIt != root.end()) {
        const std::optional<int> batchSize = nonNegativeInt(*batchSizeIt);
        if (!batchSize || *batchSize == 0)
            fail(QStringLiteral("/batchSize"), "must be a positive integer");
        config.batchSize = *batchSize;
    }
    return config;
}

} // namespace

ProjectFileResult parseProjectFile(const QByteArray& bytes)
{
    const JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok)
        return {std::nullopt, QString(), QStringLiteral("not valid JSON: ") + parsed.error, QString()};
    try {
        return {readProject(parsed.value), QString(), QString(),
                QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())};
    } catch (const FieldError& error) {
        return {std::nullopt, error.field, error.error, QString()};
    }
}

ProjectFileResult readProjectFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {std::nullopt, QString(), file.errorString(), QString()};
    return parseProjectFile(file.readAll());
}

} // namespace liteanno::formats
