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

} // namespace liteanno::imaging
