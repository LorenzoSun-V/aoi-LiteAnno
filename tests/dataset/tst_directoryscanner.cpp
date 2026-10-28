// Listing the images of a data root (format spec 2, 9.1) and their order.

#include "common/FolderBlocker.h"
#include "core/FolderCase.h"
#include "dataset/DirectoryScanner.h"
#include "dataset/NaturalSort.h"
#include "formats/AnnotationStore.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

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

#ifdef Q_OS_WIN
// Sets an empty folder to tell case apart in names (what fsutil file setCaseSensitiveInfo
// does). False where Windows or the file system cannot.
bool makeCaseSensitive(const QString& folder)
{
    const QString native = QDir::toNativeSeparators(folder);
    const HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    struct {
        ULONG flags;
    } info{0x1};  // FILE_CASE_SENSITIVE_INFO, FILE_CS_FLAG_CASE_SENSITIVE_DIR
    const bool set = SetFileInformationByHandle(handle, static_cast<FILE_INFO_BY_HANDLE_CLASS>(23), &info, sizeof(info));
    CloseHandle(handle);
    if (!set)
        return false;
    // Set and in effect: a name in other case is another file.
    touch(QDir(folder), QStringLiteral("probe.txt"));
    const bool sensitive = !QFileInfo::exists(folder + QStringLiteral("/PROBE.TXT"));
    QFile::remove(folder + QStringLiteral("/probe.txt"));
    return sensitive;
}
#endif

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

    // Architecture 6.1: the same listing pairs the annotation files (format spec 2) and
    // gives the sizes and times of both files.
    void annotationsFromTheSameListing()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("d/a.png"));
        touch(root, QStringLiteral("d/a.JSON"));
        touch(root, QStringLiteral("d/b.c.jpg"));
        touch(root, QStringLiteral("d/b.c.json"));
        touch(root, QStringLiteral("d/b.json"));  // stem "b" has no image
        touch(root, QStringLiteral("d/c.png"));
        touch(root, QStringLiteral("e/c.json"));  // another directory
        {
            QFile file(root.filePath(QStringLiteral("d/b.c.json")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{}");
        }
        const ScanResult result = scanDirectory(dir.path());
        QVERIFY(result.complete);
        QCOMPARE(paths(result), QStringList({QStringLiteral("d/a.png"), QStringLiteral("d/b.c.jpg"), QStringLiteral("d/c.png")}));
        QCOMPARE(result.images[0].annotationName, QStringLiteral("a.JSON"));
        QCOMPARE(result.images[1].annotationName, QStringLiteral("b.c.json"));
        QCOMPARE(result.images[1].annotation.size, qint64(2));
        QCOMPARE(result.images[1].annotation.modifiedMs,
                 QFileInfo(root.filePath(QStringLiteral("d/b.c.json"))).lastModified().toMSecsSinceEpoch());
        QCOMPARE(result.images[1].image.size, qint64(0));
        QVERIFY(result.images[2].annotationName.isEmpty());
        QVERIFY(!result.images[2].annotation.exists());
        QCOMPARE(result.orphanAnnotations, QStringList({QStringLiteral("d/b.json"), QStringLiteral("e/c.json")}));
    }

    // Format spec 2: the stem follows the file system, the extension never matters.
    void stemCaseFollowsTheFileSystem()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("a.png"));
        touch(root, QStringLiteral("A.json"));
        const ScanResult result = scanDirectory(dir.path());
        QCOMPARE(result.images.size(), 1);
#ifdef Q_OS_WIN
        QCOMPARE(result.images[0].annotationName, QStringLiteral("A.json"));
        QVERIFY(result.orphanAnnotations.isEmpty());
#else
        QVERIFY(result.images[0].annotationName.isEmpty());
        QCOMPARE(result.orphanAnnotations, QStringList{QStringLiteral("A.json")});
