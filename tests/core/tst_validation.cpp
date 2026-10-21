// Confirm conditions (format spec 5.3) and derived status (5.1).

#include "core/AnnotationStatus.h"
#include "core/Validation.h"

#include <QTest>

using namespace liteanno;

namespace {

Shape makeShape(ShapeType type, const QString& label, PointList points)
{
    Shape shape;
    shape.type = type;
    shape.label = label;
    shape.points = std::move(points);
    return shape;
}

Shape box(const QString& label = QStringLiteral("scratch"))
{
    return makeShape(ShapeType::Rectangle, label, {QPointF(0, 0), QPointF(10, 10)});
}

ProjectConfig projectWithClasses()
{
    ProjectConfig project;
    project.classes = {{0, QStringLiteral("scratch"), QString()}, {1, QStringLiteral("dent"), QString()}};
    return project;
}

QList<ConfirmBlockerCode> codes(const QList<ConfirmBlocker>& blockers)
{
    QList<ConfirmBlockerCode> result;
    for (const ConfirmBlocker& blocker : blockers)
        result.append(blocker.code);
    return result;
}

} // namespace

Q_DECLARE_METATYPE(liteanno::StatusFacts)

class TestValidation : public QObject
{
    Q_OBJECT

private slots:
    void validDocumentHasNoBlockers()
    {
        const AnnotationDocument document({box(), box(QStringLiteral("dent"))}, {}, {});
        QVERIFY(confirmBlockers(document, projectWithClasses(), false).isEmpty());
        // An empty image ("no objects") can be confirmed too.
        QVERIFY(confirmBlockers(AnnotationDocument(), projectWithClasses(), false).isEmpty());
    }

    void unrecognizedAndPreserveOnlyObjectsBlock()
    {
        Shape unrecognized = box();
        unrecognized.kind = ShapeKind::Unrecognized;
        Shape cuboid = box();
        cuboid.kind = ShapeKind::PreserveOnly;
        const AnnotationDocument document({box(), unrecognized, cuboid}, {}, {});
        const QList<ConfirmBlocker> blockers = confirmBlockers(document, ProjectConfig(), false);
        QCOMPARE(codes(blockers), (QList<ConfirmBlockerCode>{ConfirmBlockerCode::UnrecognizedObject,
                                                             ConfirmBlockerCode::UnrecognizedObject}));
        QCOMPARE(blockers[0].shapeIndex, 1);
        QCOMPARE(blockers[1].shapeIndex, 2);
    }

    void unknownClassBlocksOnlyWhenClassesAreDefined()
    {
        const AnnotationDocument document({box(QStringLiteral("crack"))}, {}, {});
        const QList<ConfirmBlocker> blockers = confirmBlockers(document, projectWithClasses(), false);
        QCOMPARE(codes(blockers), QList<ConfirmBlockerCode>{ConfirmBlockerCode::UnknownClass});
        QCOMPARE(blockers[0].detail, QStringLiteral("crack"));
        QVERIFY(confirmBlockers(document, ProjectConfig(), false).isEmpty());
    }

    void requiredAttributesMustBeFilled()
    {
        ProjectConfig project = projectWithClasses();
        project.attributes.insert(QStringLiteral("scratch"),
                                  {{QStringLiteral("severity"), {}, true}, {QStringLiteral("note"), {}, false}});
        Shape filled = box();
        filled.attributes.insert(QStringLiteral("severity"), QStringLiteral("重"));
        Shape empty = box();
        empty.attributes.insert(QStringLiteral("severity"), QString());
        const AnnotationDocument document({filled, box(), empty, box(QStringLiteral("dent"))}, {}, {});
        const QList<ConfirmBlocker> blockers = confirmBlockers(document, project, false);
        QCOMPARE(codes(blockers), (QList<ConfirmBlockerCode>{ConfirmBlockerCode::MissingAttribute,
                                                             ConfirmBlockerCode::MissingAttribute}));
        QCOMPARE(blockers[0].shapeIndex, 1);
        QCOMPARE(blockers[1].shapeIndex, 2);
        QCOMPARE(blockers[0].detail, QStringLiteral("severity"));
    }

