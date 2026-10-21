#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace liteanno::formats {

// A local copy of an edit, written before the annotation file in the data directory is
// replaced (format spec 8.1).
struct RecoveryRecord {
    QString formatVersion;
    QString projectId;
    QString bindingId;
    QString rootPathAtWrite;  // the data root when the record was written, with '/' separators
    QString imagePath;        // relative to the data root
    qint64 documentVersion = 0;
    QString baseDigest;       // digest of the file before editing; empty for JSON null (no file)
    QString createdAt;        // ISO 8601 with time zone
    QByteArray annotation;    // the complete per-image JSON, in the bytes it is written as
};

// The record as JSON, "annotation" embedded as an object; nullopt when `annotation` is not
// a JSON object.
std::optional<QByteArray> recoveryRecordToBytes(const RecoveryRecord& record);

struct RecoveryRecordResult {
    std::optional<RecoveryRecord> record;
    QString error;  // why the record cannot be used
};

// Reads a record written by recoveryRecordToBytes. Any missing or invalid field rejects it.
RecoveryRecordResult parseRecoveryRecord(const QByteArray& bytes);

} // namespace liteanno::formats
