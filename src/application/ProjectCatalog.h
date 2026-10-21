#pragma once

#include "application/ProjectSession.h"
#include "core/AnnotationStatus.h"
#include "core/ImageInfo.h"
#include "dataset/CatalogReader.h"
#include "dataset/CatalogWriter.h"
#include "formats/AnnotationStore.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace liteanno::application {

// The catalog of an open project (architecture 6): runs the two scan stages in the
// background and keeps the catalog up to date with this program's saves.
//
// Stage 1 lists the folder; stage 2 reads the annotation files whose status is not known
// for the files as listed. Both can be cancelled; a cancelled first stage marks nothing as
// missing (6.2).
class ProjectCatalog : public QObject
{
    Q_OBJECT

public:
    // Reads the size and orientation of an image without decoding it where possible. The
    // application layer has no image codecs, so the window passes them in.
    using ImageInfoReader = std::function<std::optional<ImageInfo>(const QString& absolutePath)>;

    ProjectCatalog(const ProjectSession& project, ImageInfoReader readImageInfo, QObject* parent = nullptr);
    // Stops the background work and writes what was found.
    ~ProjectCatalog() override;

    QString databasePath() const { return m_databasePath; }
    bool isOpen() const { return m_writer->isOpen(); }
    QString error() const { return m_writer->error(); }
    // For the window, on its thread.
    const dataset::CatalogReader& reader() const { return *m_reader; }

    // Scans the folder, then checks the statuses. Cancels a scan that is running.
    void start();
    // Stops the background work and waits for it.
    void cancel();
    bool isScanning() const { return m_scanning; }
    bool isBusy() const { return m_busy; }
    // Waits until the first stage is over (tests, and opening a project that has no catalog
    // yet). False on timeout.
    bool waitForScan(int timeoutMs);
    // Waits until both stages are over and everything is written.
    bool waitUntilIdle(int timeoutMs);

    // A save of this program succeeded: `status` holds for the file as written.
    void recordSaved(const QString& imagePath, AnnotationStatus status, const formats::FileStamp& written);

signals:
    void scanProgress(int imagesFound);
    void scanFinished(bool complete);
    void checkProgress(int done, int total);
    void checkFinished();

private:
    void work();
    bool scan();
    void check();
    AnnotationStatus checkOne(const dataset::CatalogEntry& entry, dataset::StatusObservation& observation) const;

    const ProjectSession& m_project;
    ImageInfoReader m_readImageInfo;
    QString m_databasePath;
    std::unique_ptr<dataset::CatalogWriter> m_writer;
    std::unique_ptr<dataset::CatalogReader> m_reader;
    std::thread m_thread;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_busy{false};
};

} // namespace liteanno::application
