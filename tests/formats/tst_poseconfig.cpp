// The pose configuration (X-AnyLabeling's pose_cfg_file): what is read, what is refused, the
// file written back, and the pose checks of the export. The exported and imported files are
// compared with upstream's in python/export_compat.py and python/import_compat.py.

#include "core/AnnotationDocument.h"
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
};

QTEST_GUILESS_MAIN(TestPoseConfig)
#include "tst_poseconfig.moc"
