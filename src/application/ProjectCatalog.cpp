#include "application/ProjectCatalog.h"

#include "application/FileStatus.h"
#include "dataset/DirectoryScanner.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>

#include <chrono>

namespace liteanno::application {

namespace {

constexpr int kWriteChunk = 10000;  // images per catalog transaction
constexpr int kCheckChunk = 500;    // images read per catalog query in stage 2

dataset::FileVersion versionOf(const QFileInfo& info)
{
    if (!info.exists())
        return {};
    return {info.size(), info.lastModified().toMSecsSinceEpoch()};
}

bool waitFor(const std::atomic<bool>& flag, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (flag) {
        if (timeoutMs >= 0 && timer.elapsed() > timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

} // namespace

ProjectCatalog::ProjectCatalog(const ProjectSession& project, ImageInfoReader readImageInfo, QObject* parent)
    : QObject(parent)
    , m_project(project)
    , m_readImageInfo(std::move(readImageInfo))
    , m_databasePath(project.paths().cacheDir(project.config().projectId, project.location().bindingId)
                     + QLatin1String("/catalog.sqlite"))
    , m_writer(std::make_unique<dataset::CatalogWriter>(
          m_databasePath, dataset::CatalogIdentity{project.config().projectId, project.location().bindingId}))
    , m_reader(std::make_unique<dataset::CatalogReader>(m_databasePath))
{
}

ProjectCatalog::~ProjectCatalog()
{
    cancel();
}

void ProjectCatalog::start()
{
    cancel();
    if (!m_writer->isOpen())
        return;
    m_cancelled = false;
    m_scanning = true;
    m_busy = true;
    m_thread = std::thread([this] { work(); });
}

void ProjectCatalog::cancel()
{
    m_cancelled = true;
    if (m_thread.joinable())
        m_thread.join();
    m_scanning = false;
    m_busy = false;
}

bool ProjectCatalog::waitForScan(int timeoutMs)
{
    return waitFor(m_scanning, timeoutMs);
}

bool ProjectCatalog::waitUntilIdle(int timeoutMs)
{
    if (!waitFor(m_busy, timeoutMs))
        return false;
    m_writer->flush();
    return true;
}

void ProjectCatalog::work()
{
    const bool complete = scan();
    // Sent before the flag drops, so whoever waited for the flag finds the signal queued.
    emit scanFinished(complete);
    m_scanning = false;
    if (!m_cancelled)
        check();
    m_writer->flush();
    emit checkFinished();
    m_busy = false;
}

bool ProjectCatalog::scan()
{
    QElapsedTimer sinceProgress;
    sinceProgress.start();
    dataset::ScanOptions options;
    options.cancelled = [this] { return m_cancelled.load(); };
    options.progress = [this, &sinceProgress](int found) {
        if (sinceProgress.elapsed() >= 200) {
            sinceProgress.restart();
            emit scanProgress(found);
        }
    };
    const qint64 generation = m_writer->beginScan();
    dataset::ScanResult result = dataset::scanDirectory(m_project.rootPath(), options);
    emit scanProgress(int(result.images.size()));
    // A cancelled listing is partial: neither its positions nor what it did not see count.
    if (!result.complete)
        return false;
    for (qsizetype first = 0; first < result.images.size(); first += kWriteChunk)
        m_writer->addScanned(generation, result.images.mid(first, kWriteChunk), int(first));
    m_writer->finishScan(generation, true,
                         {std::move(result.unsupported), std::move(result.orphanAnnotations), std::move(result.longPaths)});
    m_writer->flush();
    return true;
}

void ProjectCatalog::check()
{
    const dataset::CatalogReader reader(m_databasePath);
    if (!reader.isOpen())
        return;
    const int total = reader.needingCheckCount();
    int done = 0;
    // Images whose check did not make them current (changed again while read, or a save
    // got there first) are not read twice in one pass.
    QSet<QString> tried;
    QElapsedTimer sinceProgress;
    sinceProgress.start();
    emit checkProgress(0, total);
    while (!m_cancelled) {
        const QList<dataset::CatalogEntry> entries = reader.needingCheck(int(tried.size()) + kCheckChunk);
        bool any = false;
        for (const dataset::CatalogEntry& entry : entries) {
            if (m_cancelled)
                break;
            if (tried.contains(entry.path))
                continue;
            tried.insert(entry.path);
            any = true;
            dataset::StatusObservation observation;
            observation.path = entry.path;
            observation.sequence = m_writer->nextSequence();
            observation.status = checkOne(entry, observation);
            m_writer->record(std::move(observation));
            ++done;
            if (sinceProgress.elapsed() >= 200) {
                sinceProgress.restart();
                emit checkProgress(done, total);
            }
        }
        if (!any)
            break;
        // The next query has to see what was just written.
        m_writer->flush();
    }
    emit checkProgress(done, total);
}

AnnotationStatus ProjectCatalog::checkOne(const dataset::CatalogEntry& entry,
                                          dataset::StatusObservation& observation) const
{
    const QString imagePath = m_project.imageFilePath(entry.path);
    observation.image = versionOf(QFileInfo(imagePath));
    if (entry.annotationName.isEmpty()) {
        observation.annotation = {};
        StatusFacts facts;
        facts.nameCollision = entry.nameCollision;
        return deriveStatus(facts);
    }
    const QString jsonPath = QFileInfo(imagePath).dir().filePath(entry.annotationName);
    // Without the image size the size check is left out; reading the file still tells
    // everything else.
    ImageInfo image = m_readImageInfo ? m_readImageInfo(imagePath).value_or(ImageInfo()) : ImageInfo();
    image.fileName = QFileInfo(imagePath).fileName();
    const formats::LoadedAnnotation loaded = formats::loadAnnotationFile(jsonPath, image);
    observation.annotation = loaded.stamp.exists
                                 ? dataset::FileVersion{loaded.stamp.size, loaded.stamp.modified.toMSecsSinceEpoch()}
                                 : dataset::FileVersion{};
    return statusOfFile(loaded.file, m_project.config(), entry.nameCollision, loaded.stamp.exists);
}

void ProjectCatalog::recordSaved(const QString& imagePath, AnnotationStatus status, const formats::FileStamp& written)
{
    dataset::StatusObservation observation;
    observation.path = imagePath;
    observation.sequence = m_writer->nextSequence();
    observation.status = status;
    observation.image = versionOf(QFileInfo(m_project.imageFilePath(imagePath)));
    observation.annotation = written.exists ? dataset::FileVersion{written.size, written.modified.toMSecsSinceEpoch()}
                                            : dataset::FileVersion{};
    observation.written = true;
    observation.annotationName = QFileInfo(m_project.annotationFilePath(imagePath)).fileName();
    m_writer->record(std::move(observation));
}

} // namespace liteanno::application
