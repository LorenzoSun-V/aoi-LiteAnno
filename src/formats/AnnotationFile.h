#pragma once

#include "core/AnnotationDocument.h"
#include "core/Diagnostic.h"
#include "core/ImageInfo.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

namespace liteanno::formats {

// Why a file can be shown but not changed, in order of precedence.
enum class ReadOnlyReason {
    None,
    Unreadable,                  // the file exists but cannot be read
    JsonError,                   // format spec 4.6, 13.3, 13.5
    UnsupportedFormatVersion,    // section 1, 13.5
    ExifOrientationUnconfirmed,  // 13.4
    ImageSizeMismatch,           // 5.3; correctImageSize() lifts it
};

const char* readOnlyReasonName(ReadOnlyReason reason);

// One per-image annotation file (format spec section 4): the JSON as read, plus the
// AnnotationDocument the user edits.
//
// Saving starts from the JSON as read and writes back only what the edits changed and what
// the save normalizations of 4.5 require. Every other value, including unknown fields,
// unrecognized objects and invalid values nobody edited, keeps its original value (13.3).
class AnnotationFile
{
public:
    // Reads the content of an existing file. Never fails: problems are reported through
    // diagnostics() and readOnlyReason().
    static AnnotationFile fromBytes(const QByteArray& bytes, const ImageInfo& image);
    // A document for an image that has no annotation file yet. The image-level labels the
    // project offers are written as false (13.1).
    static AnnotationFile createNew(const ImageInfo& image, const QStringList& imageLabelOptions = {});
    // A file that exists but could not be read. It is never overwritten.
    static AnnotationFile unreadable(const ImageInfo& image, const QString& error);

    AnnotationFile(AnnotationFile&& other) noexcept;
    AnnotationFile& operator=(AnnotationFile&& other) noexcept;
    ~AnnotationFile();

    const ImageInfo& image() const;
    bool isNew() const;
    ReadOnlyReason readOnlyReason() const;
    bool isReadOnly() const { return readOnlyReason() != ReadOnlyReason::None; }
    const QList<Diagnostic>& diagnostics() const;

    // The editable content. Edits to a read-only file are never saved.
    AnnotationDocument& document();
    const AnnotationDocument& document() const;

    // "按当前图片尺寸更正" (format spec 5.3): the next save writes the actual image size to
    // imageWidth and imageHeight without scaling coordinates. Only possible when
    // readOnlyReason() is ImageSizeMismatch.
    bool correctImageSize();

    // Whether saving would write anything. Unedited and read-only files are never written
    // (format spec section 1).
    bool needsWrite() const;
    // The complete file content to write.
    QByteArray toBytes() const;
    // Call after the bytes from toBytes() were written: they become the new base for edits.
    void markSaved();

private:
    struct Data;
    explicit AnnotationFile(std::unique_ptr<Data> data);

    std::unique_ptr<Data> d;
};

} // namespace liteanno::formats