#endif
    }

    void cancelledScanIsIncomplete()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        for (int i = 0; i < 5; ++i)
            touch(root, QStringLiteral("d%1/a.png").arg(i));
        int directories = 0;
        QList<int> progress;
        ScanOptions options;
        options.cancelled = [&directories] { return ++directories > 3; };
        options.progress = [&progress](int found) { progress.append(found); };
        const ScanResult result = scanDirectory(dir.path(), options);
        QVERIFY(!result.complete);
        QVERIFY(result.images.size() < 5);
        QVERIFY(!progress.isEmpty());
        QVERIFY(scanDirectory(dir.path()).complete);
    }

    // The scan pairs the file the editor opens and saves (formats::existingAnnotationPath),
    // also where several files differ only in the case of the extension.
    // The check for one image agrees with the full scan.
    void nameCollisionOfOneImage()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("a.png"));
        touch(root, QStringLiteral("a.json"));
        touch(root, QStringLiteral("b.png"));
        touch(root, QStringLiteral("b.txt"));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("a.png"))));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("b.png"))));
        touch(root, QStringLiteral("A.JPG"));
        QVERIFY(hasNameCollision(root.filePath(QStringLiteral("a.png"))));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("b.png"))));
        for (const ScannedImage& image : scanDirectory(root.path()).images)
            QCOMPARE(hasNameCollision(root.filePath(image.path)), image.nameCollision);
    }

    // The check for one image reads names straight from the folder (it runs for every image
    // an import writes): the same answers as the full comparison of format spec 3.
    void nameCollisionWithoutFileInformation()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        const QString nfc = QStringLiteral(u"café");
        const QString nfd = QStringLiteral(u"café");
        touch(root, nfc + QStringLiteral(".png"));
        touch(root, QStringLiteral(u"K.png"));  // KELVIN SIGN folds to "k"
        touch(root, QStringLiteral("x.png"));
        touch(root, QStringLiteral("x.png.txt"));
        touch(root, QStringLiteral("x.pngx"));
        touch(root, QStringLiteral(".png"));
        touch(root, QStringLiteral("x.JpEg/inside.txt"));  // a folder, not an image
        QVERIFY(!hasNameCollision(root.filePath(nfc + QStringLiteral(".png"))));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral(u"K.png"))));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("x.png"))));
        touch(root, nfd + QStringLiteral(".JPG"));
        touch(root, QStringLiteral("k.bmp"));
        touch(root, QStringLiteral("X.TIFF"));
        QVERIFY(hasNameCollision(root.filePath(nfc + QStringLiteral(".png"))));
        QVERIFY(hasNameCollision(root.filePath(QStringLiteral(u"K.png"))));
        QVERIFY(hasNameCollision(root.filePath(QStringLiteral("x.png"))));
        for (const ScannedImage& image : scanDirectory(root.path()).images)
            QCOMPARE(hasNameCollision(root.filePath(image.path)), image.nameCollision);
        // A folder that cannot be listed: nothing says the annotation file may be written.
        QVERIFY(hasNameCollision(root.filePath(QStringLiteral("missing/a.png"))));
    }

    // The search by name on Windows: '?' for every character a name not in ASCII folds to,
    // checked against every code point with the comparison of format spec 3. Such names are
    // one UTF-16 unit and give one ASCII character, so the '?' covers them in place.
    void nameQueryCoversEveryFold()
    {
        const auto ascii = [](const QString& text) {
            return !text.isEmpty() && std::all_of(text.begin(), text.end(), [](QChar c) { return c.unicode() < 0x80; });
        };
        int folds = 0;
        for (char32_t cp = 0x80; cp <= 0x10FFFF; ++cp) {
            if (cp >= 0xD800 && cp <= 0xDFFF)
                continue;
            const QString other = QString::fromUcs4(&cp, 1);
            const QString key = other.normalized(QString::NormalizationForm_C).toCaseFolded();
            if (!ascii(key))
                continue;
            ++folds;
            QVERIFY2(other.size() == 1 && key.size() == 1, qPrintable(QString::number(uint(cp), 16)));
            for (const QString& stem : {key, key.toUpper(), QStringLiteral("x") + key + QStringLiteral("y")})
                QVERIFY2(nameQueryPattern(stem) == QString(stem).replace(key, QStringLiteral("?"), Qt::CaseInsensitive) + QStringLiteral(".*"),
                         qPrintable(QString::number(uint(cp), 16) + QLatin1Char(' ') + nameQueryPattern(stem)));
        }
        QVERIFY(folds >= 4);
        QCOMPARE(nameQueryPattern(u"SES-260713-7_0"), QStringLiteral("?E?-260713-7_0.*"));
        QCOMPARE(nameQueryPattern(u"a.b"), QStringLiteral("a.b.*"));
        QCOMPARE(nameQueryPattern(u"abc;`"), QStringLiteral("abc??.*"));
        QVERIFY(nameQueryPattern(u"").isEmpty());
        QVERIFY(nameQueryPattern(u"划痕_001").isEmpty());
        QVERIFY(nameQueryPattern(u"caf\u00e9").isEmpty());
    }

    // What the search finds is filtered as a listing is, and names in and out of ASCII meet:
    // "a.*" also finds "a.b.png", "a.png.txt" and a folder named "A.jpg", none of them an
    // image of stem "a"; ASCII stems collide with names that fold to them, Chinese ones with
    // the same characters.
    void nameCollisionAcrossScripts()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("a.png"));
        touch(root, QStringLiteral("a.b.png"));
        touch(root, QStringLiteral("a.png.txt"));
        touch(root, QStringLiteral("a.json"));
        touch(root, QStringLiteral("A.jpg/inside.txt"));
        touch(root, QStringLiteral("ab.png"));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("a.png"))));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("a.b.png"))));

        const QString ascii = QStringLiteral("SES-1_k;`");
        const QString folded = QStringLiteral(u"\u017FE\u017F-1_\u212A\u037E\u1FEF");  // ſ, KELVIN SIGN, GREEK QUESTION MARK, VARIA
        const QString chinese = QStringLiteral(u"划痕_001");
        touch(root, ascii + QStringLiteral(".png"));
        touch(root, chinese + QStringLiteral(".png"));
        touch(root, QStringLiteral("SES-1_k.png"));
        QVERIFY(!hasNameCollision(root.filePath(ascii + QStringLiteral(".png"))));
        QVERIFY(!hasNameCollision(root.filePath(chinese + QStringLiteral(".png"))));
        touch(root, folded + QStringLiteral(".jpg"));
        touch(root, chinese + QStringLiteral(".BMP"));
        if (QDir(root.path()).entryList({folded + QStringLiteral(".jpg")}, QDir::Files).isEmpty())
            QSKIP("this file system changes such names");
        QVERIFY(hasNameCollision(root.filePath(ascii + QStringLiteral(".png"))));
        QVERIFY(hasNameCollision(root.filePath(folded + QStringLiteral(".jpg"))));
        QVERIFY(hasNameCollision(root.filePath(chinese + QStringLiteral(".png"))));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("SES-1_k.png"))));
        for (const ScannedImage& image : scanDirectory(root.path()).images)
            QCOMPARE(hasNameCollision(root.filePath(image.path)), image.nameCollision);
    }

    // A Windows folder set to tell case apart: the search would compare with case, so the
    // folder is listed and "AB.jpg" still collides with "ab.png" (format spec 3).
    void nameCollisionInCaseSensitiveFolder()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows folders set to tell case apart");
