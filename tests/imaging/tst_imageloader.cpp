// Reading images (format spec 9.1, 9.2, 11): EXIF orientation, 16-bit PNG and TIFF, multi-page
// TIFF, and content that does not match the extension.

#include "imaging/ImageLoader.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace liteanno::imaging;

namespace {

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

void appendU16(QByteArray& out, quint16 value)
{
    out.append(char(value >> 8)).append(char(value & 0xFF));
}

void appendU32(QByteArray& out, quint32 value)
{
    appendU16(out, quint16(value >> 16));
    appendU16(out, quint16(value & 0xFFFF));
}

// A 16x8 JPEG, blue with a red 4x4 block in the stored top-left corner, carrying the EXIF
// orientation `orientation`.
QByteArray jpegWithOrientation(int orientation)
{
    QImage image(16, 8, QImage::Format_RGB32);
    image.fill(Qt::blue);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            image.setPixel(x, y, qRgb(255, 0, 0));
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPEG", 100))
        qFatal("cannot encode JPEG");

    // APP1: "Exif\0\0", a big-endian TIFF header and one IFD entry, Orientation (0x0112).
    QByteArray exif("Exif\0\0", 6);
    exif.append("MM\0*", 4);
    appendU32(exif, 8);
    appendU16(exif, 1);
    appendU16(exif, 0x0112);
    appendU16(exif, 3);  // SHORT
    appendU32(exif, 1);
    appendU16(exif, quint16(orientation));
    appendU16(exif, 0);
    appendU32(exif, 0);  // no next IFD
    QByteArray segment("\xFF\xE1", 2);
    appendU16(segment, quint16(exif.size() + 2));
    segment.append(exif);
    return jpeg.left(2) + segment + jpeg.mid(2);
}

// An uncompressed 8-bit grayscale TIFF (little-endian) with one page per size.
QByteArray multiPageTiff(const QList<QSize>& pages)
{
    QByteArray out("II*\0", 4);
    out.append(4, '\0');  // offset of the first IFD, set below
    const auto put16 = [](QByteArray& b, quint16 v) { b.append(char(v & 0xFF)).append(char(v >> 8)); };
    const auto put32 = [&put16](QByteArray& b, quint32 v) {
        put16(b, quint16(v & 0xFFFF));
        put16(b, quint16(v >> 16));
    };
    qsizetype linkAt = 4;
    for (const QSize& size : pages) {
        const quint32 pixelsAt = quint32(out.size());
        out.append(QByteArray(size.width() * size.height(), char(0x80)));
        if (out.size() % 2)
            out.append('\0');
        const quint32 ifdAt = quint32(out.size());
        qToLittleEndian<quint32>(ifdAt, out.data() + linkAt);
        struct Entry {
            quint16 tag, type;
            quint32 count, value;
        };
        const QList<Entry> entries = {
            {256, 4, 1, quint32(size.width())},                     // ImageWidth
            {257, 4, 1, quint32(size.height())},                    // ImageLength
            {258, 3, 1, 8},                                         // BitsPerSample
            {259, 3, 1, 1},                                         // no compression
            {262, 3, 1, 1},                                         // BlackIsZero
            {273, 4, 1, pixelsAt},                                  // StripOffsets
            {277, 3, 1, 1},                                         // SamplesPerPixel
            {278, 4, 1, quint32(size.height())},                    // RowsPerStrip
            {279, 4, 1, quint32(size.width() * size.height())},     // StripByteCounts
        };
        put16(out, quint16(entries.size()));
        for (const Entry& e : entries) {
            put16(out, e.tag);
            put16(out, e.type);
            put32(out, e.count);
            if (e.type == 3) {
                put16(out, quint16(e.value));
                put16(out, 0);
            } else {
                put32(out, e.value);
            }
        }
        linkAt = out.size();
        put32(out, 0);
    }
    return out;
}

// The catalog reads sizes without decoding (architecture 6.1); they must be the sizes the
// editor finds when it loads the image.
void compareInfo(const QString& path, const LoadedImage& loaded)
{
    const std::optional<liteanno::ImageInfo> info = readImageInfo(path);
    QVERIFY(info.has_value());
    QCOMPARE(info->width, loaded.info.width);
    QCOMPARE(info->height, loaded.info.height);
    QCOMPARE(info->exifOrientation, loaded.info.exifOrientation);
    QCOMPARE(info->fileName, loaded.info.fileName);
}

} // namespace

