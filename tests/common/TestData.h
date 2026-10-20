#pragma once

#include "formats/JsonText.h"

#include <QFile>
#include <QString>
#include <QTest>

namespace liteanno::test {

inline QString testDataPath(const QString& relativePath)
{
    return QStringLiteral(LITEANNO_TEST_DATA_DIR "/") + relativePath;
}

inline QByteArray readTestData(const QString& relativePath)
{
    QFile file(testDataPath(relativePath));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("cannot open test data %s", qPrintable(relativePath));
    return file.readAll();
}

// Parses JSON with nlohmann's own parser, to state expectations independently of the
// parser under test.
inline formats::Json json(const char* text)
{
    return formats::Json::parse(text);
}

inline formats::Json parseOrFail(const QByteArray& bytes)
{
    const formats::JsonParseResult parsed = formats::parseJson(bytes);
    if (!parsed.ok)
        qFatal("output is not valid JSON: %s", qPrintable(parsed.error));
    return parsed.value;
}

inline QByteArray describe(const formats::Json& value)
{
    return formats::serializeJson(value);
}

} // namespace liteanno::test

namespace QTest {

// Lets QCOMPARE print JSON values.
template <>
inline char* toString(const liteanno::formats::Json& value)
{
    return qstrdup(liteanno::formats::serializeJson(value).constData());
}

} // namespace QTest

// Compares JSON values as format spec section 1 defines it and prints both on failure.
#define LITEANNO_COMPARE_JSON(actual, expected)                                                          \
    do {                                                                                                 \
        const auto liteannoActual = (actual);                                                           \
        const auto liteannoExpected = (expected);                                                       \
        QVERIFY2(liteanno::formats::jsonValuesEqual(liteannoActual, liteannoExpected),                  \
                 ("\nactual:\n" + liteanno::test::describe(liteannoActual) + "\nexpected:\n"            \
                  + liteanno::test::describe(liteannoExpected)).constData());                           \
    } while (false)
