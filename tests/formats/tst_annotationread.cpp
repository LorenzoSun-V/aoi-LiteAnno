// Reading per-image annotation files (format spec section 4, 13.3-13.5).

#include "common/TestData.h"
#include "formats/AnnotationFile.h"

#include <QTest>

#include <algorithm>

using namespace liteanno;
using namespace liteanno::formats;

namespace {

ImageInfo imageInfo(const char* fileName, int width, int height, int exifOrientation = 1)
{
    return ImageInfo{QString::fromUtf8(fileName), width, height, exifOrientation};
}

AnnotationFile readFixture(const char* name, const ImageInfo& image)
{
    return AnnotationFile::fromBytes(test::readTestData(QStringLiteral("annotations/") + QLatin1String(name)), image);
}

bool hasDiagnostic(const AnnotationFile& file, DiagnosticCode code, const QString& location = QString())
{
    return std::any_of(file.diagnostics().begin(), file.diagnostics().end(), [&](const Diagnostic& diagnostic) {
        return diagnostic.code == code && (location.isNull() || diagnostic.location == location);
    });
}

QByteArray diagnosticsText(const AnnotationFile& file)
{
    QByteArray text;
    for (const Diagnostic& diagnostic : file.diagnostics())
        text += QByteArray(diagnosticCodeName(diagnostic.code)) + ' ' + diagnostic.location.toUtf8() + ' '
            + diagnostic.detail.toUtf8() + '\n';
    return text;
}

} // namespace

class TestAnnotationRead : public QObject
{
    Q_OBJECT

private slots:
    void upstreamFile()
    {
        const AnnotationFile file = readFixture("upstream_v4.json", imageInfo("0001.jpg", 768, 768));
        QVERIFY(!file.isReadOnly());
        QVERIFY2(file.diagnostics().isEmpty(), diagnosticsText(file).constData());

        const AnnotationDocument& document = file.document();
        QCOMPARE(document.shapes().size(), qsizetype(7));
        const ShapeType types[] = {ShapeType::Rectangle, ShapeType::Polygon, ShapeType::Rotation, ShapeType::Point,
                                   ShapeType::Line, ShapeType::LineStrip, ShapeType::Circle};
        for (qsizetype i = 0; i < 7; ++i) {
            QCOMPARE(document.shapes()[i].kind, ShapeKind::Editable);
            QCOMPARE(document.shapes()[i].type, types[i]);
        }
        QCOMPARE(document.shapes()[1].label, QStringLiteral("dent"));
        QCOMPARE(document.shapes()[1].description, QStringLiteral("边缘"));
        QVERIFY(document.shapes()[6].locked);
        QVERIFY(!document.review().checked);
        QCOMPARE(document.review().skip, SkipState::None);
        QVERIFY(!document.isModified());
    }

    // Upstream 2.x: two-point rectangles are read as their four corners (format spec 4.2).
    void legacyRectangle()
    {
        const AnnotationFile file = readFixture("upstream_v2_legacy.json", imageInfo("0002.jpg", 640, 480));
        QVERIFY(!file.isReadOnly());
        const Shape& rectangle = file.document().shapes().at(0);
        QCOMPARE(rectangle.kind, ShapeKind::Editable);
        const PointList corners{QPointF(120.5, 80.0), QPointF(260.0, 80.0), QPointF(260.0, 150.25),
                                QPointF(120.5, 150.25)};
        QVERIFY(exactlyEqual(rectangle.points, corners));
        QVERIFY(!file.document().review().checked);  // no "checked" field in 2.x files
    }

    void embeddedImageData()
    {
        const AnnotationFile file = readFixture("embedded_image_data.json", imageInfo("0003.png", 1, 1));
        QVERIFY(!file.isReadOnly());
        QVERIFY(hasDiagnostic(file, DiagnosticCode::EmbeddedImageData, QStringLiteral("/imageData")));
    }

    // Wide in: missing fields get defaults and warnings (format spec section 1, 4.1).
    void missingFields()
    {
        const AnnotationFile file = readFixture("missing_fields.json", imageInfo("0004.png", 10, 10));
        QVERIFY(!file.isReadOnly());
        QVERIFY(hasDiagnostic(file, DiagnosticCode::MissingVersion));
        QVERIFY(hasDiagnostic(file, DiagnosticCode::MissingImageSize, QStringLiteral("/imageHeight")));
        QVERIFY(hasDiagnostic(file, DiagnosticCode::MissingImageSize, QStringLiteral("/imageWidth")));
        QCOMPARE(file.document().shapes().size(), qsizetype(1));
        QVERIFY(file.document().imageLabels().isEmpty());

        const AnnotationFile empty = readFixture("empty_object.json", imageInfo("0004.png", 10, 10));
        QVERIFY(!empty.isReadOnly());
        QVERIFY(hasDiagnostic(empty, DiagnosticCode::MissingShapes));
        QVERIFY(hasDiagnostic(empty, DiagnosticCode::MissingImagePath));
        QVERIFY(hasDiagnostic(empty, DiagnosticCode::MissingImageData));
        QVERIFY(empty.document().shapes().isEmpty());
    }

