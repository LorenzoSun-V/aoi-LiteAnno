#include "dataset/DirectoryScanner.h"

#include "dataset/NaturalSort.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QDateTime>
#include <QSet>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif

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

// Format spec 3: entries with the hidden or the system attribute are not scanned. Qt's
// QDir::System filter is about device and socket files, not the Windows attribute, so the
// attributes are read here.
bool hasHiddenOrSystemAttribute(const QFileInfo& entry)
{
#ifdef Q_OS_WIN
    const DWORD attributes =
        GetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(entry.filePath()).utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
#else
    Q_UNUSED(entry);
    return false;
#endif
}

FileVersion versionOf(const QFileInfo& entry)
{
    return {entry.size(), entry.lastModified().toMSecsSinceEpoch()};
}

// Format spec 2: same directory, same stem, ".json" in any case. The stem follows the file
// system: without case on Windows, with case elsewhere. Of several candidates (only possible
// where case matters) preferredAnnotation() picks.
QString stemKey(const QString& stem)
{
#ifdef Q_OS_WIN
    return stem.toCaseFolded();
#else
    return stem;
#endif
}

// QDir lists nothing both for an empty folder and for one it cannot read; asked only when the
// listing came back empty.
bool cannotList(const QString& path)
{
#ifdef Q_OS_WIN
    WIN32_FIND_DATAW data;
    const QString pattern = QDir::toNativeSeparators(path) + QLatin1String("\\*");
    const HANDLE find = FindFirstFileExW(reinterpret_cast<const wchar_t*>(pattern.utf16()), FindExInfoBasic, &data,
                                         FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() != ERROR_FILE_NOT_FOUND;
    FindClose(find);
    return false;
#else
    DIR* dir = opendir(QFile::encodeName(path).constData());
    if (!dir)
        return true;
    closedir(dir);
    return false;
#endif
}

// Of two annotation files for one image, the one the editor opens and saves
// (formats::existingAnnotationPath): the lower-case extension where case matters, otherwise
// the first by name.
bool preferredAnnotation(const QString& candidate, const QString& current)
{
#ifndef Q_OS_WIN
    const bool candidateLower = candidate.endsWith(QLatin1String(".json"));
    const bool currentLower = current.endsWith(QLatin1String(".json"));
    if (candidateLower != currentLower)
        return candidateLower;
#endif
    return candidate < current;
}

class Scanner
{
public:
    Scanner(const QDir& root, const ScanOptions& options, ScanResult& result)
        : m_root(root)
        , m_rootLength(QDir::toNativeSeparators(root.absolutePath()).size())
        , m_options(options)
        , m_result(result)
    {
    }

    // False when cancelled.
    bool scan(const QString& relativeDir)
    {
        if (m_options.cancelled && m_options.cancelled())
            return false;
        const QDir dir(relativeDir.isEmpty() ? m_root.path() : m_root.filePath(relativeDir));
        // Everything is listed and the attributes are checked below.
        const QFileInfoList entries =
            dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::NoSort);
        if (entries.isEmpty() && cannotList(dir.path())) {
            m_result.unreadable.append(relativeDir.isEmpty() ? QStringLiteral(".") : relativeDir);
            return true;
        }
        const QString prefix = relativeDir.isEmpty() ? QString() : relativeDir + QLatin1Char('/');
        QStringList directories;
        QList<qsizetype> images;  // indexes into m_result.images
        QHash<QString, QFileInfo> annotations;  // by stem key
        for (const QFileInfo& entry : entries) {
            const QString name = entry.fileName();
            if (name.startsWith(QLatin1Char('.')) || entry.isHidden() || hasHiddenOrSystemAttribute(entry))
                continue;
            if (!entry.isDir() && !entry.isFile())
                continue;  // sockets, devices, broken links
            if (entry.isDir()) {
                if (entry.isSymLink())
                    continue;
                if ((relativeDir.isEmpty() && name == QLatin1String("liteanno"))
                    || name == QLatin1String("x-anylabeling-exif-backup"))
                    continue;
                directories.append(prefix + name);
                continue;
            }
            const QString suffix = suffixOf(name);
            if (kImageSuffixes.contains(suffix)) {
                ScannedImage image;
                image.path = prefix + name;
                image.image = versionOf(entry);
                images.append(m_result.images.size());
                m_result.images.append(image);
            } else if (kUnsupportedSuffixes.contains(suffix)) {
                m_result.unsupported.append(prefix + name);
            } else if (suffix == QLatin1String("json")) {
                const QString key = stemKey(name.left(name.size() - 5));
                const auto it = annotations.find(key);
                if (it == annotations.end() || preferredAnnotation(name, it->fileName()))
                    annotations.insert(key, entry);
            }
        }
        QSet<QString> paired;
        for (const qsizetype index : std::as_const(images)) {
            ScannedImage& image = m_result.images[index];
            const QString name = image.path.mid(prefix.size());
            const QString stem = name.left(name.lastIndexOf(QLatin1Char('.')));
            const QString key = stemKey(stem);
            const auto it = annotations.constFind(key);
            if (it != annotations.cend()) {
                image.annotationName = it->fileName();
                image.annotation = versionOf(*it);
                paired.insert(key);
            }
            // The root, a separator, and the identifier with native separators; the
            // annotation file counts too, also one still to be written.
            const qsizetype jsonLength =
                image.annotationName.isEmpty() ? stem.size() + 5 : image.annotationName.size();
            if (m_rootLength + 1 + prefix.size() + std::max(name.size(), jsonLength) > 259)
                m_result.longPaths.append(image.path);
        }
        for (auto it = annotations.cbegin(); it != annotations.cend(); ++it) {
            if (!paired.contains(it.key()))
                m_result.orphanAnnotations.append(prefix + it->fileName());
        }
        if (m_options.progress)
            m_options.progress(int(m_result.images.size()));
        for (const QString& sub : std::as_const(directories)) {
            if (!scan(sub))
                return false;
        }
        return true;
    }

private:
    const QDir m_root;
    const qsizetype m_rootLength;
    const ScanOptions& m_options;
    ScanResult& m_result;
};

} // namespace

bool isImageFileName(const QString& fileName)
{
    return kImageSuffixes.contains(suffixOf(fileName));
}

ScanResult scanDirectory(const QString& root, const ScanOptions& options)
{
    ScanResult result;
    result.cancelled = !Scanner(QDir(root), options, result).scan(QString());
    result.complete = !result.cancelled && result.unreadable.isEmpty();

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

    sortNaturally(result.images, [](const ScannedImage& image) -> const QString& { return image.path; });
    sortNaturally(result.unsupported);
    sortNaturally(result.orphanAnnotations);
    sortNaturally(result.longPaths);
    sortNaturally(result.unreadable);
    return result;
}

} // namespace liteanno::dataset
