#pragma once

// Writing the per-image annotations into training formats (architecture 11, 11.1), as
// X-AnyLabeling 4.2.0's LabelConverter writes them (custom_to_yolo, custom_to_voc,
// custom_to_coco), so the files are the same for the same input.

#include "formats/AnnotationFile.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <array>
#include <memory>
#include <optional>
#include <utility>

class QIODevice;

namespace liteanno::formats {

enum class ExportFormat {
    YoloDetection,     // rectangles as YOLO boxes
    YoloSegmentation,  // polygons as YOLO segments
    VocRectangle,      // rectangles as Pascal VOC boxes
    VocPolygon,        // rectangles and polygons, polygons with their points
    CocoDetection,     // rectangles
    CocoSegmentation,  // polygons, instances joined by group_id
};

// As the command line names them: "yolo-det", "yolo-seg", "voc", "voc-polygon", "coco-det",
// "coco-seg".
QString exportFormatName(ExportFormat format);
std::optional<ExportFormat> exportFormatFromName(const QString& name);
QList<ExportFormat> exportFormats();
// Class numbers come from "classes[].id" of the project file (architecture 11).
bool exportNeedsClassIds(ExportFormat format);

// A number as Python's json module reads it. An int stays an int through min, max and
// subtraction, and X-AnyLabeling then writes "0" where a float would be "0.0".
struct PyNumber {
    double value = 0;
    bool isInteger = false;
};

struct ExportShape {
    bool recognized = true;  // false: an object format spec 13.5 does not recognize
    QString label;
    QString type;            // "shape_type"; a missing one reads as "polygon"
    QList<std::array<PyNumber, 2>> points;
    int difficult = 0;       // int(shape.get("difficult", False))
    std::optional<qint64> groupId;
};

struct ExportImage {
    QString path;  // relative to the data root, with '/'
    int width = 0;  // "imageWidth" and "imageHeight", checked against the image when confirmed
    int height = 0;
    QList<ExportShape> shapes;
};

// The objects of an annotation file the way the upstream converters read them. `file` was
// read from the same bytes; its shapes say which objects are recognized.
ExportImage exportImageOf(const QString& path, const QByteArray& bytes, const AnnotationFile& file);

struct ExportIssue {
    int shape = -1;  // index in "shapes"
    QString reason;
};

// Objects the format cannot hold. An image with any is not exported: no object is dropped
// and no such image becomes a negative sample (architecture 11).
QList<ExportIssue> unconvertibleShapes(const ExportImage& image, ExportFormat format,
                                       const QHash<QString, int>& classIds);
// Objects with a point outside the image; the export moves it to the border, as upstream does.
int clampedShapes(const ExportImage& image, ExportFormat format);

// The YOLO label file of one image; empty for a negative sample.
QByteArray yoloLabels(const ExportImage& image, ExportFormat format, const QHash<QString, int>& classIds);
// The VOC XML of one image, as xml.dom.minidom's toprettyxml(indent="  ") writes it.
// `folder` is the folder of the XML file, written into <folder> as upstream does.
QByteArray vocAnnotation(const ExportImage& image, ExportFormat format, const QString& folder);

struct CocoInfo {
    int year = 0;
    QString version;
    QString dateCreated;  // "2026-10-22"
};

// Writes one COCO file while the images are added, so the whole project never has to be in
// memory (architecture 11). Annotations wait in `annotations` until finish().
class CocoWriter
{
public:
    // `categories`: class id and name, in id order.
    CocoWriter(ExportFormat format, QList<std::pair<int, QString>> categories, CocoInfo info);
    ~CocoWriter();

    // Both devices are open for writing; `annotations` also for reading.
    bool begin(QIODevice* out, QIODevice* annotations);
    // `fileName` is written as "file_name".
    bool add(const ExportImage& image, const QString& fileName, const QHash<QString, int>& classIds);
    bool finish();
    // The file name upstream gives the result: coco_detection.json, coco_instance_segmentation.json.
    static QString fileName(ExportFormat format);

private:
    struct Data;
    std::unique_ptr<Data> d;
};

// liteanno-export.json, written into the output folder by every export (format spec 10).
struct ExportManifest {
    QString projectId;
    QString exportedAt;  // ISO 8601 with time zone
    QString appVersion;
    bool wholeProject = false;
    QList<std::pair<int, QString>> batches;  // number and batchId
    bool scanComplete = true;
    ExportFormat format = ExportFormat::YoloDetection;
    bool skipUnconvertible = false;
    bool copyImages = false;
    QList<std::pair<int, QString>> categories;  // class id and name
    QList<std::pair<QString, int>> counts;      // result name and number of images
    struct Item {
        QString image;
        QString result;  // "exported", "negative", "skipped", "not-done", "unconvertible"
        QString status;  // annotationStatusName(), empty when not read
        QString reason;
        int clampedObjects = 0;
    };
    QList<Item> items;
};

QByteArray exportManifestToBytes(const ExportManifest& manifest);

} // namespace liteanno::formats
