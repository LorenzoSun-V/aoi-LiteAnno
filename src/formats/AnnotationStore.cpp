#include "formats/AnnotationStore.h"

#include <QFile>
#include <QSaveFile>

namespace liteanno::formats {

AnnotationFile loadAnnotationFile(const QString& jsonPath, const ImageInfo& image,
                                  const QStringList& imageLabelOptions)
{
    QFile file(jsonPath);
    if (!file.exists())
        return AnnotationFile::createNew(image, imageLabelOptions);
    if (!file.open(QIODevice::ReadOnly))
        return AnnotationFile::unreadable(image, file.errorString());
    return AnnotationFile::fromBytes(file.readAll(), image);
}

SaveResult saveAnnotationFile(const QString& jsonPath, AnnotationFile& file)
{
    if (file.isReadOnly())
        return {SaveStatus::ReadOnly, QString::fromLatin1(readOnlyReasonName(file.readOnlyReason()))};
    if (!file.needsWrite())
        return {SaveStatus::NotModified, QString()};

    const QByteArray bytes = file.toBytes();
    QSaveFile out(jsonPath);
    if (!out.open(QIODevice::WriteOnly))
        return {SaveStatus::Failed, out.errorString()};
    if (out.write(bytes) != bytes.size()) {
        const QString error = out.errorString();
        out.cancelWriting();
        return {SaveStatus::Failed, error};
    }
    if (!out.commit())
        return {SaveStatus::Failed, out.errorString()};

    file.markSaved();
    return {SaveStatus::Saved, QString()};
}

} // namespace liteanno::formats
