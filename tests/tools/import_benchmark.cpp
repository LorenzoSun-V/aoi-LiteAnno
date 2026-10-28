// Measures a YOLO detection import into a plain folder (architecture 11.2, 14): all images in
// one folder, as in the field, each with a label file of three boxes. Prints one Markdown
// table row per measurement: the import as a whole, and the parts of it that touch every
// image, timed on their own.
//
//   liteanno_import_benchmark <folder for the test data> <image count>
//
// The test data goes into a new subfolder that is removed at the end.

#include "application/ExportService.h"
#include "application/ImportService.h"
#include "application/LocalDataPaths.h"
#include "application/ProjectSession.h"
#include "dataset/DirectoryScanner.h"
#include "formats/AnnotationStore.h"
#include "imaging/ImageLoader.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

using namespace liteanno;

namespace {

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

QByteArray smallPng()
{
    QImage image(768, 768, QImage::Format_Grayscale8);
    image.fill(128);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QString imageName(int i)
{
    return QStringLiteral("SES-%1.png").arg(i, 6, 10, QLatin1Char('0'));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: liteanno_import_benchmark <folder for the test data> <image count>\n");
        return 2;
    }
    QDir().mkpath(args[1]);
    const QTemporaryDir dir(QDir(args[1]).absoluteFilePath(QStringLiteral("import-benchmark-XXXXXX")));
    const int count = args[2].toInt();
    if (!dir.isValid() || count <= 0) {
        std::fprintf(stderr, "cannot create the test folder, or the count is not a number\n");
        return 2;
    }
    const QString work = dir.path();
    const QString root = work + QLatin1String("/data");
    const QString labels = work + QLatin1String("/labels");
    QDir().mkpath(root);
    QDir().mkpath(labels);
    QTextStream out(stdout);

    QElapsedTimer timer;
    timer.start();
    const QByteArray png = smallPng();
    for (int i = 0; i < count; ++i) {
        writeFile(root + QLatin1Char('/') + imageName(i), png);
        writeFile(labels + QStringLiteral("/SES-%1.txt").arg(i, 6, 10, QLatin1Char('0')),
                  "1\t0.78\t0.14\t0.10\t0.06\n0\t0.57\t0.31\t0.02\t0.02\n3\t0.51\t0.04\t0.06\t0.05\n");
    }
    writeFile(labels + QLatin1String("/classes.txt"), "a\nb\nc\nd\n");
    const qint64 generateMs = timer.elapsed();

    const application::LocalDataPaths local(work + QLatin1String("/local"));
    application::ProjectSession::OpenResult opened = application::ProjectSession::open(root, local);
    if (!opened.session)
        qFatal("cannot open the folder: %s", qPrintable(opened.message));

    out << "| Images | Step | Time (ms) | Calls | Notes |\n|---|---|---|---|---|\n";
    out << "| " << count << " | generate test folder | " << generateMs << " | | not part of the program |\n";

    timer.restart();
    const dataset::ScanResult scan = dataset::scanDirectory(root);
    out << "| " << count << " | list the folder once | " << timer.elapsed() << " | 1 | |\n";

    timer.restart();
    for (const dataset::ScannedImage& image : scan.images)
        imaging::readImageInfo(opened.session->imageFilePath(image.path));
    out << "| " << count << " | read every image size once | " << timer.elapsed() << " | " << scan.images.size() << " | |\n";

    timer.restart();
    for (const dataset::ScannedImage& image : scan.images)
        dataset::hasNameCollision(opened.session->imageFilePath(image.path));
    out << "| " << count << " | name-collision check of every image once | " << timer.elapsed() << " | "
        << scan.images.size() << " | lists the image's folder each time |\n";

    qint64 infoMs = 0;
    int infoCalls = 0;
    const application::ImageInfoReader timedInfo = [&](const QString& path) {
        QElapsedTimer t;
        t.start();
        std::optional<ImageInfo> info = imaging::readImageInfo(path);
        infoMs += t.elapsed();
        ++infoCalls;
        return info;
    };
    application::ImportRequest request;
    request.format = formats::ImportFormat::YoloDetection;
    request.labelDir = labels;
    request.classesFile = labels + QLatin1String("/classes.txt");
    timer.restart();
    const application::ImportReport report = application::ImportService(*opened.session, timedInfo).run(request);
    const qint64 importMs = timer.elapsed();
    if (!report.written || report.count(application::ImportResult::Imported) != count)
        qFatal("the import did not write every image: %s", qPrintable(report.error));
    out << "| " << count << " | import (whole) | " << importMs << " | | |\n";
    out << "| " << count << " | import: reading image sizes | " << infoMs << " | " << infoCalls << " | part of the above |\n";

    // The atomic write on its own: a new file per image in the same folder, as the import
    // writes, counting the attempts that had to be retried (a scanner holding the file).
    int retries = 0;
    formats::SaveOptions options;
    options.beforeRetry = [&retries](int) { ++retries; };
    const QByteArray bytes = R"({"version": "0.1.0", "flags": {}, "shapes": [], "imagePath": "a.png", "imageData": null})";
    timer.restart();
    for (int i = 0; i < count; ++i) {
        const formats::SaveResult saved =
            formats::writeFileAtomically(root + QStringLiteral("/write-%1.json").arg(i), bytes, options);
        if (saved.status != formats::SaveStatus::Saved)
            qFatal("cannot write: %s", qPrintable(saved.error));
    }
    out << "| " << count << " | write a new file per image (atomic, synced) | " << timer.elapsed() << " | " << count
        << " | " << retries << " retried attempts |\n";
    return 0;
}
