#include "formats/WorksetFiles.h"

#include <QCryptographicHash>

namespace liteanno::formats {

QString computeItemsDigest(const QStringList& items)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (qsizetype i = 0; i < items.size(); ++i) {
        if (i > 0)
            hash.addData(QByteArray("\n"));
        hash.addData(items[i].toUtf8());
    }
    return QStringLiteral("sha256:") + QString::fromLatin1(hash.result().toHex());
}

} // namespace liteanno::formats
