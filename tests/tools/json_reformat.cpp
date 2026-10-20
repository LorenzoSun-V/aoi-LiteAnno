// Reads a JSON file with formats::parseJson and writes it with formats::serializeJson.
// Used by tests/python/check_byte_format.py to compare the output with Python's json module.

#include "formats/JsonText.h"

#include <QFile>

#include <cstdio>

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.json> <output.json>\n", argv[0]);
        return 2;
    }

    QFile input(QString::fromLocal8Bit(argv[1]));
    if (!input.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], qPrintable(input.errorString()));
        return 1;
    }
    const liteanno::formats::JsonParseResult parsed = liteanno::formats::parseJson(input.readAll());
    if (!parsed.ok) {
        std::fprintf(stderr, "%s: %s\n", argv[1], qPrintable(parsed.error));
        return 1;
    }

    QFile output(QString::fromLocal8Bit(argv[2]));
    const QByteArray bytes = liteanno::formats::serializeJson(parsed.value);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) || output.write(bytes) != bytes.size()) {
        std::fprintf(stderr, "%s: %s\n", argv[2], qPrintable(output.errorString()));
        return 1;
    }
    return 0;
}
