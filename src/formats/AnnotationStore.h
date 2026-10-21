#pragma once

#include "formats/AnnotationFile.h"

#include <QString>
#include <QStringList>

namespace liteanno::formats {

// Reads the annotation file next to an image. A missing file gives a new document; a file
// that exists but cannot be read gives a read-only one, so it is never overwritten.
AnnotationFile loadAnnotationFile(const QString& jsonPath, const ImageInfo& image,
                                  const QStringList& imageLabelOptions = {});

enum class SaveStatus {
    Saved,
    NotModified,  // nothing was edited, nothing was written (format spec section 1)
    ReadOnly,     // the file must not be written; see AnnotationFile::readOnlyReason()
    Failed,
};

struct SaveResult {
    SaveStatus status = SaveStatus::Failed;
    QString error;
};

// Writes an edited file atomically: a temporary file is written, flushed and then renamed
// over the target (QSaveFile). Unedited and read-only files are left untouched.
SaveResult saveAnnotationFile(const QString& jsonPath, AnnotationFile& file);

} // namespace liteanno::formats
