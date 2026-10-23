#pragma once

// Reading training-format labels back into shapes (architecture 11.2), with the numbers
// X-AnyLabeling 4.2.0's LabelConverter computes in yolo_to_custom, yolo_obb_to_custom,
// voc_to_custom, dota_to_custom, coco_to_custom and mask_to_custom. Unlike upstream, a file with a line or object that cannot
// be read is refused as a whole instead of losing that object.

#include "core/RgbImage.h"
#include "core/Shape.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>
#include <utility>

namespace liteanno::formats {

enum class ImportFormat {
    YoloDetection,     // YOLO boxes as rectangles
    YoloSegmentation,  // YOLO segments as polygons
    YoloObb,           // YOLO OBB corners as rotated boxes
    VocRectangle,      // VOC boxes as rectangles
    VocPolygon,        // VOC polygons as polygons, boxes as rectangles
    Dota,              // DOTA lines as rotated boxes
    CocoDetection,     // COCO boxes as rectangles (one JSON file for all images)
    CocoSegmentation,  // COCO polygons as polygons
    Mask,              // mask PNGs as polygons, with the class of each value or colour
};

// The names of the matching export formats: "yolo-det", "yolo-seg", "yolo-obb", "voc",
// "voc-polygon", "dota", "coco-det", "coco-seg", "mask".
QString importFormatName(ImportFormat format);
std::optional<ImportFormat> importFormatFromName(const QString& name);
QList<ImportFormat> importFormats();
// The label file of an image: its name without the extension plus this, ".txt", ".xml" or
// ".png". COCO has one file for all images: empty.
QString importLabelSuffix(ImportFormat format);
bool importReadsOneFile(ImportFormat format);
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

// A COCO file (coco_to_custom): the images it lists, each with its shapes or why they cannot
// be imported. `error` is set when the file as a whole cannot be read: no images,
// annotations or categories array, an annotation of an image the file does not list, an
// image listed twice.
struct CocoImageLabels {
    QString fileName;  // "file_name" as written
    int width = 0;
    int height = 0;
    ImportedLabels labels;
};
struct CocoLabels {
    QList<CocoImageLabels> images;
    QString error;
};
CocoLabels parseCocoLabels(const QByteArray& bytes, ImportFormat format);

// The mask mapping file (upstream's mask import asks for it; our mask export writes
// mask_colors.json): {"type": "grayscale" | "rgb", "colors": {class: value or [r, g, b]}}.
struct MaskMapping {
    bool grayscale = true;
    QList<std::pair<QString, int>> values;  // grayscale: class and value, in file order
    QList<std::pair<QString, std::array<int, 3>>> colors;  // rgb: class and colour, in file order
    QString error;
};
MaskMapping parseMaskMapping(const QByteArray& bytes);
// The polygons of a mask as upstream's get_contours_and_labels makes them. Grayscale masks
// must have the same value in all three channels; pixels of values or colours the mapping
// does not name are passed over, as upstream does.
ImportedLabels maskLabels(const RgbImage& mask, const MaskMapping& mapping);

// The angle upstream gives an imported rotated box: the direction of the side from corner 0
// to corner 1, in radians in [0, 2π) (LabelConverter.calculate_rotation_theta).
double rotationTheta(const PointList& points);

} // namespace liteanno::formats
