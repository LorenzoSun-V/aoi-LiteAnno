// Image identifiers and annotation file names (format spec 2, 3).

#include "core/ImagePath.h"

#include <QTest>

using namespace liteanno;

class TestImagePath : public QObject
{
    Q_OBJECT

private slots:
    void validity_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("valid");
        QTest::newRow("file") << QStringLiteral("0001.bmp") << true;
        QTest::newRow("nested") << QStringLiteral("line1/2026-10-20/0123.bmp") << true;
        QTest::newRow("dots in name") << QStringLiteral("a/..b/c..jpg") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("absolute") << QStringLiteral("/data/a.jpg") << false;
        QTest::newRow("drive") << QStringLiteral("E:/a.jpg") << false;
        QTest::newRow("backslash") << QStringLiteral("a\\b.jpg") << false;
        QTest::newRow("parent") << QStringLiteral("a/../b.jpg") << false;
        QTest::newRow("current") << QStringLiteral("./b.jpg") << false;
        QTest::newRow("empty segment") << QStringLiteral("a//b.jpg") << false;
        QTest::newRow("trailing slash") << QStringLiteral("a/") << false;
    }

    void validity()
    {
        QFETCH(QString, path);
        QFETCH(bool, valid);
        QCOMPARE(isValidImagePath(path), valid);
    }

    void annotationPath()
    {
        QCOMPARE(annotationPathForImage(QStringLiteral("line1/0001.bmp")), QStringLiteral("line1/0001.json"));
        QCOMPARE(annotationPathForImage(QStringLiteral("a.b.jpg")), QStringLiteral("a.b.json"));
        QCOMPARE(annotationPathForImage(QStringLiteral("dir.v2/noext")), QStringLiteral("dir.v2/noext.json"));
        QCOMPARE(annotationPathForImage(QStringLiteral("x/.hidden")), QStringLiteral("x/.hidden.json"));
    }
};

QTEST_GUILESS_MAIN(TestImagePath)
#include "tst_imagepath.moc"
