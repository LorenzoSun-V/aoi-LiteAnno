#pragma once

#include "formats/AnnotationFile.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>

namespace liteanno::formats {

// What a file on disk looked like when it was read (architecture section 8.3). Saving
// compares it with the file's current state to detect changes made by other programs.
struct FileStamp {
    bool exists = false;
    qint64 size = 0;
    QDateTime modified;
    QString digest;  // contentDigest() of the bytes read; empty when the file was missing or unreadable

    // Two stamps describe the same content. The modification time is not compared: a copy
    // that restores the same bytes is not a change, and the digest also catches a same-size
    // rewrite within the 2-second time granularity of FAT32.
    bool sameContent(const FileStamp& other) const { return exists == other.exists && digest == other.digest; }
};

// "sha256:" followed by 64 lowercase hex digits, as in recovery records (format spec 8.1).
QString contentDigest(const QByteArray& bytes);

// Reads the current state of a file. An unreadable file gives exists=true and an empty digest.
FileStamp readFileStamp(const QString& path);

struct LoadedAnnotation {
    AnnotationFile file;
    FileStamp stamp;  // the file as read; updated by every successful save
};

// Reads the annotation file next to an image. A missing file gives a new document; a file
// that exists but cannot be read gives a read-only one, so it is never overwritten.
// Format spec 3: the annotation of an image matches ".json" in any case. `jsonPath` is the
// path with a lower-case ".json"; when no such file exists, an existing file whose extension
// differs only in case is returned instead (on Linux, where names are case-sensitive).
QString existingAnnotationPath(const QString& jsonPath);

LoadedAnnotation loadAnnotationFile(const QString& jsonPath, const ImageInfo& image,
                                    const QStringList& imageLabelOptions = {});

enum class SaveStatus {
    Saved,
    NotModified,         // nothing was edited, nothing was written (format spec section 1)
    ReadOnly,            // the file must not be written; see AnnotationFile::readOnlyReason()
    ExternallyModified,  // another program changed, created or deleted the file since it was read
    Failed,
};

const char* saveStatusName(SaveStatus status);

struct SaveResult {
    SaveStatus status = SaveStatus::Failed;
    QString error;
    int attempts = 0;  // write attempts made; more than 1 when the replace was retried
};

struct SaveOptions {
    // Write even if the file changed on disk: the user chose "use my version" (format spec 5.2).
    bool overwriteExternalChanges = false;
    // Replacing the file can fail for a moment on Windows while a virus scanner or the search
    // indexer has it open (architecture section 8.1).
    int maxAttempts = 5;
    int retryDelayMs = 200;
    // Called before each retry with the number of the coming attempt (2, 3, ...). For tests.
    std::function<void(int)> beforeRetry;
    // Run right before every write attempt, after the check for external changes and also
    // when overwriting: Saved lets the attempt go ahead, any other status stops and is
    // returned. For conditions outside the file, such as a name collision (format spec 2).
    std::function<SaveResult()> mayWrite;
};

// Run before each write attempt. Saved lets the attempt go ahead, Failed waits for the next
// attempt (the last one is returned when none is left), any other status stops and is returned.
using WriteCheck = std::function<SaveResult()>;

// Writes bytes atomically: a temporary file is written, flushed and renamed over the target
// (QSaveFile), retried as SaveOptions says. The target is unchanged when this fails.
SaveResult writeFileAtomically(const QString& path, const QByteArray& bytes, const SaveOptions& options = {},
                               const WriteCheck& check = {});

// Saved when the file still has the content `expected` describes, ExternallyModified when it
// changed, appeared or disappeared, Failed when it cannot be read now.
SaveResult checkUnchanged(const QString& path, const FileStamp& expected);

// Writes an edited file atomically. Unedited and read-only files are left untouched, and a
// file another program changed since `annotation.stamp` is not overwritten unless the
// options say so. On success the stamp describes the bytes written.
SaveResult saveAnnotationFile(const QString& jsonPath, LoadedAnnotation& annotation, const SaveOptions& options = {});

} // namespace liteanno::formats
