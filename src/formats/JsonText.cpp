#include "formats/JsonText.h"

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace liteanno::formats {

namespace {

// Builds the document from SAX events. nlohmann's own DOM parser would silently turn an
// integer that does not fit in 64 bits into a double; this one rejects it instead.
class DomBuilder
{
public:
    explicit DomBuilder(Json& root)
        : m_root(root)
    {
    }

    bool null() { return addValue(Json(nullptr)); }
    bool boolean(bool value) { return addValue(Json(value)); }
    bool number_integer(Json::number_integer_t value) { return addValue(Json(value)); }
    bool number_unsigned(Json::number_unsigned_t value) { return addValue(Json(value)); }
    bool number_float(Json::number_float_t value, const Json::string_t& text)
    {
        if (text.find_first_of(".eE") == Json::string_t::npos) {
            m_error = "integer does not fit in 64 bits: " + text;
            return false;
        }
        return addValue(Json(value));
    }
    bool string(Json::string_t& value) { return addValue(Json(std::move(value))); }
    bool binary(Json::binary_t& /*value*/)
    {
        m_error = "binary values are not JSON";
        return false;
    }

    bool start_object(std::size_t /*elements*/) { return openContainer(Json::object()); }
    bool key(Json::string_t& key)
    {
        m_key = std::move(key);
        return true;
    }
    bool end_object() { return closeContainer(); }
    bool start_array(std::size_t /*elements*/) { return openContainer(Json::array()); }
    bool end_array() { return closeContainer(); }

    bool parse_error(std::size_t position, const std::string& /*lastToken*/, const Json::exception& error)
    {
        m_error = error.what();
        m_errorOffset = static_cast<qint64>(position);
        return false;
    }

    const std::string& error() const { return m_error; }
    qint64 errorOffset() const { return m_errorOffset; }

private:
    Json* place(Json&& value)
    {
        if (m_open.empty()) {
            m_root = std::move(value);
            return &m_root;
        }
        Json* parent = m_open.back();
        if (parent->is_array()) {
            parent->push_back(std::move(value));
            return &parent->back();
        }
        // operator[] keeps the position of the first occurrence of a duplicate key, and the
        // assignment keeps the last value.
        Json& slot = (*parent)[m_key];
        slot = std::move(value);
        return &slot;
    }

    bool addValue(Json&& value)
    {
        place(std::move(value));
        return true;
    }

    // Only the innermost open container grows, so the pointers to the outer ones stay valid.
    bool openContainer(Json&& container)
    {
        m_open.push_back(place(std::move(container)));
        return true;
    }

    bool closeContainer()
    {
        m_open.pop_back();
        return true;
    }

    Json& m_root;
    std::vector<Json*> m_open;
    Json::string_t m_key;
    std::string m_error;
    qint64 m_errorOffset = -1;
};

void appendIndent(std::string& out, int depth)
{
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

// Python's json.encoder with ensure_ascii=False: escape '"', '\\' and the C0 control
// characters, write everything else as it is.
void appendString(std::string& out, const std::string& text)
{
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (byte) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (byte < 0x20) {
                out += "\\u00";
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 0x0F]);
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
}

void appendValue(std::string& out, const Json& value, int depth)
{
    switch (value.type()) {
    case Json::value_t::null:
        out += "null";
        return;
    case Json::value_t::boolean:
        out += value.get<bool>() ? "true" : "false";
        return;
    case Json::value_t::number_integer:
        out += std::to_string(value.get<Json::number_integer_t>());
        return;
    case Json::value_t::number_unsigned:
        out += std::to_string(value.get<Json::number_unsigned_t>());
        return;
    case Json::value_t::number_float:
        out += pythonFloatRepr(value.get<Json::number_float_t>());
        return;
    case Json::value_t::string:
        appendString(out, value.get_ref<const Json::string_t&>());
        return;
    case Json::value_t::array: {
        if (value.empty()) {
            out += "[]";
            return;
        }
        out.push_back('[');
        bool first = true;
        for (const Json& element : value) {
            if (!first)
                out.push_back(',');
            first = false;
            out.push_back('\n');
            appendIndent(out, depth + 1);
            appendValue(out, element, depth + 1);
        }
        out.push_back('\n');
        appendIndent(out, depth);
        out.push_back(']');
        return;
    }
    case Json::value_t::object: {
        if (value.empty()) {
            out += "{}";
            return;
        }
        out.push_back('{');
        bool first = true;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!first)
                out.push_back(',');
            first = false;
            out.push_back('\n');
            appendIndent(out, depth + 1);
            appendString(out, it.key());
            out += ": ";
            appendValue(out, it.value(), depth + 1);
        }
        out.push_back('\n');
        appendIndent(out, depth);
        out.push_back('}');
        return;
    }
    case Json::value_t::binary:
    case Json::value_t::discarded:
        break;
    }
    throw std::logic_error("value has no JSON representation");
}

} // namespace

