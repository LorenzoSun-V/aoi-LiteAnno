// liteanno/project.json (format spec section 6): a valid file is read completely, and any
// invalid field rejects the file with the field named.

#include "formats/ProjectFile.h"

#include <QTest>

using namespace liteanno;
using namespace liteanno::formats;

namespace {

// The example of format spec section 6.
const char kSpecExample[] = R"({
  "formatVersion": "1.0",
  "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33",
  "name": "A 线外观缺陷 2026-10",
  "createdAt": "2026-10-20T14:00:00+08:00",
  "shapeTypes": ["rectangle", "polygon"],
  "classes": [
    {"id": 0, "name": "scratch", "color": "#E53935"},
    {"id": 1, "name": "dent", "color": "#1E88E5"}
  ],
  "imageLabels": {"mode": "single", "options": ["OK", "NG"]},
  "attributes": {
    "scratch": [{"name": "severity", "options": ["轻", "重"], "required": true}]
  },
  "batchSize": 1000
})";

} // namespace

class TestProjectFile : public QObject
{
    Q_OBJECT

private slots:
    void specExampleIsRead()
    {
        const ProjectFileResult result = parseProjectFile(QByteArray(kSpecExample));
        QVERIFY2(result.config, qPrintable(result.field + QLatin1Char(' ') + result.error));
        const ProjectConfig& config = *result.config;
        QCOMPARE(config.formatVersion, QStringLiteral("1.0"));
        QCOMPARE(config.projectId, QStringLiteral("3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"));
        QCOMPARE(config.name, QStringLiteral("A 线外观缺陷 2026-10"));
        QCOMPARE(config.shapeTypes, (QList<ShapeType>{ShapeType::Rectangle, ShapeType::Polygon}));
        QCOMPARE(config.classes.size(), 2);
        QCOMPARE(config.classes[1].id, 1);
        QCOMPARE(config.classes[1].name, QStringLiteral("dent"));
        QCOMPARE(config.classes[1].color, QStringLiteral("#1E88E5"));
        QCOMPARE(config.imageLabels.mode, ImageLabelMode::Single);
        QCOMPARE(config.imageLabels.options, (QStringList{QStringLiteral("OK"), QStringLiteral("NG")}));
        const QList<AttributeDef> attributes = config.attributes.value(QStringLiteral("scratch"));
        QCOMPARE(attributes.size(), 1);
        QCOMPARE(attributes[0].name, QStringLiteral("severity"));
        QCOMPARE(attributes[0].options, (QStringList{QStringLiteral("轻"), QStringLiteral("重")}));
        QVERIFY(attributes[0].required);
        QCOMPARE(config.batchSize, 1000);
    }

    void optionalFieldsHaveDefaults()
    {
        const ProjectFileResult result = parseProjectFile(
            R"({"formatVersion": "1.0", "projectId": "3F6C2A9E-8B1D-4C57-9A0E-2D4B7F1E6C33", "unknown": [1]})");
        QVERIFY2(result.config, qPrintable(result.error));
        QCOMPARE(result.config->projectId, QStringLiteral("3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"));
        QVERIFY(result.config->shapeTypes.isEmpty());
        QVERIFY(result.config->classes.isEmpty());
        QVERIFY(result.config->imageLabels.options.isEmpty());
        QCOMPARE(result.config->batchSize, 1000);
    }

    void invalidFieldIsNamed_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("field");
        const QByteArray id = R"("formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33")";
        const auto with = [&id](const char* rest) { return "{" + id + ", " + QByteArray(rest) + "}"; };

        QTest::newRow("not JSON") << QByteArray("{") << QString();
        QTest::newRow("not an object") << QByteArray("[]") << QString();
        QTest::newRow("version missing") << QByteArray(R"({"projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"})")
                                         << QStringLiteral("/formatVersion");
        QTest::newRow("version form") << QByteArray(R"({"formatVersion": "1", "projectId": "x"})")
                                      << QStringLiteral("/formatVersion");
        QTest::newRow("newer version") << QByteArray(R"({"formatVersion": "2.0", "projectId": "x"})")
                                       << QStringLiteral("/formatVersion");
        QTest::newRow("id missing") << QByteArray(R"({"formatVersion": "1.0"})") << QStringLiteral("/projectId");
        QTest::newRow("id not a UUID") << QByteArray(R"({"formatVersion": "1.0", "projectId": "lineA"})")
                                       << QStringLiteral("/projectId");
        QTest::newRow("createdAt without zone") << with(R"("createdAt": "2026-10-20T14:00:00")")
                                                << QStringLiteral("/createdAt");
        QTest::newRow("shape type unknown") << with(R"("shapeTypes": ["rectangle", "box"])")
                                            << QStringLiteral("/shapeTypes/1");
        QTest::newRow("shape type not editable") << with(R"("shapeTypes": ["cuboid"])")
                                                 << QStringLiteral("/shapeTypes/0");
        QTest::newRow("shape type twice") << with(R"("shapeTypes": ["polygon", "polygon"])")
                                          << QStringLiteral("/shapeTypes/1");
        QTest::newRow("class id negative") << with(R"("classes": [{"id": -1, "name": "a"}])")
                                           << QStringLiteral("/classes/0/id");
        QTest::newRow("class id fraction") << with(R"("classes": [{"id": 1.5, "name": "a"}])")
                                           << QStringLiteral("/classes/0/id");
        QTest::newRow("class id twice") << with(R"("classes": [{"id": 0, "name": "a"}, {"id": 0, "name": "b"}])")
                                        << QStringLiteral("/classes/1/id");
        QTest::newRow("class name empty") << with(R"("classes": [{"id": 0, "name": ""}])")
                                          << QStringLiteral("/classes/0/name");
        QTest::newRow("class name twice") << with(R"("classes": [{"id": 0, "name": "a"}, {"id": 1, "name": "a"}])")
                                          << QStringLiteral("/classes/1/name");
        QTest::newRow("class color") << with(R"("classes": [{"id": 0, "name": "a", "color": "red"}])")
                                     << QStringLiteral("/classes/0/color");
        QTest::newRow("image label mode") << with(R"("imageLabels": {"mode": "one", "options": ["OK"]})")
                                          << QStringLiteral("/imageLabels/mode");
        QTest::newRow("image label options") << with(R"("imageLabels": {"mode": "multi", "options": "OK"})")
                                             << QStringLiteral("/imageLabels/options");
        QTest::newRow("attribute name missing") << with(R"("attributes": {"a/b": [{"options": []}]})")
                                                << QStringLiteral("/attributes/a~1b/0/name");
        QTest::newRow("attribute required") << with(R"("attributes": {"a": [{"name": "s", "required": 1}]})")
                                            << QStringLiteral("/attributes/a/0/required");
        QTest::newRow("batch size zero") << with(R"("batchSize": 0)") << QStringLiteral("/batchSize");
        QTest::newRow("batch size text") << with(R"("batchSize": "1000")") << QStringLiteral("/batchSize");
    }

    void invalidFieldIsNamed()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, field);
        const ProjectFileResult result = parseProjectFile(json);
        QVERIFY(!result.config);
        QCOMPARE(result.field, field);
        QVERIFY(!result.error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestProjectFile)
#include "tst_projectfile.moc"
