#include "imaging/IconFile.h"

#include <QBuffer>
#include <QPainter>
#include <QtEndian>

#include <algorithm>

namespace liteanno::imaging {

QImage squareLogo(const QImage& logo)
{
    if (logo.isNull() || logo.width() == logo.height())
        return logo.convertToFormat(QImage::Format_ARGB32);
    const int side = std::max(logo.width(), logo.height());
    QImage square(side, side, QImage::Format_ARGB32);
    square.fill(Qt::transparent);
    QPainter painter(&square);
    painter.drawImage((side - logo.width()) / 2, (side - logo.height()) / 2, logo);
    return square;
}

QList<int> windowsIconSizes()
{
    return {16, 24, 32, 48, 64, 128, 256};
}

QByteArray windowsIcon(const QImage& logo)
{
    const QImage square = squareLogo(logo);
    if (square.isNull())
        return {};
    QList<QByteArray> entries;
    for (int size : windowsIconSizes()) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        square.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG");
        entries.append(png);
    }
    // ICONDIR, then one ICONDIRENTRY per image, then the images.
    QByteArray file;
    const auto put16 = [&](quint16 value) {
        const quint16 le = qToLittleEndian(value);
        file.append(reinterpret_cast<const char*>(&le), 2);
    };
    const auto put32 = [&](quint32 value) {
        const quint32 le = qToLittleEndian(value);
        file.append(reinterpret_cast<const char*>(&le), 4);
    };
    put16(0);
    put16(1);  // icon
    put16(quint16(entries.size()));
    quint32 offset = 6 + 16 * quint32(entries.size());
    const QList<int> sizes = windowsIconSizes();
    for (int i = 0; i < entries.size(); ++i) {
        const char side = char(sizes[i] >= 256 ? 0 : sizes[i]);  // 0 means 256
        file.append(side);
        file.append(side);
        file.append(char(0));  // no palette
        file.append(char(0));
        put16(1);   // planes
        put16(32);  // bits per pixel
        put32(quint32(entries[i].size()));
        put32(offset);
        offset += quint32(entries[i].size());
    }
    for (const QByteArray& entry : entries)
        file.append(entry);
    return file;
}

} // namespace liteanno::imaging