#else
        QTemporaryDir dir;
        const QDir root(dir.path());
        QVERIFY(root.mkdir(QStringLiteral("cs")));
        if (!makeCaseSensitive(root.filePath(QStringLiteral("cs"))))
            QSKIP("this Windows cannot set a folder to tell case apart");
        touch(root, QStringLiteral("cs/ab.png"));
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("cs/ab.png"))));
        touch(root, QStringLiteral("cs/AB.jpg"));
        QVERIFY(hasNameCollision(root.filePath(QStringLiteral("cs/ab.png"))));
        QVERIFY(hasNameCollision(root.filePath(QStringLiteral("cs/AB.jpg"))));
        QCOMPARE(liteanno::folderCase(root.filePath(QStringLiteral("cs"))), liteanno::FolderCase::Sensitive);

        // Review of #33: a query Windows does not support proves nothing about the folder, so
        // the folder is still listed and the collision found.
        const auto restore = qScopeGuard([] { liteanno::failFolderCaseQueryForTesting(0); });
        for (const DWORD error : {DWORD(ERROR_INVALID_PARAMETER), DWORD(ERROR_NOT_SUPPORTED), DWORD(ERROR_INVALID_FUNCTION),
                                  DWORD(ERROR_ACCESS_DENIED)}) {
            liteanno::failFolderCaseQueryForTesting(error);
            QVERIFY(liteanno::folderCase(root.filePath(QStringLiteral("cs"))) != liteanno::FolderCase::Insensitive);
            QVERIFY2(hasNameCollision(root.filePath(QStringLiteral("cs/ab.png"))), qPrintable(QString::number(error)));
            QVERIFY2(hasNameCollision(root.filePath(QStringLiteral("cs/AB.jpg"))), qPrintable(QString::number(error)));
        }
