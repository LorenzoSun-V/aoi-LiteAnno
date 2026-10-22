#include "core/Geometry.h"

#include <QTest>

#include <cmath>

using namespace liteanno;

namespace {

// The expected numbers below were produced by X-AnyLabeling 4.2.0's own code
// (canvas.py: rotate_point, bounded_rotate_shapes, get_adjoint_points) and are written as
// hexadecimal floats so that they are bit-exact.
PointList box()
{
    return {QPointF(100.0, 50.0), QPointF(180.5, 50.0), QPointF(180.5, 90.25), QPointF(100.0, 90.25)};
}

bool sameBits(const PointList& actual, const PointList& expected)
{
    if (!exactlyEqual(actual, expected)) {
        for (const QPointF& p : actual)
            qWarning("%a %a", p.x(), p.y());
        return false;
    }
    return true;
}

RotatedBox turnedBox()
{
    // Z three times, C five times, V seven times.
    RotatedBox turned{box(), 0};
    for (int i = 0; i < 3; ++i)
        turned = rotateBox(turned, radians(1.0));
    for (int i = 0; i < 5; ++i)
        turned = rotateBox(turned, -radians(0.1));
    for (int i = 0; i < 7; ++i)
        turned = rotateBox(turned, -radians(1.0));
    return turned;
}

} // namespace

class TestGeometry : public QObject
{
    Q_OBJECT

private slots:
    void pythonModulo()
    {
        QCOMPARE(liteanno::pythonModulo(-0.5, 2 * 3.141592653589793), 2 * 3.141592653589793 - 0.5);
        QCOMPARE(liteanno::pythonModulo(7.0, 2.0), 1.0);
        QVERIFY(!std::signbit(liteanno::pythonModulo(-4.0, 2.0)));
    }

    void turningMatchesUpstream()
    {
        const RotatedBox turned = turnedBox();
        QVERIFY(sameBits(turned.points, {QPointF(0x1.96cff0b21a02cp+6, 0x1.773b83d6a284ap+5),
                                         QPointF(0x1.6be8ea393e17ep+7, 0x1.a9c29868fb4e6p+5),
                                         QPointF(0x1.659807a6f2feap+7, 0x1.75623e14aebdbp+6),
                                         QPointF(0x1.8a2e2b8d83d02p+6, 0x1.5c1eb3cb8258dp+6)}));
        QCOMPARE(turned.direction, 0x1.41b2f769cf0b9p-4);
    }

    void movingACornerMatchesUpstream_data()
    {
        QTest::addColumn<int>("index");
        QTest::addColumn<QPointF>("position");
        QTest::addColumn<PointList>("expected");
        QTest::newRow("0") << 0 << QPointF(95.0, 40.0)
                           << PointList{QPointF(0x1.7c00000000000p+6, 0x1.4000000000000p+5),
                                        QPointF(0x1.6ce846d8c9a1dp+7, 0x1.770fe0abbc12bp+5),
                                        QPointF(0x1.659807a6f2feap+7, 0x1.75623e14aebdbp+6),
                                        QPointF(0x1.6d5f819c52b9dp+6, 0x1.59da4dbed0b47p+6)};
        QTest::newRow("1") << 1 << QPointF(200.0, 45.5)
                           << PointList{QPointF(0x1.99ac04e9ce54ap+6, 0x1.2e8f9fd323e4cp+5),
                                        QPointF(0x1.9000000000000p+7, 0x1.6c00000000000p+5),
                                        QPointF(0x1.88411351dabdcp+7, 0x1.7ad6e3e1f0667p+6),
                                        QPointF(0x1.8a2e2b8d83d02p+6, 0x1.5c1eb3cb8258dp+6)};
        QTest::newRow("2") << 2 << QPointF(190.0, 100.0)
                           << PointList{QPointF(0x1.96cff0b21a02cp+6, 0x1.773b83d6a284ap+5),
                                        QPointF(0x1.83380d50f98b2p+7, 0x1.b119174193c23p+5),
                                        QPointF(0x1.7c00000000000p+7, 0x1.9000000000000p+6),
                                        QPointF(0x1.885fd61026ecap+6, 0x1.7311364a87614p+6)};
        QTest::newRow("3") << 3 << QPointF(90.0, 99.0)
                           << PointList{QPointF(0x1.78965dd629218p+6, 0x1.727998302bba9p+5),
                                        QPointF(0x1.6be8ea393e17ep+7, 0x1.a9c29868fb4e6p+5),
                                        QPointF(0x1.639dbb4e29872p+7, 0x1.a7a4801c67ca0p+6),
                                        QPointF(0x1.6800000000000p+6, 0x1.8c00000000000p+6)};
    }

    void movingACornerMatchesUpstream()
    {
        QFETCH(int, index);
        QFETCH(QPointF, position);
        QFETCH(PointList, expected);
        QVERIFY(sameBits(moveBoxCorner(turnedBox(), index, position), expected));
    }

    void uprightBoxStaysUpright()
    {
        const PointList square = {QPointF(0, 0), QPointF(10, 0), QPointF(10, 10), QPointF(0, 10)};
        const PointList moved = moveBoxCorner({square, 0}, 1, QPointF(4, -3));
        QVERIFY(sameBits(moved, {QPointF(0, -3), QPointF(4, -3), QPointF(4, 10), QPointF(0, 10)}));
    }
};

QTEST_GUILESS_MAIN(TestGeometry)
#include "tst_geometry.moc"