    void unknownFieldsAreIgnoredWhenReading()
    {
        const AnnotationFile file = readFixture("unknown_fields.json", imageInfo("0005.bmp", 768, 768));
        QVERIFY(!file.isReadOnly());
        QVERIFY2(file.diagnostics().isEmpty(), diagnosticsText(file).constData());
        const QList<ImageLabel>& labels = file.document().imageLabels();
        QCOMPARE(labels.size(), qsizetype(3));
        QCOMPARE(labels[2].name, QStringLiteral("legacy_tag"));
        QCOMPARE(labels[2].value, std::optional<bool>(true));
    }

    // Format spec 4.2 and 13.5: what cannot be understood is kept as an unrecognized object.
    void unrecognizedObjects()
    {
        const AnnotationFile file = readFixture("unrecognized_objects.json", imageInfo("0006.jpg", 100, 100));
        QVERIFY(!file.isReadOnly());
        const QList<Shape>& shapes = file.document().shapes();
        QCOMPARE(shapes.size(), qsizetype(13));
        const ShapeKind expected[] = {
            ShapeKind::Editable,      // polygon
            ShapeKind::Unrecognized,  // unknown shape_type
            ShapeKind::Unrecognized,  // missing label
            ShapeKind::Unrecognized,  // coordinate is a string
            ShapeKind::Unrecognized,  // kie_linking with a float
            ShapeKind::Unrecognized,  // three-point rectangle
            ShapeKind::Unrecognized,  // not an object
            ShapeKind::PreserveOnly,  // quadrilateral
            ShapeKind::PreserveOnly,  // cuboid
            ShapeKind::Unrecognized,  // label is a number
            ShapeKind::Unrecognized,  // no points
            ShapeKind::Editable,      // polygon with two points: invalid geometry, still editable
            ShapeKind::Editable,      // null shape_type reads as polygon
        };
        for (qsizetype i = 0; i < shapes.size(); ++i)
            QVERIFY2(shapes[i].kind == expected[i], qPrintable(QStringLiteral("shape %1").arg(i)));
        QCOMPARE(shapes[12].type, ShapeType::Polygon);
        QVERIFY(hasDiagnostic(file, DiagnosticCode::ShapeTypeDefaulted, QStringLiteral("/shapes/12/shape_type")));
        QVERIFY(hasDiagnostic(file, DiagnosticCode::ShapeUnrecognized, QStringLiteral("/shapes/6")));
        QVERIFY(hasDiagnostic(file, DiagnosticCode::ShapePreserveOnly, QStringLiteral("/shapes/8")));
    }

    // Format spec section 1: a newer major version is opened read-only.
    void futureVersionIsReadOnly()
    {
        const AnnotationFile file = readFixture("future_version.json", imageInfo("0007.png", 100, 100));
        QCOMPARE(file.readOnlyReason(), ReadOnlyReason::UnsupportedFormatVersion);
        QVERIFY(!file.needsWrite());

        const AnnotationFile unreadableVersion = readFixture("bad_format_version.json", imageInfo("0009.png", 100, 100));
        QCOMPARE(unreadableVersion.readOnlyReason(), ReadOnlyReason::UnsupportedFormatVersion);
    }

    void newerMinorVersionIsWritable()
    {
        const AnnotationFile file = readFixture("minor_version.json", imageInfo("0008.png", 100, 100));
        QVERIFY(!file.isReadOnly());
    }

