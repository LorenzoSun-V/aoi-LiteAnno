// Saving per-image annotation files: value round trip, save normalizations (format spec 4.5),
// image-level labels (13.1) and invalid known fields (13.3).

#include "common/TestData.h"
#include "formats/AnnotationFile.h"

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace liteanno;
using namespace liteanno::formats;
using liteanno::test::json;

namespace {

ImageInfo imageInfo(const char* fileName, int width, int height)
{
    return ImageInfo{QString::fromUtf8(fileName), width, height, 1};
}

QByteArray fixture(const char* name)
{
    return test::readTestData(QStringLiteral("annotations/") + QLatin1String(name));
}

Json saved(const AnnotationFile& file)
{
    return test::parseOrFail(file.toBytes());
}

qsizetype firstEditableShape(const AnnotationDocument& document)
{
    const auto& shapes = document.shapes();
    const auto it = std::find_if(shapes.begin(), shapes.end(),
                                 [](const Shape& shape) { return shape.kind == ShapeKind::Editable; });
    return it == shapes.end() ? -1 : it - shapes.begin();
}

bool isPositiveInteger(const Json& value)
{
    return value.is_number() && value.get<double>() >= 1 && value.get<double>() == static_cast<qint64>(value.get<double>());
}

bool isReadableRectangle(const Json& shape)
{
    if (!shape.is_object() || shape.value("shape_type", Json()) != "rectangle" || !shape.contains("label")
        || !shape["label"].is_string() || !shape.contains("points") || !shape["points"].is_array()) {
        return false;
    }
    const Json& points = shape["points"];
    return (points.size() == 2 || points.size() == 4)
        && std::all_of(points.begin(), points.end(), [](const Json& point) {
               return point.is_array() && point.size() == 2 && point[0].is_number() && point[1].is_number();
           });
}

bool hasReadablePoints(const Json& shape)
{
    return shape.contains("points") && shape["points"].is_array() && !shape["points"].empty()
        && std::all_of(shape["points"].begin(), shape["points"].end(), [](const Json& point) {
               return point.is_array() && point.size() == 2 && point[0].is_number() && point[1].is_number();
           });
}

// What format spec 4.5 changes in any saved file, stated independently of the writer.
Json expectedNormalizations(Json value, const ImageInfo& image)
{
    value["version"] = "0.1.0";
    if (!value.contains("flags"))
        value["flags"] = Json::object();
    if (!value.contains("checked"))
        value["checked"] = false;
    if (!value.contains("shapes"))
        value["shapes"] = Json::array();
    if (!value.contains("imagePath") || value["imagePath"] != image.fileName.toStdString())
        value["imagePath"] = image.fileName.toStdString();
    if (!value.contains("imageData"))
        value["imageData"] = nullptr;
    if (!value.contains("imageHeight") || !isPositiveInteger(value["imageHeight"]))
        value["imageHeight"] = image.height;
    if (!value.contains("imageWidth") || !isPositiveInteger(value["imageWidth"]))
        value["imageWidth"] = image.width;
    if (!value.contains("liteanno") || !value["liteanno"].is_object())
        value["liteanno"] = Json::object();
    value["liteanno"]["formatVersion"] = "1.0";
    for (Json& shape : value["shapes"]) {
        // A shape read as a polygon because its shape_type is missing gets the required key
        // (13.5); an existing null stays (13.3).
        if (shape.is_object() && shape.contains("label") && shape["label"].is_string() && hasReadablePoints(shape)
            && !shape.contains("shape_type")) {
            shape["shape_type"] = "polygon";
        }
        if (!isReadableRectangle(shape))
            continue;
        double xmin = shape["points"][0][0].get<double>();
        double xmax = xmin;
        double ymin = shape["points"][0][1].get<double>();
        double ymax = ymin;
        for (const Json& point : shape["points"]) {
            xmin = std::min(xmin, point[0].get<double>());
            xmax = std::max(xmax, point[0].get<double>());
            ymin = std::min(ymin, point[1].get<double>());
            ymax = std::max(ymax, point[1].get<double>());
        }
        shape["points"] = Json::array({Json::array({xmin, ymin}), Json::array({xmax, ymin}),
                                       Json::array({xmax, ymax}), Json::array({xmin, ymax})});
    }
    return value;
}

std::vector<std::string> keys(const Json& object)
{
    std::vector<std::string> result;
    for (auto it = object.begin(); it != object.end(); ++it)
        result.push_back(it.key());
    return result;
}

} // namespace

