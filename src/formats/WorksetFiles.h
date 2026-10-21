#pragma once

#include <QString>
#include <QStringList>

namespace liteanno::formats {

// The "itemsDigest" of a batch list, also copied to "listDigest" in its progress file
// (format spec 7.1, 7.2, 13.2): SHA-256 over the paths as written (no Unicode normalization),
// UTF-8 encoded and joined with single LF characters, without a trailing LF, written as
// "sha256:" followed by 64 lowercase hex digits.
QString computeItemsDigest(const QStringList& items);

} // namespace liteanno::formats
