// The pose configuration (X-AnyLabeling's pose_cfg_file): what is read, what is refused, the
// file written back, and the pose checks of the export. The exported and imported files are
// compared with upstream's in python/export_compat.py and python/import_compat.py.

#include "formats/Exporters.h"
#include "formats/PoseConfig.h"

#include <QTest>

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
};

QTEST_GUILESS_MAIN(TestPoseConfig)
#include "tst_poseconfig.moc"
