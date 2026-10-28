// The pose configuration (X-AnyLabeling's pose_cfg_file): what is read, what is refused, the
// file written back, and the pose checks of the export. The exported and imported files are
// compared with upstream's in python/export_compat.py and python/import_compat.py.

#include "core/AnnotationDocument.h"
#include "formats/AnnotationFile.h"
#include "formats/Exporters.h"
#include "formats/Importers.h"
#include "formats/PoseConfig.h"

#include <QTest>

#include <limits>

using namespace liteanno;
using namespace liteanno::formats;

namespace {

ExportShape shape(const char* label, const char* type, std::optional<qint64> group, int points = 1)
{
    ExportShape s;
    s.label = QString::fromUtf8(label);
    s.type = QString::fromUtf8(type);
    s.groupId = group;
    for (int i = 0; i < points; ++i)
        s.points.append({PyNumber{double(10 + i), false, {}}, PyNumber{double(10 + i), false, {}}});
    return s;
}

QStringList reasons(const ExportImage& image, const PoseConfig& pose)
{
    QStringList found;
    for (const ExportIssue& issue : unconvertibleShapes(image, ExportFormat::YoloPose, {}, &pose))
        found.append(QString::number(issue.shape));
    return found;
}

bool difficult(const Shape& shape)
{
    return shape.copiedObject.contains("\"difficult\": true") || shape.copiedObject.contains("\"difficult\":true");
}

} // namespace

class TestPoseConfig : public QObject
{
    Q_OBJECT

private slots:
    void reads()
    {
        const PoseConfig config = parsePoseConfig("# keypoints\nhas_visible: false\nclasses:\n  person:\n    - nose\n"
                                                  "    - 'left eye'\n  car: [front, \"back\"]  # flow list\n");
        QVERIFY2(config.error.isEmpty(), qPrintable(config.error));
        QVERIFY(!config.hasVisible);
        QCOMPARE(config.classes.size(), 2);
        QCOMPARE(config.classIndex(QStringLiteral("car")), 1);
        QCOMPARE(*config.keypoints(QStringLiteral("person")), (QStringList{QStringLiteral("nose"), QStringLiteral("left eye")}));
        QCOMPARE(*config.keypoints(QStringLiteral("car")), (QStringList{QStringLiteral("front"), QStringLiteral("back")}));
        QCOMPARE(config.maxKeypoints(), 2);

        // Written back, it reads the same.
        const PoseConfig again = parsePoseConfig(poseConfigFile(config));
        QVERIFY2(again.error.isEmpty(), qPrintable(again.error));
        QCOMPARE(again.classes, config.classes);
        QCOMPARE(again.hasVisible, config.hasVisible);
    }

