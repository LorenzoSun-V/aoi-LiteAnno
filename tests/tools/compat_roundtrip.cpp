// Reads an annotation file the way the editing window does, then edits and writes it.
// Used by tests/python/upstream_compat.py to check the result with X-AnyLabeling's own reader
// (format spec 11).
//
// usage: liteanno_compat_roundtrip <image> <annotation.json> <edited-output.json>
//
// Prints one JSON object: whether the unedited file would be written, the read-only reason,
// and what was edited. Nothing is written when the file is read-only.

#include "formats/AnnotationFile.h"
#include "formats/AnnotationStore.h"
#include "imaging/ImageLoader.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace liteanno;

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    const QStringList args = QGuiApplication::arguments();
    if (args.size() != 4) {
        std::fprintf(stderr, "usage: %s <image> <annotation.json> <edited-output.json>\n", argv[0]);
        return 2;
    }

    const imaging::LoadedImage image = imaging::loadImage(args[1]);
    if (image.image.isNull()) {
        std::fprintf(stderr, "%s: %s\n", qPrintable(args[1]), qPrintable(image.error));
        return 1;
    }
    formats::LoadedAnnotation loaded = formats::loadAnnotationFile(args[2], image.info);
    formats::AnnotationFile& file = loaded.file;

    QJsonObject report;
    report.insert(QStringLiteral("readOnly"), QString::fromLatin1(formats::readOnlyReasonName(file.readOnlyReason())));
    report.insert(QStringLiteral("uneditedNeedsWrite"), file.needsWrite());
    report.insert(QStringLiteral("shapes"), int(file.document().shapes().size()));
    report.insert(QStringLiteral("relabeled"), -1);

    if (!file.isReadOnly()) {
        AnnotationDocument& document = file.document();
        // The first shape this program can edit gets another label.
        for (qsizetype i = 0; i < document.shapes().size(); ++i) {
            if (document.setShapeLabel(i, document.shapes()[i].label + QStringLiteral("_relabeled"))) {
                report.insert(QStringLiteral("relabeled"), int(i));
                break;
            }
        }
        Shape added;
        added.type = ShapeType::Rectangle;
        added.label = QStringLiteral("liteanno_added");
        added.points = {QPointF(1.5, 2.0), QPointF(qMin(image.info.width, 40), qMin(image.info.height, 30))};
        if (document.addShape(added) < 0) {
            std::fprintf(stderr, "cannot add a rectangle\n");
            return 1;
        }
        document.setImageLabel(QStringLiteral("liteanno_flag"), true);
        document.confirm();

        QFile out(args[3]);
        const QByteArray bytes = file.toBytes();
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size()) {
            std::fprintf(stderr, "%s: %s\n", qPrintable(args[3]), qPrintable(out.errorString()));
            return 1;
        }
        report.insert(QStringLiteral("written"), true);
    }
    std::fputs(QJsonDocument(report).toJson(QJsonDocument::Compact).constData(), stdout);
    return 0;
}