class TestAnnotationWrite : public QObject
{
    Q_OBJECT

private slots:
    void valueRoundTrip_data()
    {
        QTest::addColumn<QByteArray>("file");
        QTest::addColumn<QString>("imageName");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::newRow("upstream 4.x") << QByteArray("upstream_v4.json") << "0001.jpg" << 768 << 768;
        QTest::newRow("upstream 2.x") << QByteArray("upstream_v2_legacy.json") << "0002.jpg" << 640 << 480;
        QTest::newRow("embedded imageData") << QByteArray("embedded_image_data.json") << "0003.png" << 1 << 1;
        QTest::newRow("missing fields") << QByteArray("missing_fields.json") << "0004.png" << 10 << 10;
        QTest::newRow("unknown fields") << QByteArray("unknown_fields.json") << "0005.bmp" << 768 << 768;
        QTest::newRow("unrecognized objects") << QByteArray("unrecognized_objects.json") << "0006.jpg" << 100 << 100;
        QTest::newRow("newer minor version") << QByteArray("minor_version.json") << "0008.png" << 100 << 100;
        QTest::newRow("invalid checked") << QByteArray("checked_invalid.json") << "0010.png" << 100 << 100;
        QTest::newRow("skipped") << QByteArray("skipped.json") << "0011.png" << 100 << 100;
        QTest::newRow("state conflict") << QByteArray("state_conflict.json") << "0012.png" << 100 << 100;
        QTest::newRow("unknown status") << QByteArray("unknown_status.json") << "0013.png" << 100 << 100;
        QTest::newRow("BOM and CRLF") << QByteArray("bom_crlf.json") << "0001.jpg" << 768 << 768;
        QTest::newRow("compact") << QByteArray("compact.json") << "0001.jpg" << 768 << 768;
        QTest::newRow("escaped text") << QByteArray("chinese_text.json") << "样本 01.png" << 64 << 64;
    }

    // Format spec section 1: after an edit, everything the edit did not touch keeps its JSON
    // value, apart from the save normalizations of 4.5.
    void valueRoundTrip()
    {
        QFETCH(QByteArray, file);
        QFETCH(QString, imageName);
        QFETCH(int, width);
        QFETCH(int, height);
        const ImageInfo image{imageName, width, height, 1};
        const QByteArray original = fixture(file.constData());

        AnnotationFile annotation = AnnotationFile::fromBytes(original, image);
        QVERIFY(!annotation.isReadOnly());
        const qsizetype edited = firstEditableShape(annotation.document());
        QVERIFY(edited >= 0);
        QVERIFY(annotation.document().setShapeDescription(edited, QStringLiteral("round trip")));
        QVERIFY(annotation.needsWrite());

        Json expected = expectedNormalizations(test::parseOrFail(original), image);
        expected["shapes"][static_cast<std::size_t>(edited)]["description"] = "round trip";
        LITEANNO_COMPARE_JSON(saved(annotation), expected);
    }

    void unknownFieldsSurviveContentEdits()
    {
        const ImageInfo image = imageInfo("0005.bmp", 768, 768);
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("unknown_fields.json"), image);
        AnnotationDocument& document = annotation.document();
        QVERIFY(document.setShapePoints(0, {QPointF(11, 21), QPointF(31, 41)}));
        QVERIFY(document.setImageLabel(QStringLiteral("OK"), true));
        QVERIFY(document.setImageLabel(QStringLiteral("NG"), false));

