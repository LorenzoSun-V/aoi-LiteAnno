#pragma once

#include "dataset/Catalog.h"

#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

struct sqlite3;

namespace liteanno::dataset {

// The only connection that writes the catalog (architecture 6.3), on its own thread. Scan
// results, status checks and save acknowledgements arrive as messages and are written in
// the order they were sent. Reading goes through CatalogReader.
class CatalogWriter
{
public:
    // Opens the catalog file, creating it when needed. A file that cannot be read, has
    // another layout, or belongs to another project or location is replaced by an empty one.
    CatalogWriter(const QString& databasePath, const CatalogIdentity& identity);
    // Writes what was sent, then closes.
    ~CatalogWriter();

    CatalogWriter(const CatalogWriter&) = delete;
    CatalogWriter& operator=(const CatalogWriter&) = delete;

    bool isOpen() const { return m_db != nullptr; }
    // Why the catalog could not be opened, or the last write that failed.
    QString error() const;
    // The file was empty or replaced when opened: nothing from earlier is known.
    bool startedEmpty() const { return m_startedEmpty; }

    // For StatusObservation::sequence. Thread-safe; always larger than any sequence used
    // before, also by earlier runs on this catalog.
    qint64 nextSequence() { return ++m_sequence; }

    // Starts a full scan and returns its generation (architecture 6.2). Waits for the writer.
    qint64 beginScan();
    // Images of the scan, in order: `firstPosition` is the natural position of the first one.
    void addScanned(qint64 generation, QList<ScannedImage> images, int firstPosition);
    // Only a complete scan marks the images it did not see as missing and replaces the reports.
    void finishScan(qint64 generation, bool complete, ScanReports reports);
    void record(StatusObservation observation);
    // Waits until everything sent so far is written.
    void flush();

private:
    bool open(const QString& path, const CatalogIdentity& identity);
    void post(std::function<void()> task);
    void run();
    void setError(const QString& error);
    // Runs `work` in one transaction; false and the error kept when it fails.
    bool transaction(const std::function<bool()>& work);

    sqlite3* m_db = nullptr;
    bool m_startedEmpty = false;
    std::atomic<qint64> m_sequence{0};

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::function<void()>> m_queue;
    bool m_busy = false;
    bool m_stopping = false;
    QString m_error;
    std::thread m_thread;
};

} // namespace liteanno::dataset
