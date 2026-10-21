// Output byte format and strict parsing (format spec section 1 and 13.5).

#include "common/TestData.h"
#include "formats/JsonText.h"

#include <QTest>

using namespace liteanno;
using namespace liteanno::formats;
using liteanno::test::json;

class TestJsonText : public QObject
{
    Q_OBJECT

private slots:
    void floatRepr_data()
    {
        QTest::addColumn<double>("value");
        QTest::addColumn<QByteArray>("expected");
        // Expected strings are Python's repr() of the same doubles.
        const struct {
            double value;
            const char* repr;
        } cases[] = {
            {0.0, "0.0"},
            {-0.0, "-0.0"},
            {80.0, "80.0"},
            {120.5, "120.5"},
            {0.1, "0.1"},
            {1e-05, "1e-05"},
            {0.0001, "0.0001"},
            {1e+16, "1e+16"},
            {1000000000000000.0, "1000000000000000.0"},
            {1.2345678901234568e+17, "1.2345678901234568e+17"},
            {5e-324, "5e-324"},
            {1.7976931348623157e+308, "1.7976931348623157e+308"},
            {9007199254740992.0, "9007199254740992.0"},
            {0.1 + 0.2, "0.30000000000000004"},
            {1e+22, "1e+22"},
            {1.5e-07, "1.5e-07"},
            {100.0, "100.0"},
            {-3.25, "-3.25"},
            {9999999999999998.0, "9999999999999998.0"},
            {1e+100, "1e+100"},
            {2.2250738585072014e-308, "2.2250738585072014e-308"},
            {0.00012345, "0.00012345"},
            {1234567890123456.8, "1234567890123456.8"},
        };
        for (const auto& entry : cases)
            QTest::newRow(entry.repr) << entry.value << QByteArray(entry.repr);
    }

    void floatRepr()
    {
        QFETCH(double, value);
        QFETCH(QByteArray, expected);
        QCOMPARE(QByteArray::fromStdString(pythonFloatRepr(value)), expected);
    }

    // json.dumps(value, ensure_ascii=False, indent=2)
    void layout()
    {
        const Json value = json(R"({"a": [], "b": {}, "c": [1, 2.5, [true, null]], "d": {"e": "f"}})");
        const QByteArray expected = "{\n"
                                    "  \"a\": [],\n"
                                    "  \"b\": {},\n"
                                    "  \"c\": [\n"
                                    "    1,\n"
                                    "    2.5,\n"
                                    "    [\n"
                                    "      true,\n"
                                    "      null\n"
                                    "    ]\n"
                                    "  ],\n"
                                    "  \"d\": {\n"
                                    "    \"e\": \"f\"\n"
                                    "  }\n"
                                    "}";
        QCOMPARE(serializeJson(value), expected);
        QCOMPARE(serializeJson(json("[]")), QByteArray("[]"));
        QCOMPARE(serializeJson(json("\"x\"")), QByteArray("\"x\""));
    }

    void integersKeepTheirForm()
    {
        QCOMPARE(serializeJson(json("[80, 80.0, -0, 18446744073709551615, -9223372036854775808]")),
                 QByteArray("[\n  80,\n  80.0,\n  0,\n  18446744073709551615,\n  -9223372036854775808\n]"));
    }

    // ensure_ascii=False: non-ASCII text is written as UTF-8, only '"', '\\' and C0 control
    // characters are escaped.
    void stringEscapes()
    {
        Json value = std::string("\"\\/\b\f\n\r\t\x01\x1f\x7f");
        // A plain literal held outside QCOMPARE: MSVC's preprocessor mangles a raw string with
        // backslashes when QCOMPARE turns its arguments into text.
        const QByteArray expected("\"\\\"\\\\/\\b\\f\\n\\r\\t\\u0001\\u001f\x7f\"");
        QCOMPARE(serializeJson(value), expected);
        value = std::string("\xE5\x88\x92\xE4\xBC\xA4 \xF0\x9F\x98\x80");  // "划伤 😀"
        QCOMPARE(serializeJson(value), QByteArray("\"\xE5\x88\x92\xE4\xBC\xA4 \xF0\x9F\x98\x80\""));
        const Json escapedKey = json(R"({"a\nb": 1})");
        QCOMPARE(serializeJson(escapedKey), QByteArray("{\n  \"a\\nb\": 1\n}"));
    }