        Json expected = expectedNormalizations(test::parseOrFail(fixture("unknown_fields.json")), image);
        expected["shapes"][0]["points"] = json("[[11.0, 21.0], [31.0, 21.0], [31.0, 41.0], [11.0, 41.0]]");
        expected["flags"] = json(R"({"OK": true, "NG": false, "legacy_tag": true})");
        expected["checked"] = false;
        LITEANNO_COMPARE_JSON(saved(annotation), expected);
    }

    void unrecognizedObjectsAreKeptExactly()
    {
        const QByteArray original = fixture("unrecognized_objects.json");
        AnnotationFile annotation = AnnotationFile::fromBytes(original, imageInfo("0006.jpg", 100, 100));
        QVERIFY(annotation.document().setShapeLabel(0, QStringLiteral("renamed")));

        const Json before = test::parseOrFail(original);
        const Json after = saved(annotation);
        QCOMPARE(after["shapes"].size(), before["shapes"].size());
        for (std::size_t i = 1; i < before["shapes"].size(); ++i) {
            // Same value and the same text, number forms included. Shape 12 is a polygon with
            // shape_type null, which stays null (format spec 13.3).
            QCOMPARE(serializeJson(after["shapes"][i]), serializeJson(before["shapes"][i]));
        }
        QCOMPARE(after["shapes"][0]["label"].get<std::string>(), std::string("renamed"));

        // The user can remove them explicitly.
        QVERIFY(annotation.document().removeShape(1));
        QCOMPARE(saved(annotation)["shapes"].size(), before["shapes"].size() - 1);
    }

    // Format spec 4.2, 4.5: shape_type is required. A shape without it is read as a polygon,
    // and a save adds the key after the existing ones.
    void missingShapeTypeIsCompleted()
    {
        const QByteArray original = R"({"shapes": [{"label": "a", "points": [[1, 1], [5, 1], [5, 5]]}],
            "imagePath": "0007.png", "imageData": null, "imageHeight": 10, "imageWidth": 10})";
        AnnotationFile annotation = AnnotationFile::fromBytes(original, imageInfo("0007.png", 10, 10));
        QVERIFY(!annotation.isReadOnly());
        QCOMPARE(annotation.document().shapes().at(0).type, ShapeType::Polygon);
        QVERIFY(annotation.document().setShapeLabel(0, QStringLiteral("b")));

        const Json shape = saved(annotation)["shapes"][0];
        const std::vector<std::string> expectedKeys{"label", "points", "shape_type"};
        QCOMPARE(keys(shape), expectedKeys);
        LITEANNO_COMPARE_JSON(shape, json(R"({"label": "b", "points": [[1, 1], [5, 1], [5, 5]], "shape_type": "polygon"})"));
    }

    // Format spec 4.2: upstream writes the angle of every rotated box. A new one holds the
    // integer 0, as upstream's new boxes do; a turned one its angle.
    void rotatedBoxAngle()
    {
        AnnotationFile annotation = AnnotationFile::createNew(imageInfo("0001.bmp", 100, 100));
        Shape box;
        box.type = ShapeType::Rotation;
        box.label = QStringLiteral("crack");
        box.points = {QPointF(10, 10), QPointF(30, 10), QPointF(30, 20), QPointF(10, 20)};
        QCOMPARE(annotation.document().addShape(box), qsizetype(0));
        Json shape = saved(annotation)["shapes"][0];
        const std::vector<std::string> expectedKeys{"label", "score", "points", "group_id", "description", "difficult",
                                                    "shape_type", "flags", "attributes", "kie_linking", "direction"};
        QCOMPARE(keys(shape), expectedKeys);
        QVERIFY(shape["direction"].is_number_integer());
        QCOMPARE(shape["direction"].get<int>(), 0);

        const RotatedBox turned = rotateBox({box.points, 0}, radians(1.0));
        QVERIFY(annotation.document().setShapeRotation(0, turned));
        shape = saved(annotation)["shapes"][0];
        QVERIFY(shape["direction"].is_number_float());
        QCOMPARE(shape["direction"].get<double>(), turned.direction);

        // Read back, unedited: the same value; a corner edit keeps it.
        AnnotationFile reread = AnnotationFile::fromBytes(annotation.toBytes(), imageInfo("0001.bmp", 100, 100));
        QCOMPARE(reread.document().shapes().at(0).direction, turned.direction);
        QVERIFY(reread.document().setShapePoints(0, moveBoxCorner(turned, 0, QPointF(5, 5))));
        QCOMPARE(saved(reread)["shapes"][0]["direction"].get<double>(), turned.direction);
    }

    // A rotated box without an angle is read with angle 0; editing it adds the key.
    void missingAngleIsCompletedOnEdit()
    {
        const QByteArray original = R"({"shapes": [{"label": "a", "points": [[1, 1], [5, 1], [5, 5], [1, 5]],
            "shape_type": "rotation"}], "imagePath": "0007.png", "imageData": null, "imageHeight": 10, "imageWidth": 10})";
        AnnotationFile annotation = AnnotationFile::fromBytes(original, imageInfo("0007.png", 10, 10));
        QCOMPARE(annotation.document().shapes().at(0).direction, 0.0);
        QVERIFY(annotation.document().setShapeLabel(0, QStringLiteral("b")));
        QVERIFY(!saved(annotation)["shapes"][0].contains("direction"));
        QVERIFY(annotation.document().setShapePoints(0, {QPointF(1, 1), QPointF(6, 1), QPointF(6, 5), QPointF(1, 5)}));
        LITEANNO_COMPARE_JSON(saved(annotation)["shapes"][0]["direction"], json("0.0"));
    }

    // group_id: an integer is the shape's group; another value reads as none and stays as it
    // is until the group is edited. A new shape writes its group.
    void groupId()
    {
        const QByteArray original = R"({"shapes": [
            {"label": "a", "points": [[1, 1], [5, 1], [5, 5]], "group_id": 3, "shape_type": "polygon"},
            {"label": "b", "points": [[1, 1], [5, 1], [5, 5]], "group_id": "x", "shape_type": "polygon"},
            {"label": "c", "points": [[1, 1], [5, 1], [5, 5]], "group_id": 1.5, "shape_type": "polygon"}],
            "imagePath": "0007.png", "imageData": null, "imageHeight": 10, "imageWidth": 10})";
        AnnotationFile annotation = AnnotationFile::fromBytes(original, imageInfo("0007.png", 10, 10));
        AnnotationDocument& document = annotation.document();
        QCOMPARE(document.shapes().at(0).groupId, std::optional<qint64>(3));
        QVERIFY(!document.shapes().at(1).groupId);
        QVERIFY(!document.shapes().at(2).groupId);

        QVERIFY(document.setShapeLabel(1, QStringLiteral("b2")));
        QVERIFY(!document.setShapeGroupId(0, 3));
        QVERIFY(document.setShapeGroupId(2, 7));
        Json shapes = saved(annotation)["shapes"];
        LITEANNO_COMPARE_JSON(shapes[0]["group_id"], json("3"));
        LITEANNO_COMPARE_JSON(shapes[1]["group_id"], json(R"("x")"));
        LITEANNO_COMPARE_JSON(shapes[2]["group_id"], json("7"));

        QVERIFY(document.setShapeGroupId(0, std::nullopt));
        QVERIFY(saved(annotation)["shapes"][0]["group_id"].is_null());

        Shape point;
        point.type = ShapeType::Point;
        point.label = QStringLiteral("nose");
        point.points = {QPointF(2, 2)};
        point.groupId = 4;
        const qsizetype added = document.addShape(point);
        LITEANNO_COMPARE_JSON(saved(annotation)["shapes"][static_cast<std::size_t>(added)]["group_id"], json("4"));

        // A pasted copy keeps a non-integer value; an integer one follows the shape's group.
        Shape copy = document.shapes().at(1);
        copy.copiedObject = annotation.shapeObject(1);
        const qsizetype pasted = document.addShape(copy);
        LITEANNO_COMPARE_JSON(saved(annotation)["shapes"][static_cast<std::size_t>(pasted)]["group_id"], json(R"("x")"));
        Shape regrouped = document.shapes().at(2);
        regrouped.copiedObject = annotation.shapeObject(2);
        regrouped.groupId.reset();
        const qsizetype ungrouped = document.addShape(regrouped);
        QVERIFY(saved(annotation)["shapes"][static_cast<std::size_t>(ungrouped)]["group_id"].is_null());
    }

    // The group numbers of objects this build does not recognize are taken too: a keypoint
    // drawn in an ungrouped rectangle never joins one of them, and they are kept as they are.
    void newGroupSkipsUnrecognizedObjects()
    {
        const QByteArray original = R"({"shapes": [
            {"label": "x", "points": [[1, 1]], "group_id": 0, "shape_type": "custom_type"},
            {"label": "person", "points": [[0, 0], [10, 0], [10, 10], [0, 10]], "group_id": null, "shape_type": "rectangle"}],
            "imagePath": "0007.png", "imageData": null, "imageHeight": 100, "imageWidth": 100})";
        AnnotationFile annotation = AnnotationFile::fromBytes(original, imageInfo("0007.png", 100, 100));
        AnnotationDocument& document = annotation.document();
        QCOMPARE(document.shapes().at(0).kind, ShapeKind::Unrecognized);
        Shape point;
        point.type = ShapeType::Point;
        point.label = QStringLiteral("nose");
        point.points = {QPointF(5, 5)};
        const qsizetype index = document.addKeypoint(point);
        QVERIFY(index >= 0);
        const Json shapes = saved(annotation)["shapes"];
        QCOMPARE(serializeJson(shapes[0]), serializeJson(test::parseOrFail(original)["shapes"][0]));
        QVERIFY(shapes[1]["group_id"].is_number_integer());
        QVERIFY(shapes[1]["group_id"].get<qint64>() != 0);
        LITEANNO_COMPARE_JSON(shapes[static_cast<std::size_t>(index)]["group_id"], shapes[1]["group_id"]);
    }

    // Upstream's paste keeps every field of the copied object; only what the copy changes
    // (label, points, ...) is written anew, and unknown fields come first, as in to_dict().
    void pastedShapeKeepsTheCopiedFields()
    {
        AnnotationFile source = AnnotationFile::fromBytes(fixture("unknown_fields.json"), imageInfo("0005.bmp", 768, 768));
        const qsizetype index = firstEditableShape(source.document());
        QVERIFY(index >= 0);
        const Json original = test::parseOrFail(source.shapeObject(index));

        AnnotationFile target = AnnotationFile::createNew(imageInfo("0009.png", 768, 768));
        Shape copy = source.document().shapes().at(index);
        copy.copiedObject = source.shapeObject(index);
        copy.locked = false;
        QCOMPARE(target.document().addShape(copy), qsizetype(0));
        const Json pasted = saved(target)["shapes"][0];
        for (auto it = original.begin(); it != original.end(); ++it) {
            if (it.key() == "points" || it.key() == "locked")
                continue;
            QVERIFY2(pasted.contains(it.key()), it.key().c_str());
            LITEANNO_COMPARE_JSON(pasted[it.key()], it.value());
        }
        QCOMPARE(pasted["points"].size(), original["points"].size() == 2 ? std::size_t(4) : original["points"].size());
        QVERIFY(!pasted.contains("locked"));
        const std::vector<std::string> order = keys(pasted);
        QCOMPARE(order.at(0), std::string("track_id"));
        QCOMPARE(order.at(1), std::string("model_meta"));
        QCOMPARE(order.at(2), std::string("label"));
    }

    // Format spec 13.3: a null shape_type the edit did not touch keeps its value, also when the
    // edit is to the same shape.
    void nullShapeTypeIsKept()
    {
        const QByteArray original = R"({"shapes": [{"label": "a", "points": [[1, 1], [5, 1], [5, 5]], "shape_type": null}],
            "imagePath": "0007.png", "imageData": null, "imageHeight": 10, "imageWidth": 10})";
        AnnotationFile annotation = AnnotationFile::fromBytes(original, imageInfo("0007.png", 10, 10));
        QCOMPARE(annotation.document().shapes().at(0).type, ShapeType::Polygon);
        QVERIFY(annotation.document().setShapeDescription(0, QStringLiteral("edge")));
        QVERIFY(annotation.document().setImageLabel(QStringLiteral("NG"), true));
        QVERIFY(annotation.document().setShapeLabel(0, QStringLiteral("b")));

        const Json shape = saved(annotation)["shapes"][0];
        QVERIFY(shape.contains("shape_type"));
        QVERIFY(shape["shape_type"].is_null());
        QCOMPARE(shape["label"].get<std::string>(), std::string("b"));
    }

    void embeddedImageDataIsKept()
    {
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("embedded_image_data.json"), imageInfo("0003.png", 1, 1));
        QVERIFY(annotation.document().confirm());
        const Json after = saved(annotation);
        QCOMPARE(after["imageData"], test::parseOrFail(fixture("embedded_image_data.json"))["imageData"]);
    }

    // Format spec 4.5: imageData is always written; null when the file had none.
    void missingImageDataIsWrittenAsNull()
    {
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("empty_object.json"), imageInfo("0004.png", 10, 20));
        QVERIFY(annotation.document().confirm());
        const Json expected = json(R"({
            "shapes": [], "version": "0.1.0", "flags": {}, "checked": true, "imagePath": "0004.png",
            "imageData": null, "imageHeight": 20, "imageWidth": 10, "liteanno": {"formatVersion": "1.0"}})");
        LITEANNO_COMPARE_JSON(saved(annotation), expected);
    }

    void legacyRectanglesBecomeFourPoints()
    {
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("upstream_v2_legacy.json"), imageInfo("0002.jpg", 640, 480));
        QVERIFY(annotation.document().setShapeDescription(1, QStringLiteral("only the polygon is edited")));
        QCOMPARE(saved(annotation)["shapes"][0]["points"],
                 json("[[120.5, 80.0], [260.0, 80.0], [260.0, 150.25], [120.5, 150.25]]"));
    }

    // Untouched numbers keep their form; edited coordinates are written as decimals (13.5).
    void numberForms()
    {
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("missing_fields.json"), imageInfo("0004.png", 10, 10));
        QVERIFY(annotation.document().setShapeDescription(0, QStringLiteral("x")));
        QVERIFY(saved(annotation)["shapes"][0]["points"][0][0].is_number_integer());

        QVERIFY(annotation.document().setShapePoints(0, {QPointF(1, 2), QPointF(4, 5)}));
        const Json points = saved(annotation)["shapes"][0]["points"];
        QVERIFY(points[0][0].is_number_float());
        QVERIFY(annotation.toBytes().contains("4.0,"));
    }

    // Format spec 13.3, "checked": 1.
    void invalidChecked_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<QByteArray>("expectedChecked");
        QTest::newRow("content edit") << "content" << QByteArray("false");
        QTest::newRow("confirm") << "confirm" << QByteArray("true");
        QTest::newRow("skip") << "skip" << QByteArray("false");
        QTest::newRow("description only") << "description" << QByteArray("1");
        QTest::newRow("lock only") << "lock" << QByteArray("1");
    }

    void invalidChecked()
    {
        QFETCH(QString, operation);
        QFETCH(QByteArray, expectedChecked);
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("checked_invalid.json"), imageInfo("0010.png", 100, 100));
        AnnotationDocument& document = annotation.document();
        if (operation == QLatin1String("content"))
            QVERIFY(document.setShapeLabel(0, QStringLiteral("dent")));
        else if (operation == QLatin1String("confirm"))
            QVERIFY(document.confirm());
        else if (operation == QLatin1String("skip"))
            QVERIFY(document.skip(QStringLiteral("图片模糊")));
        else if (operation == QLatin1String("description"))
            QVERIFY(document.setShapeDescription(0, QStringLiteral("边缘")));
        else
            QVERIFY(document.setShapeLocked(0, true));

        const Json after = saved(annotation);
        QCOMPARE(serializeJson(after["checked"]), expectedChecked);
        if (operation == QLatin1String("skip")) {
            QCOMPARE(after["liteanno"]["status"].get<std::string>(), std::string("skipped"));
            QCOMPARE(QString::fromStdString(after["liteanno"]["skipReason"].get<std::string>()), QStringLiteral("图片模糊"));
        }
    }

    // Other invalid known fields: kept until an operation writes them (13.3).
    void otherInvalidFields()
    {
        const QByteArray original = R"({"version": 3, "flags": [], "checked": false, "shapes": [
            {"label": "a", "points": [[1, 1], [2, 2], [3, 1]], "shape_type": "polygon", "description": 7, "group_id": "x"}],
            "imagePath": 12, "imageData": null, "imageHeight": -5, "imageWidth": "640"})";
        const ImageInfo image = imageInfo("a.png", 640, 480);

        AnnotationFile annotation = AnnotationFile::fromBytes(original, image);
        QVERIFY(!annotation.isReadOnly());
        QVERIFY(annotation.document().setShapeLocked(0, true));
        Json after = saved(annotation);
        QCOMPARE(after["flags"], json("[]"));
        QCOMPARE(after["shapes"][0]["description"], json("7"));
        QCOMPARE(after["shapes"][0]["group_id"], json(R"("x")"));
        QCOMPARE(after["shapes"][0]["locked"], json("true"));
        QCOMPARE(after["version"], json(R"("0.1.0")"));
        QCOMPARE(after["imagePath"], json(R"("a.png")"));
        QCOMPARE(after["imageHeight"], json("480"));
        QCOMPARE(after["imageWidth"], json("640"));

        // Writing the field gives it a legal value.
        QVERIFY(annotation.document().setImageLabel(QStringLiteral("OK"), true));
        QVERIFY(annotation.document().setShapeDescription(0, QStringLiteral("edge")));
        after = saved(annotation);
        QCOMPARE(after["flags"], json(R"({"OK": true})"));
        QCOMPARE(after["shapes"][0]["description"], json(R"("edge")"));
    }

    // Format spec 13.1.
    void imageLabels()
    {
        // New files list every configured option, or write {}.
        AnnotationFile configured = AnnotationFile::createNew(imageInfo("a.png", 10, 10),
                                                              {QStringLiteral("OK"), QStringLiteral("NG")});
        QVERIFY(configured.document().confirm());
        QCOMPARE(saved(configured)["flags"], json(R"({"OK": false, "NG": false})"));
        AnnotationFile unconfigured = AnnotationFile::createNew(imageInfo("a.png", 10, 10));
        QVERIFY(unconfigured.document().confirm());
        QCOMPARE(saved(unconfigured)["flags"], json("{}"));

        // Existing keys stay, whatever the project configures.
        const QByteArray original = fixture("unknown_fields.json");
        AnnotationFile shapeEdit = AnnotationFile::fromBytes(original, imageInfo("0005.bmp", 768, 768));
        QVERIFY(shapeEdit.document().setShapeLabel(0, QStringLiteral("dent")));
        QCOMPARE(serializeJson(saved(shapeEdit)["flags"]), serializeJson(test::parseOrFail(original)["flags"]));

        AnnotationFile labelEdit = AnnotationFile::fromBytes(original, imageInfo("0005.bmp", 768, 768));
        QVERIFY(labelEdit.document().setImageLabel(QStringLiteral("OK"), true));
        QVERIFY(labelEdit.document().setImageLabel(QStringLiteral("NG"), false));
        QVERIFY(labelEdit.document().setImageLabel(QStringLiteral("new"), true));
        QCOMPARE(serializeJson(saved(labelEdit)["flags"]),
                 serializeJson(json(R"({"OK": true, "NG": false, "legacy_tag": true, "new": true})")));

        // A non-boolean value stays until the user sets that label.
        const QByteArray invalidValue = R"({"flags": {"OK": "yes", "NG": false}, "shapes": [], "imagePath": "a.png", "imageData": null})";
        AnnotationFile untouched = AnnotationFile::fromBytes(invalidValue, imageInfo("a.png", 10, 10));
        QCOMPARE(untouched.document().imageLabel(QStringLiteral("OK")), std::optional<bool>());
        QVERIFY(untouched.document().setImageLabel(QStringLiteral("NG"), true));
        QCOMPARE(saved(untouched)["flags"], json(R"({"OK": "yes", "NG": true})"));
        QVERIFY(untouched.document().setImageLabel(QStringLiteral("OK"), false));
        QCOMPARE(saved(untouched)["flags"], json(R"({"OK": false, "NG": true})"));
    }

    // Format spec 5.2: how the review state reaches "checked" and "liteanno".
    void reviewState()
    {
        const ImageInfo image = imageInfo("0011.png", 100, 100);

        AnnotationFile confirmSkipped = AnnotationFile::fromBytes(fixture("skipped.json"), image);
        QVERIFY(confirmSkipped.document().confirm());
        Json after = saved(confirmSkipped);
        QCOMPARE(after["checked"], json("true"));
        QCOMPARE(after["liteanno"], json(R"({"formatVersion": "1.0"})"));

        AnnotationFile editSkipped = AnnotationFile::fromBytes(fixture("skipped.json"), image);
        QVERIFY(editSkipped.document().setShapeLabel(0, QStringLiteral("dent")));
        after = saved(editSkipped);
        QCOMPARE(after["checked"], json("false"));
        QCOMPARE(after["liteanno"], json(R"({"formatVersion": "1.0"})"));

        AnnotationFile describeSkipped = AnnotationFile::fromBytes(fixture("skipped.json"), image);
        QVERIFY(describeSkipped.document().setShapeDescription(0, QStringLiteral("边缘")));
        QCOMPARE(saved(describeSkipped)["liteanno"]["status"], json(R"("skipped")"));

        AnnotationFile unknown = AnnotationFile::fromBytes(fixture("unknown_status.json"), imageInfo("0013.png", 100, 100));
        QVERIFY(unknown.document().setShapeLabel(0, QStringLiteral("dent")));
        QCOMPARE(saved(unknown)["liteanno"]["status"], json(R"("review_later")"));
        QVERIFY(unknown.document().confirm());
        QVERIFY(!saved(unknown)["liteanno"].contains("status"));

        AnnotationFile conflict = AnnotationFile::fromBytes(fixture("state_conflict.json"), imageInfo("0012.png", 100, 100));
        QVERIFY(conflict.document().skip(QStringLiteral("重复")));
        after = saved(conflict);
        QCOMPARE(after["checked"], json("false"));
        QCOMPARE(after["liteanno"]["status"], json(R"("skipped")"));
    }

    // Format spec 4.3 and 4.5: the current format version is written; unknown members of
    // "liteanno" are kept.
    void newerMinorVersionIsWrittenAsCurrent()
    {
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("minor_version.json"), imageInfo("0008.png", 100, 100));
        QVERIFY(annotation.document().confirm());
        QCOMPARE(saved(annotation)["liteanno"], json(R"({"formatVersion": "1.0", "reviewRound": 2})"));
    }

    // Format spec 13.5: existing keys keep their order, added keys go to the end.
    void keyOrder()
    {
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("missing_fields.json"), imageInfo("0004.png", 10, 10));
        QVERIFY(annotation.document().setShapeDescription(0, QStringLiteral("x")));
        const std::vector<std::string> expected{"shapes", "imagePath", "imageData", "version", "flags",
                                                "checked", "imageHeight", "imageWidth", "liteanno"};
        QVERIFY(keys(saved(annotation)) == expected);

        AnnotationFile upstream = AnnotationFile::fromBytes(fixture("upstream_v4.json"), imageInfo("0001.jpg", 768, 768));
        QVERIFY(upstream.document().setShapeDescription(0, QStringLiteral("x")));
        const std::vector<std::string> upstreamOrder{"version", "flags", "checked", "shapes", "imagePath", "imageData",
                                                     "imageHeight", "imageWidth", "description", "liteanno"};
        QVERIFY(keys(saved(upstream)) == upstreamOrder);
    }

    // The exact bytes of a new file (format spec 13.5); the Python check confirms that the
    // expected file is what json.dumps writes for the same value.
    void newFileBytes()
    {
        AnnotationFile annotation = AnnotationFile::createNew(imageInfo("0001.bmp", 768, 768),
                                                              {QStringLiteral("OK"), QStringLiteral("NG")});
        Shape rectangle;
        rectangle.type = ShapeType::Rectangle;
        rectangle.label = QStringLiteral("scratch");
        rectangle.points = {QPointF(260.0, 150.25), QPointF(120.5, 80.0)};
        QCOMPARE(annotation.document().addShape(rectangle), qsizetype(0));
        QVERIFY(annotation.document().setImageLabel(QStringLiteral("NG"), true));
        QCOMPARE(annotation.toBytes(), fixture("golden_new_file.json"));
    }

    void outputEncoding()
    {
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("bom_crlf.json"), imageInfo("0001.jpg", 768, 768));
        QVERIFY(annotation.document().confirm());
        const QByteArray bytes = annotation.toBytes();
        QVERIFY(!bytes.startsWith("\xEF\xBB\xBF"));
        QVERIFY(!bytes.contains('\r'));
        QVERIFY(!bytes.endsWith('\n'));
        QVERIFY(bytes.contains("\"description\": \"边缘\""));
    }

    // Format spec 5.3: the correction writes the image size and nothing else.
    void sizeCorrection()
    {
        const QByteArray original = fixture("upstream_v4.json");
        AnnotationFile annotation = AnnotationFile::fromBytes(original, imageInfo("0001.jpg", 1024, 768));
        QVERIFY(annotation.correctImageSize());
        Json expected = expectedNormalizations(test::parseOrFail(original), imageInfo("0001.jpg", 1024, 768));
        expected["imageWidth"] = 1024;
        LITEANNO_COMPARE_JSON(saved(annotation), expected);
    }

    void markSavedMakesTheOutputTheNewBase()
    {
        AnnotationFile annotation = AnnotationFile::fromBytes(fixture("upstream_v2_legacy.json"), imageInfo("0002.jpg", 640, 480));
        QVERIFY(annotation.document().setShapeLabel(0, QStringLiteral("dent")));
        const QByteArray first = annotation.toBytes();
        annotation.markSaved();
        QVERIFY(!annotation.needsWrite());
        QVERIFY(!annotation.isNew());
        QCOMPARE(annotation.toBytes(), first);

        QVERIFY(annotation.document().setShapeDescription(0, QStringLiteral("x")));
        const Json after = saved(annotation);
        QCOMPARE(after["shapes"][0]["label"], json(R"("dent")"));
        QCOMPARE(after["shapes"][0]["description"], json(R"("x")"));
    }
};

QTEST_GUILESS_MAIN(TestAnnotationWrite)
#include "tst_annotationwrite.moc"
