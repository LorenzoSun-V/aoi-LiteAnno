#pragma once

#include "application/ProjectSession.h"
#include "core/AnnotationStatus.h"
#include "core/ImageInfo.h"
#include "dataset/CatalogReader.h"
#include "dataset/CatalogWriter.h"
#include "formats/AnnotationStore.h"

#include <QObject>
#include <QString>
#include <QTemporaryDir>

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
    // False when neither the cache folder nor a temporary folder can hold a catalog; the
    // window then lists the folder itself.
    bool isOpen() const { return m_writer && m_writer->isOpen(); }
    // The cache folder could not be used: the catalog is in a temporary folder and is gone
    // when the project closes.
    bool isTemporary() const { return m_temporary != nullptr; }
    QString error() const { return m_writer ? m_writer->error() : m_error; }
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

    // A save of this program succeeded: `status` holds for the file as written, derived with
    // `nameCollision` as the window knew it.
    void recordSaved(const QString& imagePath, AnnotationStatus status, const formats::FileStamp& written,
                     bool nameCollision);

    // Tests: runs on the scan thread after the folder is listed, before the catalog is written.
    void setAfterListing(std::function<void()> hook) { m_afterListing = std::move(hook); }

signals:
    void scanProgress(int imagesFound);
    // The listing is in the catalog. Not complete: some folders could not be listed (see
    // the reports); their images are kept as they were. Not sent for a cancelled scan.
    void scanFinished(bool complete);
    void checkProgress(int done, int total);
    void checkFinished();

private:
    enum class ScanOutcome { Cancelled, Incomplete, Complete };

    void open(const QString& databasePath);
    void work();
    ScanOutcome scan();
    void check();
    AnnotationStatus checkOne(const dataset::CatalogEntry& entry, dataset::StatusObservation& observation) const;

    const ProjectSession& m_project;
    ImageInfoReader m_readImageInfo;
    std::unique_ptr<QTemporaryDir> m_temporary;
    QString m_databasePath;
    std::unique_ptr<dataset::CatalogWriter> m_writer;
    std::unique_ptr<dataset::CatalogReader> m_reader;
    QString m_error;
    std::thread m_thread;
    std::function<void()> m_afterListing;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_busy{false};
};

} // namespace liteanno::application