class TestImageLoader : public QObject
{
    Q_OBJECT

private slots:
    void exifOrientation_data()
    {
        QTest::addColumn<int>("orientation");
        QTest::addColumn<bool>("redRight");
        QTest::addColumn<bool>("redBottom");
        // Where the stored top-left corner is shown (EXIF 2.3, tag 0x0112).
        QTest::newRow("1") << 1 << false << false;
        QTest::newRow("2 mirrored") << 2 << true << false;
        QTest::newRow("3 rotated 180") << 3 << true << true;
        QTest::newRow("4 flipped") << 4 << false << true;
        QTest::newRow("5 transposed") << 5 << false << false;
        QTest::newRow("6 rotated 90 cw") << 6 << true << false;
        QTest::newRow("7 transversed") << 7 << true << true;
        QTest::newRow("8 rotated 90 ccw") << 8 << false << true;
    }

    void exifOrientation()
    {
        QFETCH(int, orientation);
        QFETCH(bool, redRight);
        QFETCH(bool, redBottom);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("photo.jpg"));
        writeFile(path, jpegWithOrientation(orientation));

        const LoadedImage loaded = loadImage(path);
        QVERIFY2(!loaded.image.isNull(), qPrintable(loaded.error));
        QCOMPARE(loaded.info.exifOrientation, orientation);
        compareInfo(path, loaded);
        // Format spec 9.2: sizes and coordinates are those of the upright image.
        const bool turned = orientation >= 5;
        QCOMPARE(loaded.info.width, turned ? 8 : 16);
        QCOMPARE(loaded.info.height, turned ? 16 : 8);
        QCOMPARE(loaded.image.size(), QSize(loaded.info.width, loaded.info.height));
        const int x = redRight ? loaded.info.width - 2 : 1;
        const int y = redBottom ? loaded.info.height - 2 : 1;
        const QColor corner = loaded.image.pixelColor(x, y);
        QVERIFY2(corner.red() > 200 && corner.blue() < 60, qPrintable(corner.name()));
    }

    void sixteenBit_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("format");
        QTest::newRow("PNG") << QStringLiteral("deep.png") << QByteArray("PNG");
        QTest::newRow("TIFF") << QStringLiteral("deep.tif") << QByteArray("TIFF");
    }

    void sixteenBit()
    {
        QFETCH(QString, name);
        QFETCH(QByteArray, format);
        QTemporaryDir dir;
        const QString path = dir.filePath(name);
        QImage deep(4, 2, QImage::Format_Grayscale16);
        for (int x = 0; x < 4; ++x) {
            reinterpret_cast<quint16*>(deep.scanLine(0))[x] = quint16(x * 21845);  // 0 .. 65535
            reinterpret_cast<quint16*>(deep.scanLine(1))[x] = 65535;
        }
        QVERIFY2(deep.save(path, format.constData()), "no writer for this format (Qt image formats plugin missing?)");
        QImageReader probe(path);
        QCOMPARE(probe.read().format(), QImage::Format_Grayscale16);

        const LoadedImage loaded = loadImage(path);
        QVERIFY2(!loaded.image.isNull(), qPrintable(loaded.error));
        QCOMPARE(loaded.info.width, 4);
        QCOMPARE(loaded.info.height, 2);
        QCOMPARE(loaded.image.depth(), 8);
        compareInfo(path, loaded);
        QCOMPARE(qGray(loaded.image.pixel(0, 0)), 0);
        QCOMPARE(qGray(loaded.image.pixel(3, 0)), 255);
        QVERIFY(qAbs(qGray(loaded.image.pixel(1, 0)) - 85) <= 1);
    }

    void multiPageTiffShowsTheFirstPage()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("pages.tiff"));
        writeFile(path, multiPageTiff({QSize(6, 4), QSize(3, 3)}));
        const LoadedImage loaded = loadImage(path);
        QVERIFY2(!loaded.image.isNull(), qPrintable(loaded.error));
        QVERIFY(loaded.hasMoreFrames);
        compareInfo(path, loaded);
        QCOMPARE(loaded.info.width, 6);
        QCOMPARE(loaded.info.height, 4);
    }

    // Format spec 9.1: the content decides, not the extension.
    void contentNotMatchingTheExtension()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("really_png.jpg"));
        QImage image(5, 3, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(path, "PNG"));
        const LoadedImage loaded = loadImage(path);
        QVERIFY2(!loaded.image.isNull(), qPrintable(loaded.error));
        QCOMPARE(loaded.info.width, 5);
        QCOMPARE(loaded.info.height, 3);
        QCOMPARE(loaded.info.fileName, QStringLiteral("really_png.jpg"));
        compareInfo(path, loaded);
    }

    void notAnImage()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("broken.png"));
        writeFile(path, "not an image");
        const LoadedImage loaded = loadImage(path);
        QVERIFY(loaded.image.isNull());
        QVERIFY(!loaded.error.isEmpty());
        QVERIFY(!readImageInfo(path));
    }
};

QTEST_MAIN(TestImageLoader)
#include "tst_imageloader.moc"
