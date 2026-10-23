// The Windows icon made from the logo (imaging/IconFile.h, cmake/LiteAnnoLogo.cmake).
#include "imaging/IconFile.h"

#include <QImage>
#include <QTest>
#include <QtEndian>

using namespace liteanno::imaging;

namespace {

quint16 read16(const QByteArray& data, int at)
{
    return qFromLittleEndian<quint16>(data.constData() + at);
}

quint32 read32(const QByteArray& data, int at)
{
    return qFromLittleEndian<quint32>(data.constData() + at);
}

} // namespace

class TestIconFile : public QObject
{
    Q_OBJECT

private slots:
    // An .ico with one PNG entry per size, each the logo at that size.
    void iconHoldsEverySize()
    {
        QImage logo(300, 300, QImage::Format_ARGB32);
        logo.fill(QColor(31, 111, 235));
        const QByteArray icon = windowsIcon(logo);
        QVERIFY(icon.size() > 6);
        QCOMPARE(read16(icon, 0), quint16(0));
        QCOMPARE(read16(icon, 2), quint16(1));
        const QList<int> sizes = windowsIconSizes();
        QCOMPARE(int(read16(icon, 4)), sizes.size());
        quint32 expectedOffset = 6 + 16 * quint32(sizes.size());
        for (int i = 0; i < sizes.size(); ++i) {
            const int entry = 6 + 16 * i;
            const int side = sizes[i] == 256 ? 0 : sizes[i];
            QCOMPARE(int(quint8(icon[entry])), side);
            QCOMPARE(int(quint8(icon[entry + 1])), side);
            QCOMPARE(read16(icon, entry + 6), quint16(32));
            const quint32 length = read32(icon, entry + 8);
            const quint32 offset = read32(icon, entry + 12);
            QCOMPARE(offset, expectedOffset);
            expectedOffset += length;
            QImage image;
            QVERIFY(image.loadFromData(icon.mid(int(offset), int(length)), "PNG"));
            QCOMPARE(image.size(), QSize(sizes[i], sizes[i]));
            QCOMPARE(image.pixelColor(sizes[i] / 2, sizes[i] / 2), QColor(31, 111, 235));
        }
        QCOMPARE(quint32(icon.size()), expectedOffset);
    }

    // A logo that is not square is centred on a transparent square, not stretched.
    void wideLogoIsCentred()
    {
        QImage wide(200, 100, QImage::Format_ARGB32);
        wide.fill(Qt::red);
        const QImage square = squareLogo(wide);
        QCOMPARE(square.size(), QSize(200, 200));
        QCOMPARE(square.pixelColor(100, 100), QColor(Qt::red));
        QCOMPARE(square.pixelColor(100, 10).alpha(), 0);
        QCOMPARE(square.pixelColor(100, 190).alpha(), 0);
    }

    void nothingFromNoImage()
    {
        QVERIFY(windowsIcon(QImage()).isEmpty());
    }

    // The default logo of the repository is a square PNG large enough for 256 px.
    void defaultLogo()
    {
        const QImage logo(QStringLiteral(LITEANNO_DEFAULT_LOGO));
        QVERIFY(!logo.isNull());
        QCOMPARE(logo.width(), logo.height());
        QVERIFY(logo.width() >= 256);
    }
};

QTEST_GUILESS_MAIN(TestIconFile)
#include "tst_iconfile.moc"
