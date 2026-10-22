#pragma once

// Reading training-format labels back into shapes (architecture 11.2), with the numbers
// X-AnyLabeling 4.2.0's LabelConverter computes in yolo_to_custom, yolo_obb_to_custom,
// voc_to_custom and dota_to_custom. Unlike upstream, a file with a line or object that cannot
// be read is refused as a whole instead of losing that object.

#include "core/Shape.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace liteanno::formats {

enum class ImportFormat {
    YoloDetection,     // YOLO boxes as rectangles
    YoloSegmentation,  // YOLO segments as polygons
    YoloObb,           // YOLO OBB corners as rotated boxes
    VocRectangle,      // VOC boxes as rectangles
    VocPolygon,        // VOC polygons as polygons, boxes as rectangles
    Dota,              // DOTA lines as rotated boxes
};

// The names of the matching export formats: "yolo-det", "yolo-seg", "yolo-obb", "voc",
// "voc-polygon", "dota".
QString importFormatName(ImportFormat format);
std::optional<ImportFormat> importFormatFromName(const QString& name);
QList<ImportFormat> importFormats();
// The label file of an image: its name without the extension plus this, ".txt" or ".xml".
QString importLabelSuffix(ImportFormat format);
// YOLO files hold class numbers; their names come from classes.txt or the project.
bool importNeedsClassNames(ImportFormat format);

// The shapes of one label file, or why the file cannot be imported.
struct ImportedLabels {
    QList<Shape> shapes;
    QString error;  // empty when the file was read
};

// `classes`: the name of each class number; an empty name is a number without a class.
// `width` and `height` are the image's, as the coordinates are relative to them.
ImportedLabels parseYoloLabels(const QByteArray& bytes, ImportFormat format, const QStringList& classes, int width,
                               int height);
// "x0 y0 x1 y1 x2 y2 x3 y3 label [difficult]" per line; the "imagesource:" and "gsd:" header
// lines of DOTA files are passed over.
ImportedLabels parseDotaLabels(const QByteArray& bytes);
// The size in the XML must be the image's: coordinates are not scaled.
ImportedLabels parseVocAnnotation(const QByteArray& bytes, ImportFormat format, int width, int height);

// The angle upstream gives an imported rotated box: the direction of the side from corner 0
// to corner 1, in radians in [0, 2π) (LabelConverter.calculate_rotation_theta).
double rotationTheta(const PointList& points);

} // namespace liteanno::formats
