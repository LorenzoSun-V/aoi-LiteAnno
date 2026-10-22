#include "formats/PngFile.h"

#include <QtEndian>

#include <array>
#include <cstdint>

namespace liteanno::formats {

namespace {

const std::array<quint32, 256>& crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

void appendUint32(QByteArray& out, quint32 value)
{
    char bytes[4];
    qToBigEndian(value, bytes);
    out.append(bytes, 4);
}

void appendChunk(QByteArray& out, const char* type, const QByteArray& data)
{
    appendUint32(out, quint32(data.size()));
    const qsizetype start = out.size();
    out.append(type, 4);
    out.append(data);
    quint32 crc = 0xffffffffu;
    for (qsizetype i = start; i < out.size(); ++i)
        crc = crcTable()[(crc ^ quint8(out[i])) & 0xff] ^ (crc >> 8);
    appendUint32(out, crc ^ 0xffffffffu);
}

} // namespace

QByteArray grayscalePng(int width, int height, const std::vector<unsigned char>& pixels)
{
    if (width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height))
        return QByteArray();
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    QByteArray header;
    appendUint32(header, quint32(width));
    appendUint32(header, quint32(height));
    header.append(char(8));  // bit depth
    header.append(char(0));  // grayscale
    header.append(char(0));  // deflate
    header.append(char(0));  // adaptive filtering
    header.append(char(0));  // no interlace
    appendChunk(out, "IHDR", header);
    QByteArray raw;
    raw.reserve(qsizetype(height) * (qsizetype(width) + 1));
    for (int y = 0; y < height; ++y) {
        raw.append(char(0));  // filter: none
        raw.append(reinterpret_cast<const char*>(pixels.data()) + size_t(y) * size_t(width), width);
    }
    // qCompress gives a zlib stream after a 4-byte length, which PNG does not have.
    appendChunk(out, "IDAT", qCompress(raw, 6).mid(4));
    appendChunk(out, "IEND", QByteArray());
    return out;
}

} // namespace liteanno::formats