    void geometry_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<PointList>("points");
        QTest::addColumn<bool>("valid");
        QTest::newRow("rectangle") << int(ShapeType::Rectangle) << PointList{{0, 0}, {1, 1}} << true;
        QTest::newRow("rectangle zero width") << int(ShapeType::Rectangle) << PointList{{5, 0}, {5, 9}} << false;
        QTest::newRow("rectangle zero height") << int(ShapeType::Rectangle)
                                               << PointList{{0, 3}, {9, 3}, {9, 3}, {0, 3}} << false;
        QTest::newRow("polygon") << int(ShapeType::Polygon) << PointList{{0, 0}, {4, 0}, {0, 4}} << true;
        QTest::newRow("polygon repeated points") << int(ShapeType::Polygon)
                                                 << PointList{{0, 0}, {4, 0}, {4, 0}, {0, 0}} << false;
        QTest::newRow("circle") << int(ShapeType::Circle) << PointList{{5, 5}, {5, 8}} << true;
        QTest::newRow("circle zero radius") << int(ShapeType::Circle) << PointList{{5, 5}, {5, 5}} << false;
        QTest::newRow("point") << int(ShapeType::Point) << PointList{{1, 1}} << true;
        QTest::newRow("linestrip") << int(ShapeType::LineStrip) << PointList{{0, 0}, {4, 0}} << true;
        // Format spec 13.5: read with too few points, editable but not confirmable.
        QTest::newRow("linestrip one point") << int(ShapeType::LineStrip) << PointList{{0, 0}} << false;
    }

    void geometry()
    {
        QFETCH(int, type);
        QFETCH(PointList, points);
        QFETCH(bool, valid);
        const Shape shape = makeShape(ShapeType(type), QStringLiteral("scratch"), points);
        QCOMPARE(hasValidGeometry(shape), valid);
        const AnnotationDocument document({shape}, {}, {});
        QCOMPARE(confirmBlockers(document, ProjectConfig(), false).isEmpty(), valid);
    }

    void outOfImageCoordinatesDoNotBlock()
    {
        const AnnotationDocument document(
            {makeShape(ShapeType::Rectangle, QStringLiteral("scratch"), {QPointF(-50, -50), QPointF(5000, 5000)})}, {},
            {});
        QVERIFY(confirmBlockers(document, ProjectConfig(), false).isEmpty());
    }

    void sizeMismatchBlocks()
    {
        QCOMPARE(codes(confirmBlockers(AnnotationDocument(), ProjectConfig(), true)),
                 QList<ConfirmBlockerCode>{ConfirmBlockerCode::ImageSizeMismatch});
    }

    void singleChoiceImageLabels_data()
    {
        QTest::addColumn<QList<ImageLabel>>("labels");
        QTest::addColumn<QList<ConfirmBlockerCode>>("expected");
        const QString ok = QStringLiteral("OK");
        const QString ng = QStringLiteral("NG");
        QTest::newRow("none") << QList<ImageLabel>{{ok, false}, {ng, false}}
                              << QList<ConfirmBlockerCode>{ConfirmBlockerCode::ImageLabelNotSelected};
        QTest::newRow("missing") << QList<ImageLabel>{}
                                 << QList<ConfirmBlockerCode>{ConfirmBlockerCode::ImageLabelNotSelected};
        QTest::newRow("one") << QList<ImageLabel>{{ok, false}, {ng, true}} << QList<ConfirmBlockerCode>{};
        QTest::newRow("two") << QList<ImageLabel>{{ok, true}, {ng, true}}
                             << QList<ConfirmBlockerCode>{ConfirmBlockerCode::ImageLabelAmbiguous};
        // Labels outside the configured options stay in the file but do not count (13.1).
        QTest::newRow("other label") << QList<ImageLabel>{{ok, true}, {QStringLiteral("old"), true}}
                                     << QList<ConfirmBlockerCode>{};
        QTest::newRow("non-boolean") << QList<ImageLabel>{{ok, std::nullopt}}
                                     << QList<ConfirmBlockerCode>{ConfirmBlockerCode::ImageLabelNotSelected};
    }

    void singleChoiceImageLabels()
    {
        QFETCH(QList<ImageLabel>, labels);
        QFETCH(QList<ConfirmBlockerCode>, expected);
        ProjectConfig project;
        project.imageLabels = {ImageLabelMode::Single, {QStringLiteral("OK"), QStringLiteral("NG")}};
        QCOMPARE(codes(confirmBlockers(AnnotationDocument({}, labels, {}), project, false)), expected);

        // Multiple choice has no limit.
        project.imageLabels.mode = ImageLabelMode::Multi;
        QVERIFY(confirmBlockers(AnnotationDocument({}, labels, {}), project, false).isEmpty());
    }

    void derivedStatus_data()
    {
        QTest::addColumn<StatusFacts>("facts");
        QTest::addColumn<AnnotationStatus>("expected");

        StatusFacts confirmed;
        confirmed.hasJson = true;
        confirmed.hasContent = true;
        confirmed.review.checked = true;
        confirmed.confirmable = true;

        StatusFacts facts = confirmed;
        facts.imageMissing = true;
        facts.nameCollision = true;
        facts.jsonError = true;
        QTest::newRow("missing first") << facts << AnnotationStatus::Missing;
        facts.imageMissing = false;
        QTest::newRow("collision before corrupt") << facts << AnnotationStatus::NameCollision;
        facts.nameCollision = false;
        QTest::newRow("corrupt") << facts << AnnotationStatus::Corrupt;
        facts = confirmed;
        facts.orientationPending = true;
        QTest::newRow("orientation") << facts << AnnotationStatus::OrientationPending;

        facts = confirmed;
        facts.review.skip = SkipState::Skipped;
        facts.review.skipReason = QStringLiteral("blurred");
        QTest::newRow("checked and skipped") << facts << AnnotationStatus::StateConflict;
        facts.review.checked = false;
        QTest::newRow("skipped") << facts << AnnotationStatus::Skipped;
        facts.review.skipReason = QStringLiteral(" \t");
        QTest::newRow("skipped with blank reason") << facts << AnnotationStatus::StateConflict;
        facts.review.skipReason.clear();
        QTest::newRow("skipped without reason") << facts << AnnotationStatus::StateConflict;
        facts.review.skip = SkipState::Unknown;
        QTest::newRow("unknown status") << facts << AnnotationStatus::StateConflict;

        QTest::newRow("confirmed") << confirmed << AnnotationStatus::Confirmed;
        facts = confirmed;
        facts.hasContent = false;
        QTest::newRow("confirmed without objects") << facts << AnnotationStatus::Confirmed;
        facts.confirmable = false;
        QTest::newRow("confirmed with problems") << facts << AnnotationStatus::ConfirmedWithProblems;

        facts = confirmed;
        facts.review.checked = false;
        QTest::newRow("pending") << facts << AnnotationStatus::PendingConfirmation;
        facts.hasContent = false;
        QTest::newRow("empty JSON") << facts << AnnotationStatus::Unprocessed;
        QTest::newRow("no JSON") << StatusFacts() << AnnotationStatus::Unprocessed;
    }

    void derivedStatus()
    {
        QFETCH(StatusFacts, facts);
        QFETCH(AnnotationStatus, expected);
        QCOMPARE(annotationStatusName(deriveStatus(facts)), annotationStatusName(expected));
    }

    void onlySkippedAndConfirmedCountAsDone()
    {
        for (int i = 0; i <= int(AnnotationStatus::Unprocessed); ++i) {
            const auto status = AnnotationStatus(i);
            QCOMPARE(countsAsDone(status),
                     status == AnnotationStatus::Skipped || status == AnnotationStatus::Confirmed);
        }
    }
};

QTEST_GUILESS_MAIN(TestValidation)
#include "tst_validation.moc"
