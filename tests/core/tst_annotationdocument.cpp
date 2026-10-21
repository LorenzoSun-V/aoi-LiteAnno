#include "core/AnnotationDocument.h"

#include <QTest>

#include <limits>
#include <utility>

using namespace liteanno;

namespace {

Shape editableShape(quint64 id, ShapeType type, PointList points)
{
    Shape shape;
    shape.id = id;
    shape.type = type;
    shape.label = QStringLiteral("scratch");
    shape.points = std::move(points);
    return shape;
}

PointList square()
{
    return {QPointF(0, 0), QPointF(10, 0), QPointF(10, 10), QPointF(0, 10)};
}

AnnotationDocument confirmedDocument()
{
    ReviewState review;
    review.checked = true;
    Shape unrecognized;
    unrecognized.id = 2;
    unrecognized.kind = ShapeKind::Unrecognized;
    return AnnotationDocument({editableShape(1, ShapeType::Polygon, square()), unrecognized}, {}, review);
}

AnnotationDocument skippedDocument()
{
    ReviewState review;
    review.skip = SkipState::Skipped;
    review.skipReason = QStringLiteral("图片模糊");
    return AnnotationDocument({editableShape(1, ShapeType::Polygon, square())}, {}, review);
}

} // namespace

class TestAnnotationDocument : public QObject
{
    Q_OBJECT

private slots:
    void readDocumentIsUnmodified()
    {
        const AnnotationDocument document = confirmedDocument();
        QVERIFY(!document.isModified());
        QVERIFY(!document.reviewStateWritten());
        QVERIFY(document.review().checked);
    }

    // Format spec 5.2: content edits clear the confirmation.
    void contentEditClearsConfirmation()
    {
        AnnotationDocument document = confirmedDocument();
        QVERIFY(document.setShapeLabel(0, QStringLiteral("dent")));
        QVERIFY(!document.review().checked);
        QVERIFY(document.reviewStateWritten());
        QVERIFY(document.isModified());
    }

    void contentEditClearsSkip()
    {
        AnnotationDocument document = skippedDocument();
        QVERIFY(document.setShapePoints(0, {QPointF(1, 1), QPointF(5, 1), QPointF(5, 5)}));
        QCOMPARE(document.review().skip, SkipState::None);
        QVERIFY(document.review().skipReason.isEmpty());
    }

    void contentEditKeepsUnknownStatus()
    {
        ReviewState review;
        review.skip = SkipState::Unknown;
        AnnotationDocument document({editableShape(1, ShapeType::Polygon, square())}, {}, review);
        QVERIFY(document.removeShape(0));
        QCOMPARE(document.review().skip, SkipState::Unknown);
    }

    void everyContentEditCounts()
    {
        AnnotationDocument document = confirmedDocument();
        QVERIFY(document.setImageLabel(QStringLiteral("NG"), true));
        QVERIFY(!document.review().checked);

        document = confirmedDocument();
        QVERIFY(document.addShape(editableShape(0, ShapeType::Point, {QPointF(3, 4)})) >= 0);
        QVERIFY(!document.review().checked);

        document = confirmedDocument();
        QVERIFY(document.removeShape(1));
        QVERIFY(!document.review().checked);
    }

    // Description and lock are not content fields.
    void nonContentEditsKeepReviewState()
    {
        AnnotationDocument document = confirmedDocument();
        QVERIFY(document.setShapeDescription(0, QStringLiteral("边缘")));
        QVERIFY(document.setShapeLocked(0, true));
        QVERIFY(document.review().checked);
        QVERIFY(!document.reviewStateWritten());
        QVERIFY(document.isModified());
    }

    void unchangedValuesAreNotEdits()
    {
        AnnotationDocument document = confirmedDocument();
        QVERIFY(!document.setShapeLabel(0, QStringLiteral("scratch")));
        QVERIFY(!document.setShapePoints(0, square()));
        QVERIFY(!document.setShapeDescription(0, QString()));
        QVERIFY(!document.confirm());
        QVERIFY(!document.unskip());
        QVERIFY(!document.isModified());
    }

    void unrecognizedShapesCanOnlyBeRemoved()
    {
        AnnotationDocument document = confirmedDocument();
        QVERIFY(!document.setShapeLabel(1, QStringLiteral("dent")));
        QVERIFY(!document.setShapePoints(1, square()));
        QVERIFY(!document.setShapeDescription(1, QStringLiteral("x")));
        QVERIFY(!document.isModified());
        QVERIFY(document.removeShape(1));
        QCOMPARE(document.shapes().size(), qsizetype(1));
    }