JsonParseResult parseJson(const QByteArray& bytes)
{
    JsonParseResult result;
    DomBuilder builder(result.value);
    const char* begin = bytes.constData();
    const char* end = begin + bytes.size();
    bool ok = false;
    try {
        ok = Json::sax_parse(begin, end, &builder);
    } catch (const Json::exception& error) {
        // The SAX parser reports errors through DomBuilder::parse_error; this is a safety net.
        result.error = QString::fromUtf8(error.what());
    }
    if (!ok) {
        result.value = Json();
        if (result.error.isEmpty())
            result.error = QString::fromStdString(builder.error());
        result.byteOffset = builder.errorOffset();
        return result;
    }
    result.ok = true;
    return result;
}

QByteArray serializeJson(const Json& value)
{
    std::string out;
    appendValue(out, value, 0);
    return QByteArray(out.data(), static_cast<qsizetype>(out.size()));
}

std::string pythonFloatRepr(double value)
{
    if (std::isnan(value))
        return "NaN";
    if (std::isinf(value))
        return value < 0 ? "-Infinity" : "Infinity";

    // Shortest round-trip digits in scientific notation, e.g. "-1.205e+02".
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    const std::string_view text(buffer, static_cast<std::size_t>(converted.ptr - buffer));
    const std::size_t exponentMark = text.find('e');

    std::string out;
    std::string_view mantissa = text.substr(0, exponentMark);
    if (!mantissa.empty() && mantissa.front() == '-') {
        out.push_back('-');
        mantissa.remove_prefix(1);
    }
    std::string digits;
    for (const char ch : mantissa) {
        if (ch != '.')
            digits.push_back(ch);
    }
    std::string_view exponentText = text.substr(exponentMark + 1);
    const bool negativeExponent = exponentText.front() == '-';
    exponentText.remove_prefix(1);  // the sign; to_chars always writes one
    int exponent = 0;
    std::from_chars(exponentText.data(), exponentText.data() + exponentText.size(), exponent);
    if (negativeExponent)
        exponent = -exponent;

    // Python's float_repr_style 'short' (Python/pystrtod.c, format_float_short with mode 'r'):
    // the value is 0.<digits> * 10^decimalPoint; exponent notation when decimalPoint <= -4
    // or decimalPoint > 16, otherwise fixed notation with at least one digit after the point.
    const int decimalPoint = exponent + 1;
    const int digitCount = static_cast<int>(digits.size());
    if (decimalPoint <= -4 || decimalPoint > 16) {
        out.push_back(digits.front());
        if (digitCount > 1) {
            out.push_back('.');
            out.append(digits, 1, std::string::npos);
        }
        out.push_back('e');
        out.push_back(exponent < 0 ? '-' : '+');
        const int magnitude = exponent < 0 ? -exponent : exponent;
        if (magnitude < 10)
            out.push_back('0');
        out += std::to_string(magnitude);
    } else if (decimalPoint <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-decimalPoint), '0');
        out += digits;
    } else if (decimalPoint >= digitCount) {
        out += digits;
        out.append(static_cast<std::size_t>(decimalPoint - digitCount), '0');
        out += ".0";
    } else {
        out.append(digits, 0, static_cast<std::size_t>(decimalPoint));
        out.push_back('.');
        out.append(digits, static_cast<std::size_t>(decimalPoint), std::string::npos);
    }
    return out;
}

bool jsonValuesEqual(const Json& a, const Json& b)
{
    if (a.is_number() && b.is_number())
        return a.get<double>() == b.get<double>();
    if (a.type() != b.type())
        return false;
    switch (a.type()) {
    case Json::value_t::object:
        if (a.size() != b.size())
            return false;
        for (auto it = a.begin(); it != a.end(); ++it) {
            const auto other = b.find(it.key());
            if (other == b.end() || !jsonValuesEqual(it.value(), *other))
                return false;
        }
        return true;
    case Json::value_t::array:
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!jsonValuesEqual(a[i], b[i]))
                return false;
        }
        return true;
    default:
        return a == b;
    }
}

} // namespace liteanno::formats