    void refuses_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::newRow("tab") << QByteArray("classes:\n\tperson: [a]\n");
        QTest::newRow("duplicate class") << QByteArray("classes:\n  a: [x]\n  a: [y]\n");
        QTest::newRow("empty list") << QByteArray("classes:\n  a: []\n");
        QTest::newRow("number as name") << QByteArray("classes:\n  a: [1]\n");
        QTest::newRow("boolean as name") << QByteArray("classes:\n  a: [yes]\n");
        QTest::newRow("no classes") << QByteArray("has_visible: true\n");
        QTest::newRow("unclosed quote") << QByteArray("classes:\n  a: ['x]\n");
    }

    void refuses()
    {
        QFETCH(QByteArray, text);
        QVERIFY(!parsePoseConfig(text).error.isEmpty());
    }

    // Each group: one rectangle of a class the file names, points named by its keypoints,
    // each at most once.
    // The names of an ultralytics dataset yaml (data.yaml), chosen as the class file.
    void datasetYaml()
    {
        const QStringList two = {QStringLiteral("Dirty"), QStringLiteral("Scratch")};
        QCOMPARE(parseDatasetYaml("names:\n  0: Dirty\n  1: Scratch\n").names, two);
        QCOMPARE(parseDatasetYaml("names:\n  1: Scratch\n  0: Dirty  # comment\n").names, two);
        QCOMPARE(parseDatasetYaml("names: [Dirty, 'Scratch']\n").names, two);
        QCOMPARE(parseDatasetYaml("names:\n- Dirty\n- Scratch\n").names, two);
        QCOMPARE(parseDatasetYaml("\xEF\xBB\xBF# dataset\n---\npath: ../datasets/aoi  # root\ntrain: images/train\nval:\n  - images/val\n"
                                  "test:\nnc: 2\nnames:\n  0: Dirty\n  1: Scratch\ndownload: |\n  from x import y\n  y()\n")
                     .names,
                 two);
        QCOMPARE(parseDatasetYaml(u8"names:\n  0: 划痕\n  1: '1'\n").names, (QStringList{QStringLiteral(u"划痕"), QStringLiteral("1")}));

        QVERIFY(parseDatasetYaml("nc: 3\nnames: [a, b]\n").error.contains(QStringLiteral(u"第 1 行")));
        QVERIFY(parseDatasetYaml("train: images\n").error.contains(QStringLiteral("names")));
        QVERIFY(parseDatasetYaml("names: {0: a, 1: b}\n").error.contains(QStringLiteral(u"第 1 行")));
        QVERIFY(parseDatasetYaml("names:\n  0: a\n  2: b\n").error.contains(QStringLiteral(u"缺少编号 1")));
        QVERIFY(parseDatasetYaml("names:\n  0: a\n  - b\n").error.contains(QStringLiteral(u"第 3 行")));
        QVERIFY(parseDatasetYaml("names:\n  0: a\n  x: b\n").error.contains(QStringLiteral(u"第 3 行")));
        QVERIFY(parseDatasetYaml("names:\n  0: 1\n").error.contains(QStringLiteral(u"第 2 行")));  // a number, not a name
        QVERIFY(parseDatasetYaml("names: [a, a]\n").error.contains(QStringLiteral("a")));
        QVERIFY(parseDatasetYaml("names: [a]\nnames: [b]\n").error.contains(QStringLiteral(u"第 2 行")));
        QVERIFY(parseDatasetYaml("names:\n\t0: a\n").error.contains(QStringLiteral(u"第 2 行")));
        QVERIFY(!parseDatasetYaml("\xFF").error.isEmpty());

        // Review of #36: a list at the indentation of another key is passed over with it.
        QCOMPARE(parseDatasetYaml("train:\n- images/train\n- images/extra\nval: images/val\nnames: [Dirty, Scratch]\n").names, two);
        QCOMPARE(parseDatasetYaml("names:\n- Dirty\n- Scratch\nval:\n- images/val\n").names, two);
        // Review of #36: quotes do not make an empty or blank name.
        QVERIFY(parseDatasetYaml("names: [Dirty, \"\"]\n").error.contains(QStringLiteral(u"空")));
        QVERIFY(parseDatasetYaml("names:\n  0: Dirty\n  1: ' '\n").error.contains(QStringLiteral(u"空")));
        QVERIFY(parseDatasetYaml("names:\n- ''\n").error.contains(QStringLiteral(u"空")));
    }

    void poseChecks()
    {
        const PoseConfig pose = parsePoseConfig("classes:\n  person: [nose, tail]\n");
        QVERIFY(pose.error.isEmpty());
        ExportImage image;
        image.width = 100;
        image.height = 100;
        image.shapes = {shape("person", "rectangle", 0, 4), shape("nose", "point", 0), shape("tail", "point", 0)};
        QVERIFY(reasons(image, pose).isEmpty());

        ExportImage noGroup = image;
        noGroup.shapes[1].groupId.reset();
        QVERIFY(!reasons(noGroup, pose).isEmpty());

        ExportImage twoBoxes = image;
        twoBoxes.shapes.append(shape("person", "rectangle", 0, 4));
        QVERIFY(!reasons(twoBoxes, pose).isEmpty());

        ExportImage unknownClass = image;
        unknownClass.shapes[0].label = QStringLiteral("dog");
        QVERIFY(!reasons(unknownClass, pose).isEmpty());

        ExportImage unknownKeypoint = image;
        unknownKeypoint.shapes[2].label = QStringLiteral("ear");
        QVERIFY(!reasons(unknownKeypoint, pose).isEmpty());

        ExportImage twice = image;
        twice.shapes.append(shape("nose", "point", 0));
        QVERIFY(!reasons(twice, pose).isEmpty());
    }
    // A COCO image that cannot be imported leaves nothing behind for the next one: its
    // visibility flags are its own.
    void cocoFlagsStayWithTheirImage()
    {
        const PoseConfig pose = parsePoseConfig("classes:\n  person: [nose, eye]\n");
        const QByteArray file = R"({"images": [{"id": 1, "file_name": "a.png", "width": 100, "height": 100},
                                               {"id": 2, "file_name": "b.png", "width": 100, "height": 100}],
            "annotations": [{"id": 1, "image_id": 1, "category_id": 1, "bbox": [1, 1, 50, 50], "keypoints": [5, 5, 1, 6, 6, 7]},
                            {"id": 2, "image_id": 2, "category_id": 1, "bbox": [1, 1, 50, 50], "keypoints": [5, 5, 2, 6, 6, 2]}],
            "categories": [{"id": 1, "name": "person", "keypoints": ["nose", "eye"]}]})";
        const CocoLabels labels = parseCocoLabels(file, ImportFormat::CocoPose, &pose);
        QVERIFY2(labels.error.isEmpty(), qPrintable(labels.error));
        QVERIFY(!labels.images[0].labels.error.isEmpty());
        const QList<Shape>& shapes = labels.images[1].labels.shapes;
        QVERIFY2(labels.images[1].labels.error.isEmpty(), qPrintable(labels.images[1].labels.error));
        QCOMPARE(shapes.size(), 3);
        QVERIFY(!difficult(shapes[1]));
        QVERIFY(!difficult(shapes[2]));
    }

    // With visibility flags, a keypoint at (0, 0) marked visible is a keypoint: only flag 0
    // means missing. (Upstream also drops it; a deliberate difference, so that our own
    // export reads back.)
    void yoloKeypointAtTheOrigin()
    {
        const PoseConfig visible = parsePoseConfig("classes:\n  person: [nose, eye]\n");
        ImportedLabels labels = parseYoloPoseLabels("0 0.5 0.5 0.2 0.2 0 0 2 0.3 0.3 1\n", visible, 100, 100);
        QVERIFY2(labels.error.isEmpty(), qPrintable(labels.error));
        QCOMPARE(labels.shapes.size(), 3);
        QCOMPARE(labels.shapes[1].label, QStringLiteral("nose"));
        QCOMPARE(labels.shapes[1].points[0], QPointF(0, 0));
        labels = parseYoloPoseLabels("0 0.5 0.5 0.2 0.2 0 0 0 0.3 0.3 1\n", visible, 100, 100);
        QCOMPARE(labels.shapes.size(), 2);

        // Without flags (0, 0) is how a missing keypoint is written.
        const PoseConfig plain = parsePoseConfig("has_visible: false\nclasses:\n  person: [nose, eye]\n");
        labels = parseYoloPoseLabels("0 0.5 0.5 0.2 0.2 0 0 0.3 0.3\n", plain, 100, 100);
        QCOMPARE(labels.shapes.size(), 2);
    }

    // The group a keypoint drawn in an ungrouped rectangle gets is one no shape uses, also
    // next to the largest numbers.
    void newGroupIsFree()
    {
        const auto shape = [](ShapeType type, PointList points, std::optional<qint64> group) {
            Shape s;
            s.type = type;
            s.label = QStringLiteral("x");
            s.points = std::move(points);
            s.groupId = group;
            return s;
        };
        const qint64 max = std::numeric_limits<qint64>::max();
        const PointList box{QPointF(0, 0), QPointF(10, 0), QPointF(10, 10), QPointF(0, 10)};
        const PointList far{QPointF(50, 50), QPointF(60, 50), QPointF(60, 60), QPointF(50, 60)};
        AnnotationDocument document({shape(ShapeType::Rectangle, far, max), shape(ShapeType::Rectangle, far, max - 1),
                                     shape(ShapeType::Rectangle, box, std::nullopt)},
                                    {}, {});
        const qsizetype index = document.addKeypoint(shape(ShapeType::Point, {QPointF(5, 5)}, std::nullopt));
        QVERIFY(index >= 0);
        const std::optional<qint64> group = document.shapes().at(index).groupId;
        QVERIFY(group);
        QVERIFY(*group != max && *group != max - 1);
        QCOMPARE(document.shapes().at(2).groupId, group);
    }

    // "遮挡": a keypoint's "difficult". Checked exports v=1, unchecked v=2, a missing one v=0.
    // A value that is not true reads as unchecked and stays as it is until it is changed.
    void occludedKeypoints()
    {
        const QByteArray original = R"({"shapes": [
            {"label": "person", "points": [[10, 10], [90, 10], [90, 90], [10, 90]], "group_id": 1, "shape_type": "rectangle"},
            {"label": "nose", "points": [[20, 20]], "group_id": 1, "difficult": false, "shape_type": "point"},
            {"label": "eye", "points": [[30, 30]], "group_id": 1, "difficult": true, "shape_type": "point"},
            {"label": "ear", "points": [[40, 40]], "group_id": 1, "difficult": "yes", "shape_type": "point"}],
            "imagePath": "a.png", "imageData": null, "imageHeight": 100, "imageWidth": 100})";
        AnnotationFile file = AnnotationFile::fromBytes(original, ImageInfo{QStringLiteral("a.png"), 100, 100, 1});
        AnnotationDocument& document = file.document();
        QVERIFY(!document.shapes().at(1).difficult);
        QVERIFY(document.shapes().at(2).difficult);
        QVERIFY(!document.shapes().at(3).difficult);
        const PoseConfig pose = parsePoseConfig("classes:\n  person: [nose, eye, ear, tail]\n");
        QVERIFY(pose.error.isEmpty());
        const auto flags = [&](const QByteArray& bytes) {
            const AnnotationFile read = AnnotationFile::fromBytes(bytes, ImageInfo{QStringLiteral("a.png"), 100, 100, 1});
            const QList<QByteArray> fields = yoloPoseLabels(exportImageOf(QStringLiteral("a.json"), bytes, read), pose)
                                                 .trimmed().split(' ');
            QByteArray found;
            for (qsizetype k = 0; k < 4; ++k)
                found += fields.at(5 + 3 * k + 2);
            return found;
        };
        QCOMPARE(flags(original), QByteArray("2120"));

        QVERIFY(document.setShapeDifficult(1, true));
        QVERIFY(document.setShapeDifficult(2, false));
        QVERIFY(!document.setShapeDifficult(3, false));
        const QByteArray bytes = file.toBytes();
        QVERIFY(bytes.contains(R"("difficult": "yes")"));
        QCOMPARE(flags(bytes), QByteArray("1220"));

        // A pasted copy keeps the mark, and a new keypoint can take it.
        Shape copy = document.shapes().at(1);
        copy.copiedObject = file.shapeObject(1);
        QVERIFY(document.shapes().at(document.addShape(copy)).difficult);
        Shape unmarked = document.shapes().at(1);
        unmarked.copiedObject = file.shapeObject(1);
        unmarked.difficult = false;
        const qsizetype added = document.addShape(unmarked);
        QVERIFY(file.toBytes().count(R"("difficult": false)") >= 1);
        QVERIFY(!AnnotationFile::fromBytes(file.toBytes(), ImageInfo{QStringLiteral("a.png"), 100, 100, 1})
                     .document().shapes().at(added).difficult);
    }
};

QTEST_GUILESS_MAIN(TestPoseConfig)
#include "tst_poseconfig.moc"
