// Listing the images of a data root (format spec 2, 9.1) and their order.

#include "dataset/DirectoryScanner.h"
#include "dataset/NaturalSort.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace liteanno::dataset;

namespace {

void touch(const QDir& root, const QString& path)
{
    root.mkpath(QFileInfo(root.filePath(path)).path());
    QFile file(root.filePath(path));
    if (!file.open(QIODevice::WriteOnly))
        qFatal("cannot create %s", qPrintable(path));
}

QStringList paths(const ScanResult& result)
{
    QStringList list;
    for (const ScannedImage& image : result.images)
        list.append(image.path);
    return list;
}

} // namespace

class TestDirectoryScanner : public QObject
{
    Q_OBJECT

private slots:
    void naturalOrder()
    {
        QStringList names = {QStringLiteral("img10.jpg"), QStringLiteral("img2.jpg"), QStringLiteral("IMG1.jpg"),
                             QStringLiteral("a/b.jpg"), QStringLiteral("a-b.jpg"), QStringLiteral("img02.jpg")};
        std::sort(names.begin(), names.end(), naturalLess);
        const QStringList expected = {QStringLiteral("a/b.jpg"), QStringLiteral("a-b.jpg"), QStringLiteral("IMG1.jpg"),
                                      QStringLiteral("img02.jpg"), QStringLiteral("img2.jpg"), QStringLiteral("img10.jpg")};
        QCOMPARE(names, expected);
    }

    void scan()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("line1/10.bmp"));
        touch(root, QStringLiteral("line1/9.BMP"));
        touch(root, QStringLiteral("line1/9.json"));
        touch(root, QStringLiteral("line1/notes.txt"));
        touch(root, QStringLiteral("line2/a.jpg"));
        touch(root, QStringLiteral("line2/a.png"));
        touch(root, QStringLiteral("line2/raw.dng"));
        touch(root, QStringLiteral("liteanno/x.png"));
        touch(root, QStringLiteral("sub/liteanno/kept.png"));
        touch(root, QStringLiteral("x-anylabeling-exif-backup/old.jpg"));
        touch(root, QStringLiteral(".hidden/h.jpg"));
        touch(root, QStringLiteral("line1/.xal_1.tmp.jpg"));

        const ScanResult result = scanDirectory(dir.path());
        const QStringList expected = {QStringLiteral("line1/9.BMP"), QStringLiteral("line1/10.bmp"),
                                      QStringLiteral("line2/a.jpg"), QStringLiteral("line2/a.png"),
                                      QStringLiteral("sub/liteanno/kept.png")};
        QCOMPARE(paths(result), expected);
        QVERIFY(!result.images[0].nameCollision);
        // line2/a.jpg and line2/a.png share a.json.
        QVERIFY(result.images[2].nameCollision);
        QVERIFY(result.images[3].nameCollision);
        QCOMPARE(result.unsupported, QStringList{QStringLiteral("line2/raw.dng")});
    }
};

QTEST_GUILESS_MAIN(TestDirectoryScanner)
#include "tst_directoryscanner.moc"
