// Listing the images of a data root (format spec 2, 9.1) and their order.

#include "dataset/DirectoryScanner.h"
#include "dataset/NaturalSort.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

    // Format spec 3, 11: names are kept as they are on disk; Chinese names scan like others,
    // and names that differ only in Unicode normalization collide.
    void unicodeNames()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        const QString chinese = QStringLiteral(u"产线一/划痕_001.png");
        touch(root, chinese);
        const QString nfc = QStringLiteral(u"l\u00e9.png");   // é as one code point
        const QString nfd = QStringLiteral(u"le\u0301.png");  // e + combining acute
        touch(root, QStringLiteral(u"u/") + nfc);
        touch(root, QStringLiteral(u"u/") + nfd);
        touch(root, QStringLiteral(u"u/plain.png"));

        const ScanResult result = scanDirectory(dir.path());
        QStringList found = paths(result);
        QVERIFY2(found.contains(chinese), qPrintable(found.join(QLatin1Char(' '))));
        const QStringList u = found.filter(QStringLiteral("u/"));
        // Some file systems store one normalization form only; then both names are one file.
        if (QDir(root.filePath(QStringLiteral("u"))).entryList(QDir::Files).size() < 3)
            QSKIP("this file system normalizes names");
        QCOMPARE(u.size(), 3);
        for (const ScannedImage& image : result.images) {
            const bool accented = image.path != QStringLiteral("u/plain.png") && image.path.startsWith(QLatin1String("u/"));
            QCOMPARE(image.nameCollision, accented);
            // The characters on disk, not a normalized form.
            if (accented)
                QVERIFY(image.path == QStringLiteral(u"u/") + nfc || image.path == QStringLiteral(u"u/") + nfd);
        }
    }

    // Windows attributes: system alone, hidden alone, on files and on directories.
    void windowsAttributes()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows file attributes");
#else
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("kept.png"));
        touch(root, QStringLiteral("system.png"));
        touch(root, QStringLiteral("hidden.png"));
        touch(root, QStringLiteral("systemdir/a.png"));
        touch(root, QStringLiteral("hiddendir/b.png"));
        const auto set = [&root](const QString& name, DWORD attribute) {
            const QString path = QDir::toNativeSeparators(root.filePath(name));
            QVERIFY(SetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()), attribute));
        };
        set(QStringLiteral("system.png"), FILE_ATTRIBUTE_SYSTEM);
        set(QStringLiteral("hidden.png"), FILE_ATTRIBUTE_HIDDEN);
        set(QStringLiteral("systemdir"), FILE_ATTRIBUTE_SYSTEM);
        set(QStringLiteral("hiddendir"), FILE_ATTRIBUTE_HIDDEN);

        const ScanResult result = scanDirectory(dir.path());
        QCOMPARE(paths(result), QStringList{QStringLiteral("kept.png")});

        // Removable again by QTemporaryDir.
        set(QStringLiteral("system.png"), FILE_ATTRIBUTE_NORMAL);
        set(QStringLiteral("hidden.png"), FILE_ATTRIBUTE_NORMAL);
        set(QStringLiteral("systemdir"), FILE_ATTRIBUTE_NORMAL);
        set(QStringLiteral("hiddendir"), FILE_ATTRIBUTE_NORMAL);
#endif
    }
};

QTEST_GUILESS_MAIN(TestDirectoryScanner)
#include "tst_directoryscanner.moc"
