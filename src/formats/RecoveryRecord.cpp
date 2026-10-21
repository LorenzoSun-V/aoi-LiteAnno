#include "formats/RecoveryRecord.h"

#include "core/FormatVersion.h"
#include "formats/JsonText.h"

namespace liteanno::formats {

namespace {

std::string utf8(const QString& text)
{
    return text.toStdString();
}

bool readString(const Json& object, const char* key, QString& out)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return false;
    out = QString::fromStdString(it->get_ref<const Json::string_t&>());
    return true;
}

} // namespace

std::optional<QByteArray> recoveryRecordToBytes(const RecoveryRecord& record)
{
    JsonParseResult parsed = parseJson(record.annotation);
    if (!parsed.ok || !parsed.value.is_object())
        return std::nullopt;

    Json json = Json::object();
    json["formatVersion"] = utf8(record.formatVersion.isEmpty() ? QString::fromLatin1(kFormatVersion)
                                                                : record.formatVersion);
    json["projectId"] = utf8(record.projectId);
    json["bindingId"] = utf8(record.bindingId);
    json["rootPathAtWrite"] = utf8(record.rootPathAtWrite);
    json["imagePath"] = utf8(record.imagePath);
    json["documentVersion"] = record.documentVersion;
    json["baseDigest"] = record.baseDigest.isEmpty() ? Json(nullptr) : Json(utf8(record.baseDigest));
    json["createdAt"] = utf8(record.createdAt);
    json["annotation"] = std::move(parsed.value);
    return serializeJson(json);
}

RecoveryRecordResult parseRecoveryRecord(const QByteArray& bytes)
{
    const JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok)
        return {std::nullopt, QStringLiteral("not valid JSON: ") + parsed.error};
    const Json& json = parsed.value;
    if (!json.is_object())
        return {std::nullopt, QStringLiteral("not a JSON object")};

    RecoveryRecord record;
    if (!readString(json, "formatVersion", record.formatVersion))
        return {std::nullopt, QStringLiteral("formatVersion missing")};
    const int major = formatMajorVersion(utf8(record.formatVersion));
    if (major < 0 || major > kFormatVersionMajor)
        return {std::nullopt, QStringLiteral("unsupported formatVersion ") + record.formatVersion};
    if (!readString(json, "projectId", record.projectId) || !readString(json, "bindingId", record.bindingId)
        || !readString(json, "rootPathAtWrite", record.rootPathAtWrite)
        || !readString(json, "imagePath", record.imagePath) || !readString(json, "createdAt", record.createdAt))
        return {std::nullopt, QStringLiteral("a text field is missing")};

    const auto versionIt = json.find("documentVersion");
    if (versionIt == json.end() || !versionIt->is_number_integer())
        return {std::nullopt, QStringLiteral("documentVersion missing")};
    record.documentVersion = versionIt->get<qint64>();

    const auto baseIt = json.find("baseDigest");
    if (baseIt == json.end() || !(baseIt->is_null() || baseIt->is_string()))
        return {std::nullopt, QStringLiteral("baseDigest missing")};
    if (baseIt->is_string())
        record.baseDigest = QString::fromStdString(baseIt->get_ref<const Json::string_t&>());

    const auto annotationIt = json.find("annotation");
    if (annotationIt == json.end() || !annotationIt->is_object())
        return {std::nullopt, QStringLiteral("annotation missing")};
    record.annotation = serializeJson(*annotationIt);
    return {record, QString()};
}

} // namespace liteanno::formats
