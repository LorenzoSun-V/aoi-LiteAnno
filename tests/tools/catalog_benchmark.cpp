// Measures the catalog on a generated folder (architecture 6, 14): the first open with no
// catalog, opening again, a rescan after another program changed 1% of the annotation
// files, and the window opening the project. Prints one Markdown table row per measurement.
//
//   liteanno_catalog_benchmark <folder for the test data> <image count>
//
// The test data goes into a new subfolder that is removed at the end.
//
// Images are small real PNGs, 1000 per subfolder; every second image has an annotation file
// with five rectangles. Sizes are read from the image headers as in the program.

#include "application/LocalDataPaths.h"
#include "application/ProjectCatalog.h"
#include "application/ProjectSession.h"
#include "imaging/ImageLoader.h"
#include "ui/MainWindow.h"

#include <QBuffer>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <cstdio>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <psapi.h>
#endif

using namespace liteanno;

namespace {

constexpr int kPerFolder = 1000;

// Peak memory of this process so far, in MiB.
double peakMemoryMiB()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return double(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
    return -1;
#else
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray& line : status.readAll().split('\n')) {
        if (line.startsWith("VmHWM:"))
            return line.mid(6).trimmed().split(' ').first().toDouble() / 1024.0;
    }
    return -1;
#endif
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

QByteArray annotation(const QString& imageName, int variant)
{
    QByteArray shapes;
    for (int i = 0; i < 5; ++i) {
        const QByteArray x = QByteArray::number(10 + i * 100 + variant), y = QByteArray::number(20 + i * 80);
        if (i > 0)
            shapes += ",";
        shapes += R"({"label": "scratch", "points": [[)" + x + ".0, " + y + ".0], [" + QByteArray::number(x.toInt() + 60)
                  + ".0, " + QByteArray::number(y.toInt() + 40)
                  + R"(.0]], "group_id": null, "description": "", "difficult": false, "shape_type": "rectangle", "flags": {}})";
    }
    return R"({"version": "2.4.0", "flags": {}, "shapes": [)" + shapes + R"(], "imagePath": ")" + imageName.toUtf8()
           + R"(", "imageData": null, "imageHeight": 768, "imageWidth": 768, "checked": true})";
}

// A 768 x 768 PNG header with almost no data: the size is what the catalog reads.
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

void generate(const QString& root, int count)
{
    QDir().mkpath(root + QLatin1String("/liteanno"));
    writeFile(root + QLatin1String("/liteanno/project.json"),
              R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", )"
              R"("shapeTypes": ["rectangle"], "classes": [{"id": 0, "name": "scratch"}]})");
    const QByteArray png = smallPng();
    for (int i = 0; i < count; ++i) {
        const QString folder = root + QStringLiteral("/line%1").arg(i / kPerFolder, 4, 10, QLatin1Char('0'));
        if (i % kPerFolder == 0)
            QDir().mkpath(folder);
        const QString name = QStringLiteral("%1").arg(i, 7, 10, QLatin1Char('0'));
        writeFile(folder + QLatin1Char('/') + name + QLatin1String(".png"), png);
        if (i % 2 == 0)
            writeFile(folder + QLatin1Char('/') + name + QLatin1String(".json"), annotation(name + QLatin1String(".png"), 0));
    }
}

struct Times {
    qint64 scanMs = -1;
    qint64 checkMs = -1;
    int checked = 0;
};

