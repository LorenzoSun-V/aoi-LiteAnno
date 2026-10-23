#include "core/BundledLicenses.h"

#include <QDirIterator>
#include <QFile>

namespace liteanno {

namespace {

const QString kLicenses = QStringLiteral(":/licenses");

} // namespace

QStringList bundledLicenseNames()
{
    QStringList names;
    QDirIterator it(kLicenses, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        names.append(it.next().mid(kLicenses.size() + 1));
    names.sort();
    return names;
}

QString bundledLicenseText(const QString& name)
{
    QFile file(kLicenses + QLatin1Char('/') + name);
    if (name.isEmpty() || !file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QStringList missingRequiredLicenses()
{
    QStringList missing;
    for (const char* name :
         {"qt-LGPL-3.0-only.txt", "qt-GPL-3.0-only.txt", "nlohmann-json-LICENSE.MIT", "opencv-LICENSE",
          "libjpeg-LICENSE", "libjpeg-ijg-license.txt", "libjpeg-COPYRIGHT.txt", "zlib-LICENSE"}) {
        if (bundledLicenseText(QString::fromLatin1(name)).trimmed().isEmpty())
            missing.append(QString::fromLatin1(name));
    }
    return missing;
}

} // namespace liteanno
