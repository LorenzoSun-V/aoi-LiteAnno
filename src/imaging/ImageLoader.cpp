#include "imaging/ImageLoader.h"

#include <QFileInfo>
#include <QImageReader>

namespace liteanno::imaging {

namespace {

// The EXIF orientation number of a Qt transformation (EXIF 2.3, tag 0x0112).
int exifOrientation(QImageIOHandler::Transformations transformation)
{
    switch (int(transformation)) {
    case QImageIOHandler::TransformationMirror:
        return 2;
    case QImageIOHandler::TransformationRotate180:
        return 3;
    case QImageIOHandler::TransformationFlip:
        return 4;
    case QImageIOHandler::TransformationFlipAndRotate90:
        return 5;
    case QImageIOHandler::TransformationRotate90:
        return 6;
    case QImageIOHandler::TransformationMirrorAndRotate90:
        return 7;
    case QImageIOHandler::TransformationRotate270:
        return 8;
    default:
        return 1;
    }
}

} // namespace

LoadedImage loadImage(const QString& path)
{
    LoadedImage result;
    result.info.fileName = QFileInfo(path).fileName();
    result.info.exifOrientation = 1;

    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    // Qt keeps images as stored unless asked (format spec 9.2).
    reader.setAutoTransform(true);
    // Counted first: Qt's TIFF reader answers 1 once the orientation has been asked for.
    result.hasMoreFrames = reader.imageCount() > 1;
    result.info.exifOrientation = exifOrientation(reader.transformation());

    QImage image = reader.read();
    if (image.isNull()) {
        result.error = reader.errorString();
        return result;
    }
    switch (image.format()) {
    case QImage::Format_Grayscale16:
        image = image.convertToFormat(QImage::Format_Grayscale8);
        break;
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBX32FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
        image = image.convertToFormat(QImage::Format_ARGB32);
        break;
    default:
        break;
    }
    result.info.width = image.width();
    result.info.height = image.height();
    result.image = std::move(image);
    return result;
}

std::optional<ImageInfo> readImageInfo(const QString& path)
{
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    ImageInfo info;
    info.fileName = QFileInfo(path).fileName();
    info.exifOrientation = exifOrientation(reader.transformation());
    QSize size = reader.size();
    if (!size.isValid()) {
        // Some readers know the size only after decoding.
        const QImage image = reader.read();
        if (image.isNull())
            return std::nullopt;
        info.width = image.width();
        info.height = image.height();
        return info;
    }
    // The header gives the stored size; turned by a quarter, width and height swap.
    if (info.exifOrientation >= 5)
        size.transpose();
    info.width = size.width();
    info.height = size.height();
    return info;
}

QStringList missingImageFormats()
{
    const QList<QByteArray> supported = QImageReader::supportedImageFormats();
    QStringList missing;
    for (const char* format : {"jpeg", "png", "bmp", "tiff", "webp", "gif", "pbm", "pgm", "ppm"}) {
        if (!supported.contains(QByteArray(format)))
            missing.append(QString::fromLatin1(format));
    }
    return missing;
}

RgbImage readMaskImage(const QString& path)
{
    RgbImage result;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(false);
    if (reader.format() != "png") {
        result.error = reader.canRead() ? QStringLiteral(u"掩膜必须是 PNG 图片（JPEG 等有损格式会改变类别值）")
                                        : QStringLiteral(u"读不出图片：%1").arg(reader.errorString());
        return result;
    }
    QImage image = reader.read();
    if (image.isNull()) {
        result.error = QStringLiteral(u"读不出图片：%1").arg(reader.errorString());
        return result;
    }
    switch (image.format()) {
    case QImage::Format_Grayscale16:
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
        result.error = QStringLiteral(u"掩膜必须是 8 位图片，这张是 16 位的");
        return result;
    default:
        break;
    }
    // Straight alpha is dropped as OpenCV drops it; a premultiplied format would have changed
    // the colours, and Qt's PNG reader does not give one.
    if (image.format() == QImage::Format_ARGB32_Premultiplied || image.format() == QImage::Format_RGBA8888_Premultiplied) {
        result.error = QStringLiteral(u"掩膜的透明度无法按原值读取");
        return result;
    }
    if (image.hasAlphaChannel())
        image = image.convertToFormat(QImage::Format_ARGB32);
    image = image.convertToFormat(QImage::Format_RGB888);
    result.width = image.width();
    result.height = image.height();
    result.rgb.resize(std::size_t(result.width) * std::size_t(result.height) * 3);
    for (int y = 0; y < result.height; ++y) {
        const uchar* line = image.constScanLine(y);
        std::copy(line, line + std::size_t(result.width) * 3, result.rgb.begin() + std::ptrdiff_t(y) * result.width * 3);
    }
    return result;
}

} // namespace liteanno::imaging
