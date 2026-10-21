#pragma once

#include "application/DocumentSession.h"
#include "application/SaveCoordinator.h"

#include <QHash>
#include <QSet>
#include <QObject>
#include <QString>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace liteanno::application {

struct SaveJobResult {
    QString imagePath;
    quint64 sessionId = 0;
    quint64 entryId = 0;
    SaveOutcome outcome;
    // The clone that was saved, and the file after the save; for DocumentSession::savedSnapshot.
    // After a failed save, the edits that were not written and the file they expect.
    std::shared_ptr<formats::AnnotationFile> saved;
    formats::FileStamp stamp;
};

// Saves documents on a worker thread (architecture 8.1, 8.2). Each image has at most one
// waiting snapshot: a newer one replaces it. A global limit on waiting documents and bytes
// keeps a fast run through hundreds of images from piling up unsaved edits; the window waits
// at the limit and says "正在保存".
class BackgroundSaver : public QObject
{
    Q_OBJECT

public:
    struct Limits {
        int maxDocuments = 32;
        qint64 maxBytes = 32 * 1024 * 1024;
    };

    BackgroundSaver(SaveCoordinator coordinator, Limits limits, QObject* parent = nullptr);
    explicit BackgroundSaver(SaveCoordinator coordinator, QObject* parent = nullptr);
    // Finishes the saves already handed over.
    ~BackgroundSaver() override;

    void submit(SaveSnapshot snapshot, const formats::SaveOptions& options = {});

    // Waiting and running jobs.
    int pendingCount() const;
    qint64 pendingBytes() const;
    bool isPending(const QString& imagePath) const;
    bool atLimit() const;

    // Block until the condition holds or `timeoutMs` passes; true when it holds.
    bool waitUntilIdle(const QString& imagePath, int timeoutMs = -1);
    bool waitUntilBelowLimit(int timeoutMs = -1);
    bool waitUntilAllDone(int timeoutMs = -1);

    // Finished saves, oldest first. Call on the thread that owns the documents.
    std::vector<SaveJobResult> takeResults();
    // Finished saves not taken yet. A save leaves pendingCount() and arrives here at once.
    bool hasResults() const;

    const SaveCoordinator& coordinator() const { return m_coordinator; }

signals:
    // A save finished; emitted on the worker thread.
    void resultsReady();

private:
    struct Job {
        SaveSnapshot snapshot;
        formats::SaveOptions options;
        qint64 bytes = 0;
    };

    void run();
    template <typename Predicate>
    bool waitFor(int timeoutMs, Predicate&& done);
    bool isPendingLocked(const QString& imagePath) const;

    SaveCoordinator m_coordinator;
    Limits m_limits;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Job> m_queue;
    QString m_running;  // the image being saved, empty when none
    qint64 m_runningBytes = 0;
    std::vector<SaveJobResult> m_results;
    // This program's writes of each image: the file contents they replaced and the file
    // after the last one. A snapshot taken before the window learned of a write still
    // describes a replaced content; see run().
    struct Write {
        quint64 sessionId = 0;
        QSet<QString> replaced;
        formats::FileStamp after;
    };
    QHash<QString, Write> m_lastWrites;  // used by the worker thread only
    bool m_stopping = false;
    std::thread m_worker;
};

} // namespace liteanno::application
