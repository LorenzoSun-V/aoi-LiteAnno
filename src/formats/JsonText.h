#pragma once

// JSON parsing and the exact output format of every JSON file this program writes.
// Uses nlohmann/json: include this header only inside formats and its tests.

#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QString>

#include <string>

namespace liteanno::formats {

// Keeps object members in file order, so untouched content is written back in place.
using Json = nlohmann::ordered_json;

struct JsonParseResult {
    Json value;
    bool ok = false;
    QString error;           // empty when ok
    qint64 byteOffset = -1;  // where parsing stopped, when known
};

// Parses strict JSON (RFC 8259). Accepts a UTF-8 BOM and any JSON whitespace, including CRLF.
// A duplicate key keeps its first position and its last value, as Python's json module does.
// Rejects NaN and Infinity, numbers outside the double range and integers that do not fit
// in 64 bits (format spec 13.5).
JsonParseResult parseJson(const QByteArray& bytes);

// Serializes exactly like Python's json.dumps(value, ensure_ascii=False, indent=2), encoded
// as UTF-8 without BOM and without a trailing newline (format spec 13.5).
QByteArray serializeJson(const Json& value);
// The same with json.dumps(value, ensure_ascii=False, indent=indent), as if `value` were
// nested `depth` levels deep: lines after the first are indented by depth * indent more.
QByteArray serializeJson(const Json& value, int indent, int depth);

// The shortest representation of a double that reads back to the same value, written the way
// Python's repr(float) writes it: "80.0", "0.0001", "1e-05", "1e+16".
std::string pythonFloatRepr(double value);

// JSON value equality as defined by format spec section 1: objects compare as sets of
// members, arrays element by element, numbers as doubles.
bool jsonValuesEqual(const Json& a, const Json& b);

// The major version of a "major.minor" format version string, or -1 for any other form
// (format spec 13.5).
int formatMajorVersion(const std::string& text);

} // namespace liteanno::formats