    void jsonErrors_data()
    {
        QTest::addColumn<QString>("file");
        for (const char* name : {"broken/nan_coordinate.json", "broken/huge_integer.json", "broken/truncated.json",
                                 "broken/empty.json", "broken/nul_suffix.json", "top_level_array.json", "shapes_not_array.json"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    // Format spec 4.6 and 13.3: broken files are shown read-only and never written.
    void jsonErrors()
    {
        QFETCH(QString, file);
        AnnotationFile annotation = AnnotationFile::fromBytes(test::readTestData(QStringLiteral("annotations/") + file),
                                                              imageInfo("a.jpg", 10, 10));
        QCOMPARE(annotation.readOnlyReason(), ReadOnlyReason::JsonError);
        annotation.document().confirm();
        QVERIFY(!annotation.needsWrite());
    }

    void bomAndCrlf()
    {
        const AnnotationFile file = readFixture("bom_crlf.json", imageInfo("0001.jpg", 768, 768));
        QVERIFY(!file.isReadOnly());
        QCOMPARE(file.document().shapes().size(), qsizetype(7));
    }

    void invalidCheckedReadsAsFalse()
    {
        const AnnotationFile file = readFixture("checked_invalid.json", imageInfo("0010.png", 100, 100));
        QVERIFY(!file.isReadOnly());
        QVERIFY(!file.document().review().checked);
        QVERIFY(hasDiagnostic(file, DiagnosticCode::InvalidChecked, QStringLiteral("/checked")));
    }

    void skipStates()
    {
        const AnnotationFile skipped = readFixture("skipped.json", imageInfo("0011.png", 100, 100));
        QCOMPARE(skipped.document().review().skip, SkipState::Skipped);
        QCOMPARE(skipped.document().review().skipReason, QStringLiteral("图片模糊"));

        const AnnotationFile conflict = readFixture("state_conflict.json", imageInfo("0012.png", 100, 100));
        QVERIFY(conflict.document().review().checked);
        QCOMPARE(conflict.document().review().skip, SkipState::Skipped);

        const AnnotationFile unknown = readFixture("unknown_status.json", imageInfo("0013.png", 100, 100));
        QCOMPARE(unknown.document().review().skip, SkipState::Unknown);
        QVERIFY(hasDiagnostic(unknown, DiagnosticCode::UnknownStatus));
    }

    // Format spec 5.3: SIZE_MISMATCH is read-only until the size is corrected.
    void sizeMismatch()
    {
        AnnotationFile file = readFixture("upstream_v4.json", imageInfo("0001.jpg", 1024, 768));
        QCOMPARE(file.readOnlyReason(), ReadOnlyReason::ImageSizeMismatch);
        QVERIFY(hasDiagnostic(file, DiagnosticCode::ImageSizeMismatch, QStringLiteral("/imageWidth")));
        QVERIFY(file.correctImageSize());
        QVERIFY(!file.isReadOnly());
        QVERIFY(file.needsWrite());
    }

    void imagePathMismatchIsAWarning()
    {
        const AnnotationFile file = readFixture("upstream_v4.json", imageInfo("0001.JPG", 768, 768));
        QVERIFY(!file.isReadOnly());
        QVERIFY(hasDiagnostic(file, DiagnosticCode::ImagePathMismatch, QStringLiteral("/imagePath")));
    }

    void exifOrientation_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("orientation");
        QTest::addColumn<bool>("readOnly");
        for (const int orientation : {3, 6, 8})
            QTest::addRow("upstream, orientation %d", orientation) << "upstream_v4.json" << orientation << true;
        QTest::newRow("upstream, orientation 1") << "upstream_v4.json" << 1 << false;
        QTest::newRow("written by this program") << "minor_version.json" << 6 << false;
        QTest::newRow("no shapes") << "empty_object.json" << 6 << false;
    }

    // Format spec 13.4: shapes from another tool on an EXIF-rotated image are not trusted.
    void exifOrientation()
    {
        QFETCH(QString, file);
        QFETCH(int, orientation);
        QFETCH(bool, readOnly);
        // Orientations 5-8 swap width and height; use a square image so only the rule matters.
        AnnotationFile annotation = AnnotationFile::fromBytes(
            test::readTestData(QStringLiteral("annotations/") + file), imageInfo("0001.jpg", 768, 768, orientation));
        QCOMPARE(annotation.readOnlyReason() == ReadOnlyReason::ExifOrientationUnconfirmed, readOnly);
        if (readOnly) {
            QVERIFY(!annotation.correctImageSize());
            QVERIFY(annotation.document().confirm());
            QVERIFY(!annotation.needsWrite());
        }
    }

    void exifRuleComesBeforeSizeMismatch()
    {
        // An upstream annotation of a 640x480 image whose EXIF orientation 6 makes it 480x640.
        const AnnotationFile file = readFixture("upstream_v2_legacy.json", imageInfo("0002.jpg", 480, 640, 6));
        QCOMPARE(file.readOnlyReason(), ReadOnlyReason::ExifOrientationUnconfirmed);
        QVERIFY(hasDiagnostic(file, DiagnosticCode::ImageSizeMismatch));
    }

    void newDocument()
    {
        const AnnotationFile file = AnnotationFile::createNew(imageInfo("0001.bmp", 768, 768),
                                                              {QStringLiteral("OK"), QStringLiteral("NG")});
        QVERIFY(file.isNew());
        QVERIFY(!file.isReadOnly());
        QVERIFY(file.diagnostics().isEmpty());
        QVERIFY(!file.needsWrite());
        QCOMPARE(file.document().imageLabel(QStringLiteral("OK")), std::optional<bool>(false));
    }

    void unreadableFile()
    {
        AnnotationFile file = AnnotationFile::unreadable(imageInfo("a.jpg", 1, 1), QStringLiteral("Permission denied"));
        QCOMPARE(file.readOnlyReason(), ReadOnlyReason::Unreadable);
        file.document().confirm();
        QVERIFY(!file.needsWrite());
    }
};

QTEST_GUILESS_MAIN(TestAnnotationRead)
#include "tst_annotationread.moc"
