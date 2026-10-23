#include "formats/WorksetFiles.h"

#include "core/FormatVersion.h"
#include "core/ImagePath.h"
#include "formats/JsonText.h"

#include <QCryptographicHash>

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

bool supportedVersion(const QString& version)
{
    const int major = formatMajorVersion(utf8(version));
    return major >= 0 && major <= kFormatVersionMajor;
}

QString versionOrDefault(const QString& version)
{
    return version.isEmpty() ? QString::fromLatin1(kFormatVersion) : version;
}

} // namespace

QString computeItemsDigest(const QStringList& items)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (qsizetype i = 0; i < items.size(); ++i) {
        if (i > 0)
            hash.addData(QByteArray("\n"));
        hash.addData(items[i].toUtf8());
    }
    return QStringLiteral("sha256:") + QString::fromLatin1(hash.result().toHex());
}

QByteArray worksetListToBytes(const WorksetList& list)
{
    Json json = Json::object();
    json["formatVersion"] = utf8(versionOrDefault(list.formatVersion));
    json["projectId"] = utf8(list.projectId);
    json["batchId"] = utf8(list.batchId);
    json["number"] = list.number;
    json["createdAt"] = utf8(list.createdAt);
    Json items = Json::array();
    for (const QString& item : list.items)
        items.push_back(utf8(item));
    json["items"] = std::move(items);
    json["itemsDigest"] = utf8(list.itemsDigest.isEmpty() ? computeItemsDigest(list.items) : list.itemsDigest);
    return serializeJson(json);
}

WorksetListResult parseWorksetList(const QByteArray& bytes)
{
    const JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok)
        return {std::nullopt, QStringLiteral("not valid JSON: ") + parsed.error};
    const Json& json = parsed.value;
    if (!json.is_object())
        return {std::nullopt, QStringLiteral("not a JSON object")};

    WorksetList list;
    if (!readString(json, "formatVersion", list.formatVersion))
        return {std::nullopt, QStringLiteral("formatVersion missing")};
    if (!supportedVersion(list.formatVersion))
        return {std::nullopt, QStringLiteral("unsupported formatVersion ") + list.formatVersion};
    if (!readString(json, "projectId", list.projectId) || !readString(json, "batchId", list.batchId)
        || !readString(json, "createdAt", list.createdAt) || !readString(json, "itemsDigest", list.itemsDigest))
        return {std::nullopt, QStringLiteral("a text field is missing")};
    if (list.batchId.isEmpty())
        return {std::nullopt, QStringLiteral("batchId is empty")};

    const auto numberIt = json.find("number");
    if (numberIt == json.end() || !numberIt->is_number_integer() || numberIt->get<qint64>() < 1
        || numberIt->get<qint64>() > 999999)
        return {std::nullopt, QStringLiteral("number must be an integer from 1 to 999999")};
    list.number = int(numberIt->get<qint64>());

    const auto itemsIt = json.find("items");
    if (itemsIt == json.end() || !itemsIt->is_array())
        return {std::nullopt, QStringLiteral("items missing")};
    list.items.reserve(qsizetype(itemsIt->size()));
    for (const Json& item : *itemsIt) {
        if (!item.is_string())
            return {std::nullopt, QStringLiteral("an item is not a path")};
        const QString path = QString::fromStdString(item.get_ref<const Json::string_t&>());
        if (!isValidImagePath(path))
            return {std::nullopt, QStringLiteral("invalid image path: ") + path};
        list.items.append(path);
    }
    if (computeItemsDigest(list.items) != list.itemsDigest)
        return {std::nullopt, QStringLiteral("itemsDigest does not match the items")};
    return {list, QString()};
}

QStringList readItemsOfDamagedList(const QByteArray& bytes)
{
    QStringList items;
    const JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok || !parsed.value.is_object())
        return items;
    const auto itemsIt = parsed.value.find("items");
    if (itemsIt == parsed.value.end() || !itemsIt->is_array())
        return items;
    for (const Json& item : *itemsIt) {
        if (item.is_string())
            items.append(QString::fromStdString(item.get_ref<const Json::string_t&>()));
    }
    return items;
}

QByteArray worksetProgressToBytes(const WorksetProgress& progress)
{
    Json json = Json::object();
    Json originalEvents = Json::array();
    if (!progress.original.isEmpty()) {
        JsonParseResult parsed = parseJson(progress.original);
        if (parsed.ok && parsed.value.is_object()) {
            json = std::move(parsed.value);
            const auto eventsIt = json.find("events");
            if (eventsIt != json.end() && eventsIt->is_array())
                originalEvents = *eventsIt;
        }
    }
    // Known fields are set where they are; unknown ones stay as they were.
    json["formatVersion"] = utf8(versionOrDefault(progress.formatVersion));
    json["batchId"] = utf8(progress.batchId);
    json["listDigest"] = utf8(progress.listDigest);
    json["lastItem"] = progress.lastItem.isEmpty() ? Json(nullptr) : Json(utf8(progress.lastItem));
    // Events are only ever appended: the ones read keep their other fields.
    Json events = Json::array();
    for (qsizetype i = 0; i < progress.events.size(); ++i) {
        const WorksetEvent& event = progress.events[i];
        Json entry = size_t(i) < originalEvents.size() && originalEvents[size_t(i)].is_object() ? originalEvents[size_t(i)]
                                                                                                 : Json::object();
        entry["type"] = utf8(event.type);
        entry["at"] = utf8(event.at);
        events.push_back(std::move(entry));
    }
    json["events"] = std::move(events);
    return serializeJson(json);
}

std::optional<WorksetProgress> parseWorksetProgress(const QByteArray& bytes)
{
    const JsonParseResult parsed = parseJson(bytes);
    if (!parsed.ok || !parsed.value.is_object())
        return std::nullopt;
    const Json& json = parsed.value;
    WorksetProgress progress;
    progress.original = bytes;
    if (!readString(json, "formatVersion", progress.formatVersion) || !supportedVersion(progress.formatVersion)
        || !readString(json, "batchId", progress.batchId) || !readString(json, "listDigest", progress.listDigest))
        return std::nullopt;
    const auto lastIt = json.find("lastItem");
    if (lastIt != json.end() && !lastIt->is_null()) {
        if (!lastIt->is_string())
            return std::nullopt;
        progress.lastItem = QString::fromStdString(lastIt->get_ref<const Json::string_t&>());
    }
    const auto eventsIt = json.find("events");
    if (eventsIt != json.end()) {
        if (!eventsIt->is_array())
            return std::nullopt;
        for (const Json& entry : *eventsIt) {
            WorksetEvent event;
            if (!entry.is_object() || !readString(entry, "type", event.type) || !readString(entry, "at", event.at))
                return std::nullopt;
            progress.events.append(event);
        }
    }
    return progress;
}

} // namespace liteanno::formats