    void keyOrderIsKept()
    {
        const JsonParseResult parsed = parseJson(R"({"z": 1, "a": 2, "m": 3})");
        QVERIFY(parsed.ok);
        QCOMPARE(serializeJson(parsed.value), QByteArray("{\n  \"z\": 1,\n  \"a\": 2,\n  \"m\": 3\n}"));
    }

    // Python's json module: the first position, the last value.
    void duplicateKeys()
    {
        const JsonParseResult parsed = parseJson(R"({"a": 1, "b": 2, "a": {"c": 3}})");
        QVERIFY(parsed.ok);
        QCOMPARE(serializeJson(parsed.value), QByteArray("{\n  \"a\": {\n    \"c\": 3\n  },\n  \"b\": 2\n}"));
    }

    void acceptsBomAndCrlf()
    {
        const JsonParseResult parsed = parseJson("\xEF\xBB\xBF{\r\n  \"a\": [1,\r\n 2]\r\n}\r\n");
        QVERIFY2(parsed.ok, qPrintable(parsed.error));
        LITEANNO_COMPARE_JSON(parsed.value, json(R"({"a": [1, 2]})"));
    }

    void decodesEscapes()
    {
        const JsonParseResult parsed = parseJson(R"(["划伤", "😀", "\u0000"])");
        QVERIFY(parsed.ok);
        QCOMPARE(parsed.value[0].get<std::string>(), std::string("\xE5\x88\x92\xE4\xBC\xA4"));
        QCOMPARE(parsed.value[1].get<std::string>(), std::string("\xF0\x9F\x98\x80"));
        QCOMPARE(parsed.value[2].get<std::string>(), std::string(1, '\0'));
    }

    void rejectsWhatStrictJsonRejects_data()
    {
        QTest::addColumn<QString>("file");
        for (const char* name : {"nan_coordinate.json", "infinity.json", "overflow_float.json", "huge_integer.json",
                                 "invalid_utf8.json", "lone_surrogate.json", "trailing_comma.json",
                                 "truncated.json", "empty.json", "nul_suffix.json", "nul_in_string.json"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void rejectsWhatStrictJsonRejects()
    {
        QFETCH(QString, file);
        const JsonParseResult parsed = parseJson(test::readTestData(QStringLiteral("annotations/broken/") + file));
        QVERIFY(!parsed.ok);
        QVERIFY(!parsed.error.isEmpty());
    }

    void reportsWhereParsingStopped()
    {
        const JsonParseResult parsed = parseJson(R"({"a": [1, 2,]})");
        QVERIFY(!parsed.ok);
        QCOMPARE(parsed.byteOffset, qint64(13));
    }

    // Canonical files (written by Python like upstream does) come out byte for byte.
    void canonicalFilesRoundTripExactly_data()
    {
        QTest::addColumn<QString>("file");
        for (const char* name : {"upstream_v4.json", "upstream_v2_legacy.json", "embedded_image_data.json",
                                 "missing_fields.json", "unknown_fields.json", "unrecognized_objects.json"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void canonicalFilesRoundTripExactly()
    {
        QFETCH(QString, file);
        const QByteArray bytes = test::readTestData(QStringLiteral("annotations/") + file);
        QCOMPARE(serializeJson(test::parseOrFail(bytes)), bytes);
    }

    // Format spec section 1: members compare as sets, numbers as doubles.
    void valueEquality()
    {
        QVERIFY(jsonValuesEqual(json(R"({"a": 1, "b": [1.0, 2]})"), json(R"({"b": [1, 2.0], "a": 1.0})")));
        QVERIFY(!jsonValuesEqual(json(R"({"a": 1})"), json(R"({"a": 1, "b": null})")));
        QVERIFY(!jsonValuesEqual(json("[1, 2]"), json("[2, 1]")));
        QVERIFY(!jsonValuesEqual(json(R"({"a": 1})"), json(R"({"a": true})")));
        QVERIFY(!jsonValuesEqual(json(R"("1")"), json("1")));
    }
};

QTEST_GUILESS_MAIN(TestJsonText)
#include "tst_jsontext.moc"
