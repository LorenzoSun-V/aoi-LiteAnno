#include "dataset/DirectoryScanner.h"

#include "dataset/NaturalSort.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace liteanno::dataset {

namespace {

const QStringList kImageSuffixes = {
    QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jpe"), QStringLiteral("jfif"),
    QStringLiteral("png"), QStringLiteral("bmp"),  QStringLiteral("dib"), QStringLiteral("tif"),
    QStringLiteral("tiff"), QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("pbm"),
    QStringLiteral("pgm"), QStringLiteral("ppm"),
};

const QStringList kUnsupportedSuffixes = {
    QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("avif"), QStringLiteral("jp2"),
    QStringLiteral("j2k"),  QStringLiteral("dng"),  QStringLiteral("cr2"),  QStringLiteral("nef"),
    QStringLiteral("arw"),
};

QString suffixOf(const QString& fileName)
{
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? fileName.mid(dot + 1).toLower() : QString();
}

// Format spec 3: NFC, then simple case folding.
QString comparisonKey(const QString& path)
{
    return path.normalized(QString::NormalizationForm_C).toCaseFolded();
}

void scanInto(const QDir& root, const QString& relativeDir, ScanResult& result)
{
    const QDir dir(relativeDir.isEmpty() ? root.path() : root.filePath(relativeDir));
    // Without QDir::Hidden and QDir::System, hidden and system entries are left out.
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo& entry : entries) {
        const QString name = entry.fileName();
        if (name.startsWith(QLatin1Char('.')))
            continue;
        const QString relative = relativeDir.isEmpty() ? name : relativeDir + QLatin1Char('/') + name;
        if (entry.isDir()) {
            if (entry.isSymLink())
                continue;
            if ((relativeDir.isEmpty() && name == QLatin1String("liteanno"))
                || name == QLatin1String("x-anylabeling-exif-backup"))
                continue;
            scanInto(root, relative, result);
        } else if (isImageFileName(name)) {
            result.images.append({relative, false});
        } else if (kUnsupportedSuffixes.contains(suffixOf(name))) {
            result.unsupported.append(relative);
        }
    }
}

} // namespace

bool isImageFileName(const QString& fileName)
{
    return kImageSuffixes.contains(suffixOf(fileName));
}

ScanResult scanDirectory(const QString& root)
{
    ScanResult result;
    scanInto(QDir(root), QString(), result);

    // STEM_COLLISION: images whose annotation files would be the same file.
    // NAME_COLLISION: different paths with the same comparison key.
    QHash<QString, int> stems;
    QHash<QString, int> keys;
    for (const ScannedImage& image : std::as_const(result.images)) {
        const qsizetype dot = image.path.lastIndexOf(QLatin1Char('.'));
        ++stems[comparisonKey(image.path.left(dot))];
        ++keys[comparisonKey(image.path)];
    }
    for (ScannedImage& image : result.images) {
        const qsizetype dot = image.path.lastIndexOf(QLatin1Char('.'));
        image.nameCollision = stems.value(comparisonKey(image.path.left(dot))) > 1
                              || keys.value(comparisonKey(image.path)) > 1;
    }

    std::sort(result.images.begin(), result.images.end(),
              [](const ScannedImage& a, const ScannedImage& b) { return naturalLess(a.path, b.path); });
    std::sort(result.unsupported.begin(), result.unsupported.end(), naturalLess);
    return result;
}

} // namespace liteanno::dataset