Times run(application::ProjectCatalog& catalog)
{
    Times times;
    QElapsedTimer timer;
    QObject::connect(&catalog, &application::ProjectCatalog::scanFinished, &catalog,
                     [&](bool) { times.scanMs = timer.elapsed(); }, Qt::DirectConnection);
    QObject::connect(&catalog, &application::ProjectCatalog::checkProgress, &catalog,
                     [&](int done, int) { times.checked = done; }, Qt::DirectConnection);
    timer.start();
    catalog.start();
    if (!catalog.waitUntilIdle(-1))
        qFatal("catalog did not finish");
    times.checkMs = timer.elapsed() - times.scanMs;
    QObject::disconnect(&catalog, nullptr, &catalog, nullptr);
    return times;
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: liteanno_catalog_benchmark <folder for the test data> <image count>\n");
        return 2;
    }
    QDir().mkpath(args[1]);
    const QTemporaryDir dir(QDir(args[1]).absoluteFilePath(QStringLiteral("catalog-benchmark-XXXXXX")));
    const int count = args[2].toInt();
    if (!dir.isValid() || count <= 0) {
        std::fprintf(stderr, "cannot create the test folder, or the count is not a number\n");
        return 2;
    }
    const QString work = dir.path();
    const QString root = work + QLatin1String("/data");
    const application::LocalDataPaths local(work + QLatin1String("/local"));
    QTextStream out(stdout);

    QElapsedTimer timer;
    timer.start();
    generate(root, count);
    const qint64 generateMs = timer.elapsed();

    application::ProjectSession::OpenResult opened = application::ProjectSession::open(root, local);
    if (!opened.session)
        qFatal("cannot open the project: %s", qPrintable(opened.message));

    out << "| Images | Step | Time (ms) | Images checked | Notes |\n|---|---|---|---|---|\n";
    out << "| " << count << " | generate test folder | " << generateMs << " | | not part of the program |\n";

    // 1. First open: no catalog yet.
    QString database;
    {
        application::ProjectCatalog catalog(*opened.session, imaging::readImageInfo);
        if (!catalog.isOpen())
            qFatal("cannot open the catalog: %s", qPrintable(catalog.error()));
        database = catalog.databasePath();
        const Times first = run(catalog);
        out << "| " << count << " | first open: list folder (stage 1) | " << first.scanMs << " | | |\n";
        out << "| " << count << " | first open: read statuses (stage 2) | " << first.checkMs << " | " << first.checked
            << " | |\n";
    }

    // 2. Open again: what the window shows at once, then the scan that finds nothing new.
    {
        timer.restart();
        application::ProjectCatalog catalog(*opened.session, imaging::readImageInfo);
        const QList<dataset::CatalogEntry> images = catalog.reader().images();
        const qint64 listMs = timer.elapsed();
        if (images.size() != count)
            qFatal("catalog lists %lld images, expected %d", qlonglong(images.size()), count);
        out << "| " << count << " | open again: list from catalog | " << listMs << " | | before any scan |\n";
        const Times again = run(catalog);
        out << "| " << count << " | open again: list folder (stage 1) | " << again.scanMs << " | | |\n";
        out << "| " << count << " | open again: read statuses (stage 2) | " << again.checkMs << " | " << again.checked
            << " | |\n";

        // 3. Another program changes 1% of the annotation files.
        const int changed = qMax(1, count / 100);
        QThread::msleep(20);  // a later modification time also on coarse clocks
        for (int i = 0; i < changed; ++i) {
            const int image = (i * 2) % count;
            const QString name = QStringLiteral("%1").arg(image, 7, 10, QLatin1Char('0'));
            const QString folder = root + QStringLiteral("/line%1").arg(image / kPerFolder, 4, 10, QLatin1Char('0'));
            writeFile(folder + QLatin1Char('/') + name + QLatin1String(".json"), annotation(name + QLatin1String(".png"), 1));
        }
        const Times rescan = run(catalog);
        out << "| " << count << " | rescan after " << changed << " files changed: stage 1 | " << rescan.scanMs
            << " | | |\n";
        out << "| " << count << " | rescan after " << changed << " files changed: stage 2 | " << rescan.checkMs << " | "
            << rescan.checked << " | |\n";
    }

    // 4. The window opens the project again: the catalog is read, the list built, the first
    // image shown; the scan then goes on in the background.
    opened.session.reset();  // the window takes the project lock
    {
        ui::MainWindow::Options options;
        options.localData = local;
        ui::MainWindow window(options);
        window.resize(1200, 800);
        window.show();
        QCoreApplication::processEvents();
        timer.restart();
        if (!window.openProject(root))
            qFatal("the window cannot open the project");
        QCoreApplication::processEvents();  // the list paints
        const qint64 openMs = timer.elapsed();
        if (window.imageCount() != count)
            qFatal("the window lists %d images, expected %d", window.imageCount(), count);
        out << "| " << count << " | window: open again until the list is shown | " << openMs << " | | "
            << "catalog read, list built, first image shown |\n";
        window.closeProject(true);
    }

    out << "| " << count << " | catalog file | | | " << QString::number(QFileInfo(database).size() / 1048576.0, 'f', 1)
        << " MiB |\n";
    out << "| " << count << " | peak memory of the process | | | " << QString::number(peakMemoryMiB(), 'f', 0)
        << " MiB |\n";
    return 0;
}
