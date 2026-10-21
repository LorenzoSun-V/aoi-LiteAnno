#include "application/BackgroundSaver.h"

#include <algorithm>
#include <chrono>
#include <optional>

namespace liteanno::application {

namespace {

// Identifies a file content, or a missing file.
QString contentKey(const formats::FileStamp& stamp)
{
    return stamp.exists ? stamp.digest : QStringLiteral("missing");
}

} // namespace

BackgroundSaver::BackgroundSaver(SaveCoordinator coordinator, Limits limits, QObject* parent)
    : QObject(parent)
    , m_coordinator(std::move(coordinator))
    , m_limits(limits)
{
    m_worker = std::thread([this] { run(); });
}

BackgroundSaver::BackgroundSaver(SaveCoordinator coordinator, QObject* parent)
    : BackgroundSaver(std::move(coordinator), Limits(), parent)
{
}

BackgroundSaver::~BackgroundSaver()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_changed.notify_all();
    m_worker.join();
}

void BackgroundSaver::submit(SaveSnapshot snapshot, const formats::SaveOptions& options)
{
    const qint64 bytes = snapshot.annotation.file.toBytes().size();
    Job job{std::move(snapshot), options, bytes};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto waiting = std::find_if(m_queue.begin(), m_queue.end(), [&job](const Job& queued) {
            return queued.snapshot.imagePath == job.snapshot.imagePath;
        });
        if (waiting != m_queue.end())
            *waiting = std::move(job);
        else
            m_queue.push_back(std::move(job));
    }
    m_changed.notify_all();
}

bool BackgroundSaver::isPendingLocked(const QString& imagePath) const
{
    return m_running == imagePath
           || std::any_of(m_queue.begin(), m_queue.end(),
                          [&imagePath](const Job& job) { return job.snapshot.imagePath == imagePath; });
}

int BackgroundSaver::pendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return int(m_queue.size()) + (m_running.isEmpty() ? 0 : 1);
}

qint64 BackgroundSaver::pendingBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    qint64 bytes = m_runningBytes;
    for (const Job& job : m_queue)
        bytes += job.bytes;
    return bytes;
}

bool BackgroundSaver::isPending(const QString& imagePath) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return isPendingLocked(imagePath);
}

bool BackgroundSaver::atLimit() const
{
    return pendingCount() >= m_limits.maxDocuments || pendingBytes() >= m_limits.maxBytes;
}

template <typename Predicate>
bool BackgroundSaver::waitFor(int timeoutMs, Predicate&& done)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (timeoutMs < 0) {
        m_changed.wait(lock, done);
        return true;
    }
    return m_changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), done);
}

bool BackgroundSaver::waitUntilIdle(const QString& imagePath, int timeoutMs)
{
    return waitFor(timeoutMs, [&] { return !isPendingLocked(imagePath); });
}

bool BackgroundSaver::waitUntilBelowLimit(int timeoutMs)
{
    return waitFor(timeoutMs, [this] {
        qint64 bytes = m_runningBytes;
        for (const Job& job : m_queue)
            bytes += job.bytes;
        const int count = int(m_queue.size()) + (m_running.isEmpty() ? 0 : 1);
        return count < m_limits.maxDocuments && bytes < m_limits.maxBytes;
    });
}

bool BackgroundSaver::waitUntilAllDone(int timeoutMs)
{
    return waitFor(timeoutMs, [this] { return m_queue.empty() && m_running.isEmpty(); });
}

std::vector<SaveJobResult> BackgroundSaver::takeResults()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::exchange(m_results, {});
}

void BackgroundSaver::run()
{
    for (;;) {
        std::optional<Job> next;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            // Stopping still finishes the queue: those edits were handed over to be saved.
            if (m_queue.empty())
                return;
            next.emplace(std::move(m_queue.front()));
            m_queue.pop_front();
            m_running = next->snapshot.imagePath;
            m_runningBytes = next->bytes;
        }
        Job& job = *next;
        // Taken while this image's previous save ran: the snapshot still expects the file
        // as it was before that save, which is not another program's change.
        const QString expected = contentKey(job.snapshot.annotation.stamp);
        Write& writes = m_lastWrites[job.snapshot.imagePath];
        if (writes.replaced.contains(expected))
            job.snapshot.annotation.stamp = writes.after;

        SaveJobResult result;
        result.imagePath = job.snapshot.imagePath;
        result.entryId = job.snapshot.entryId;
        result.outcome = m_coordinator.save(job.snapshot.imagePath, job.snapshot.annotation,
                                            job.snapshot.documentVersion, job.options);
        result.stamp = job.snapshot.annotation.stamp;
        if (result.outcome.result.status == formats::SaveStatus::Saved) {
            writes.replaced.insert(expected);
            if (writes.after.exists)
                writes.replaced.insert(contentKey(writes.after));
            writes.after = result.stamp;
        }
        result.saved = std::make_shared<formats::AnnotationFile>(std::move(job.snapshot.annotation.file));

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_results.push_back(std::move(result));
            m_running.clear();
            m_runningBytes = 0;
        }
        m_changed.notify_all();
        emit resultsReady();
    }
}

} // namespace liteanno::application
