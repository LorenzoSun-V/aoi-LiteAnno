#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace liteanno::formats {

// The "itemsDigest" of a batch list, also copied to "listDigest" in its progress file
// (format spec 7.1, 7.2, 13.2): SHA-256 over the paths as written (no Unicode normalization),
// UTF-8 encoded and joined with single LF characters, without a trailing LF, written as
// "sha256:" followed by 64 lowercase hex digits.
QString computeItemsDigest(const QStringList& items);

// A batch list, liteanno/worksets/bNNNNNN.list.json (format spec 7.1). Written once.
struct WorksetList {
    QString formatVersion;
    QString projectId;
    QString batchId;     // 128 random bits as 32 lowercase hex digits; never reused
    int number = 0;      // shown as "第 N 批"
    QString createdAt;   // ISO 8601 with time zone
    QStringList items;   // image paths relative to the data root, in order
    QString itemsDigest;
};

QByteArray worksetListToBytes(const WorksetList& list);

struct WorksetListResult {
    std::optional<WorksetList> list;
    QString error;  // why the list is damaged; it is then not repaired
};

// Reads a list. A missing or invalid field, an invalid image path or a digest that does not
// match the items rejects it; the caller compares projectId.
WorksetListResult parseWorksetList(const QByteArray& bytes);

struct WorksetEvent {
    QString type;  // "started", "completed"
    QString at;    // ISO 8601 with time zone
};

// Progress of a batch, liteanno/worksets/bNNNNNN.progress.json (format spec 7.2): where the
// user was and what happened. Losing it only loses the last position.
struct WorksetProgress {
    QString formatVersion;
    QString batchId;
    QString listDigest;  // the list's itemsDigest
    QString lastItem;    // empty when none
    QList<WorksetEvent> events;
};

QByteArray worksetProgressToBytes(const WorksetProgress& progress);

// Nothing when the file cannot be used; it is then ignored.
std::optional<WorksetProgress> parseWorksetProgress(const QByteArray& bytes);

} // namespace liteanno::formats