    void confirmClearsSkipAndUnknownStatus()
    {
        AnnotationDocument document = skippedDocument();
        QVERIFY(document.confirm());
        QVERIFY(document.review().checked);
        QCOMPARE(document.review().skip, SkipState::None);
        QVERIFY(document.review().skipReason.isEmpty());

        ReviewState review;
        review.skip = SkipState::Unknown;
        document = AnnotationDocument({}, {}, review);
        QVERIFY(document.confirm());
        QCOMPARE(document.review().skip, SkipState::None);
    }

    void skipNeedsReason()
    {
        AnnotationDocument document = confirmedDocument();
        QVERIFY(!document.skip(QString()));
        QVERIFY(!document.skip(QStringLiteral("  ")));
        QVERIFY(document.review().checked);
        QVERIFY(document.skip(QStringLiteral("重复")));
        QVERIFY(!document.review().checked);
        QCOMPARE(document.review().skip, SkipState::Skipped);
        QCOMPARE(document.review().skipReason, QStringLiteral("重复"));
        QVERIFY(!document.skip(QStringLiteral("重复")));
    }

    void unskipRemovesStatus()
    {
        AnnotationDocument document = skippedDocument();
        QVERIFY(document.unskip());
        QCOMPARE(document.review().skip, SkipState::None);
        QVERIFY(!document.review().checked);
    }

    // Rectangles are kept as their four corners in upstream's order (format spec 4.2).
    void rectanglesAreCanonical()
    {
        AnnotationDocument document;
        const qsizetype index = document.addShape(
            editableShape(0, ShapeType::Rectangle, {QPointF(30, 40), QPointF(10, 20)}));
        QCOMPARE(index, qsizetype(0));
        const PointList expected{QPointF(10, 20), QPointF(30, 20), QPointF(30, 40), QPointF(10, 40)};
        QVERIFY(exactlyEqual(document.shapes().at(0).points, expected));

        QVERIFY(document.setShapePoints(0, {QPointF(5, 5), QPointF(1, 1), QPointF(5, 1), QPointF(1, 5)}));
        const PointList moved{QPointF(1, 1), QPointF(5, 1), QPointF(5, 5), QPointF(1, 5)};
        QVERIFY(exactlyEqual(document.shapes().at(0).points, moved));
        QVERIFY(!document.setShapePoints(0, {QPointF(1, 1)}));
    }

    void newShapesGetFreshIds()
    {
        AnnotationDocument document = confirmedDocument();
        const qsizetype index = document.addShape(editableShape(0, ShapeType::Point, {QPointF(1, 1)}));
        QCOMPARE(document.shapes().at(index).id, quint64(3));
    }

    void shapesThisBuildCannotCreate()
    {
        AnnotationDocument document;
        Shape preserved = editableShape(0, ShapeType::Quadrilateral, square());
        QCOMPARE(document.addShape(preserved), qsizetype(-1));
        QCOMPARE(document.addShape(editableShape(0, ShapeType::Rotation, square())), qsizetype(-1));
        QCOMPARE(document.addShape(editableShape(0, ShapeType::Line, {QPointF(1, 1)})), qsizetype(-1));
        QVERIFY(!document.isModified());
    }

    // JSON cannot hold NaN or infinity, so such points would make the saved file unreadable.
    void nonFinitePointsAreRejected()
    {
        AnnotationDocument document({editableShape(1, ShapeType::Polygon, square())}, {}, {});
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double infinity = std::numeric_limits<double>::infinity();
        QVERIFY(!document.setShapePoints(0, {QPointF(0, 0), QPointF(nan, 1), QPointF(2, 2)}));
        QVERIFY(!document.setShapePoints(0, {QPointF(0, 0), QPointF(1, -infinity), QPointF(2, 2)}));
        QCOMPARE(document.addShape(editableShape(0, ShapeType::Point, {QPointF(infinity, 0)})), qsizetype(-1));
        QVERIFY(!document.isModified());
        QVERIFY(exactlyEqual(document.shapes().at(0).points, square()));
    }

    // Upstream keeps the angle of a rotated box in "direction"; until the rotation tool keeps
    // it in step, the corners of existing rotated boxes stay as they are.
    void rotatedBoxCornersAreNotEditable()
    {
        AnnotationDocument document({editableShape(1, ShapeType::Rotation, square())}, {}, {});
        const PointList turned = {QPointF(5, -2), QPointF(12, 5), QPointF(5, 12), QPointF(-2, 5)};
        QVERIFY(!document.setShapePoints(0, turned));
        QVERIFY(!document.isModified());
        QVERIFY(document.setShapeLabel(0, QStringLiteral("dent")));
    }

    void markSavedResetsTracking()
    {
        AnnotationDocument document = confirmedDocument();
        document.setShapeLabel(0, QStringLiteral("dent"));
        document.markSaved();
        QVERIFY(!document.isModified());
        QVERIFY(!document.reviewStateWritten());
    }
};

QTEST_GUILESS_MAIN(TestAnnotationDocument)
#include "tst_annotationdocument.moc"
