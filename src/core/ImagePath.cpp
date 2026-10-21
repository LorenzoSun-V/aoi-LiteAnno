#include "core/ImagePath.h"

#include <QStringList>

namespace liteanno {

bool isValidImagePath(const QString& path)
{
    if (path.isEmpty() || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))
        || path.contains(QChar(0)))
        return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString& segment : segments) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String(".."))
            return false;
    }
    return true;
}

QString annotationPathForImage(const QString& imagePath)
{
    const qsizetype slash = imagePath.lastIndexOf(QLatin1Char('/'));
    const qsizetype dot = imagePath.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > slash + 1 ? imagePath.left(dot) : imagePath;
    return stem + QLatin1String(".json");
}

} // namespace liteanno