#endif
    }

    void pairingMatchesTheEditor()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        for (const char* name : {"a.png", "a.json", "a.JSON", "b.png", "b.Json", "b.JSON", "c.png", "c.JSON"})
            touch(root, QLatin1String(name));
        if (QDir(dir.path()).entryList(QDir::Files).size() < 8)
            QSKIP("this file system ignores case");
        const ScanResult result = scanDirectory(dir.path());
        QCOMPARE(result.images.size(), 3);
        for (const ScannedImage& image : result.images) {
            const QString stem = image.path.left(image.path.lastIndexOf(QLatin1Char('.')));
            const QString editor = liteanno::formats::existingAnnotationPath(root.filePath(stem + QLatin1String(".json")));
            QCOMPARE(image.annotationName, QFileInfo(editor).fileName());
        }
    }

    // A folder that cannot be listed says nothing about its images: the scan is incomplete
    // and names the folder.
    void unreadableFolderMakesTheScanIncomplete()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        touch(root, QStringLiteral("ok/a.png"));
        touch(root, QStringLiteral("locked/b.png"));
        liteanno::test::FolderBlocker blocker(root.filePath(QStringLiteral("locked")));
        if (!blocker.isActive())
            QSKIP("cannot take the right to list a folder away here (running as root?)");
        const ScanResult result = scanDirectory(dir.path());
        blocker.release();
        QVERIFY(!result.complete);
        QVERIFY(!result.cancelled);
        QCOMPARE(paths(result), QStringList{QStringLiteral("ok/a.png")});
        QCOMPARE(result.unreadable, QStringList{QStringLiteral("locked")});
        QVERIFY(scanDirectory(dir.path()).complete);
    }

    // Format spec 2: the annotation file's path counts too; ".json" is longer than ".png".
    void longAnnotationPaths()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        const int rootLength = int(QDir::toNativeSeparators(root.absolutePath()).size());
        // Image 259 characters, its annotation file 260.
        const QString stem = QString(259 - rootLength - 1 - 4, QLatin1Char('a'));
        if (stem.size() + 5 > 255)
            QSKIP("the temporary folder is too short for a name of 255 characters at most");
        touch(root, stem + QLatin1String(".png"));
        ScanResult result = scanDirectory(dir.path());
        QCOMPARE(result.longPaths, QStringList{stem + QLatin1String(".png")});
        touch(root, stem + QLatin1String(".json"));
        result = scanDirectory(dir.path());
        QCOMPARE(result.longPaths, QStringList{stem + QLatin1String(".png")});
    }

    // Format spec 2: absolute paths over 259 characters are reported.
    void longPaths()
    {
        QTemporaryDir dir;
        const QDir root(dir.path());
        const int rootLength = int(QDir::toNativeSeparators(root.absolutePath()).size());
        // The annotation file to be written ("<stem>.json") has 259 characters.
        const QString fits = QString(259 - rootLength - 1 - 5, QLatin1Char('a')) + QLatin1String(".png");
        const QString over = QString(260 - rootLength - 1 - 4, QLatin1Char('b')) + QLatin1String(".png");
        if (over.size() > 255)
            QSKIP("the temporary folder is too short for a name of 255 characters at most");
        touch(root, fits);
        touch(root, over);
        const ScanResult result = scanDirectory(dir.path());
        QCOMPARE(result.images.size(), 2);
        QCOMPARE(result.longPaths, QStringList{over});
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
        // Hidden and system images do not collide with the one that is scanned.
        touch(root, QStringLiteral("KEPT.jpg"));
        set(QStringLiteral("KEPT.jpg"), FILE_ATTRIBUTE_HIDDEN);
        touch(root, QStringLiteral("kept.bmp"));
        set(QStringLiteral("kept.bmp"), FILE_ATTRIBUTE_SYSTEM);
        QVERIFY(!hasNameCollision(root.filePath(QStringLiteral("kept.png"))));
        set(QStringLiteral("KEPT.jpg"), FILE_ATTRIBUTE_NORMAL);
        QVERIFY(hasNameCollision(root.filePath(QStringLiteral("kept.png"))));
        set(QStringLiteral("kept.bmp"), FILE_ATTRIBUTE_NORMAL);

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
