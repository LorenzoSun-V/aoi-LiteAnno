#include "dataset/CatalogWriter.h"

#include "dataset/Sqlite.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <future>

namespace liteanno::dataset {

namespace {

// Raise when the tables change: an older catalog is then rebuilt, never migrated.
const char* const kSchemaVersion = "1";

const char* const kSchema = R"sql(
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE images(
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    position INTEGER NOT NULL,
    name_collision INTEGER NOT NULL,
    image_size INTEGER NOT NULL,
    image_mtime INTEGER NOT NULL,
    annotation_name TEXT NOT NULL,
    annotation_size INTEGER NOT NULL,
    annotation_mtime INTEGER NOT NULL,
    seen_generation INTEGER NOT NULL,
    missing INTEGER NOT NULL DEFAULT 0,
    status TEXT,
    status_image_size INTEGER,
    status_image_mtime INTEGER,
    status_annotation_size INTEGER,
    status_annotation_mtime INTEGER,
    status_sequence INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX images_order ON images(missing, position);
CREATE TABLE reports(kind TEXT NOT NULL, path TEXT NOT NULL);
)sql";

void removeDatabase(const QString& path)
{
    for (const char* suffix : {"", "-wal", "-shm", "-journal"})
        QFile::remove(path + QLatin1String(suffix));
}

QString metaValue(sqlite3* db, const char* key)
{
    sqlite::Statement select(db, "SELECT value FROM meta WHERE key = ?1");
    select.bind(1, QString::fromLatin1(key));
    return select.step() == SQLITE_ROW ? select.text(0) : QString();
}

bool setMeta(sqlite3* db, const char* key, const QString& value)
{
    sqlite::Statement upsert(db, "INSERT INTO meta(key, value) VALUES(?1, ?2) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    return upsert.bind(1, QString::fromLatin1(key)).bind(2, value).run();
}

} // namespace

CatalogWriter::CatalogWriter(const QString& databasePath, const CatalogIdentity& identity)
{
    if (!open(databasePath, identity)) {
        // A damaged file is only a cache: start over once.
        if (m_db)
            sqlite3_close(m_db);
        m_db = nullptr;
        removeDatabase(databasePath);
        m_startedEmpty = true;
        if (!open(databasePath, identity)) {
            if (m_db)
                sqlite3_close(m_db);
            m_db = nullptr;
        }
    }
    m_thread = std::thread([this] { run(); });
}

CatalogWriter::~CatalogWriter()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_changed.notify_all();
    m_thread.join();
    if (m_db)
        sqlite3_close(m_db);
}

bool CatalogWriter::open(const QString& path, const CatalogIdentity& identity)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray file = QFile::encodeName(path);
    if (sqlite3_open_v2(file.constData(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        setError(sqlite::lastError(m_db));
        return false;
    }
    sqlite3_busy_timeout(m_db, 5000);
    // WAL lets the readers go on while this connection writes; NORMAL is enough for a cache.
    if (!sqlite::exec(m_db, "PRAGMA journal_mode = WAL") || !sqlite::exec(m_db, "PRAGMA synchronous = NORMAL")) {
        setError(sqlite::lastError(m_db));
        return false;
    }
    sqlite::Statement tables(m_db, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = 'meta'");
    if (tables.step() != SQLITE_ROW) {
        setError(sqlite::lastError(m_db));
        return false;
    }
    if (tables.integer(0) == 0) {
        m_startedEmpty = true;
        const bool created = transaction([&] {
            return sqlite::exec(m_db, kSchema) && setMeta(m_db, "schemaVersion", QLatin1String(kSchemaVersion))
                   && setMeta(m_db, "projectId", identity.projectId)
                   && setMeta(m_db, "bindingId", identity.bindingId) && setMeta(m_db, "generation", QStringLiteral("0"))
                   && setMeta(m_db, "completeGeneration", QStringLiteral("0"));
        });
        return created;
    }
    if (metaValue(m_db, "schemaVersion") != QLatin1String(kSchemaVersion)
        || metaValue(m_db, "projectId") != identity.projectId || metaValue(m_db, "bindingId") != identity.bindingId)
        return false;
    sqlite::Statement last(m_db, "SELECT max(status_sequence) FROM images");
    if (last.step() != SQLITE_ROW) {
        setError(sqlite::lastError(m_db));
        return false;
    }
    m_sequence = last.integer(0);
    return true;
}

QString CatalogWriter::error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

void CatalogWriter::setError(const QString& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_error = error;
}

bool CatalogWriter::transaction(const std::function<bool()>& work)
{
    if (!sqlite::exec(m_db, "BEGIN IMMEDIATE")) {
        setError(sqlite::lastError(m_db));
        return false;
    }
    if (work() && sqlite::exec(m_db, "COMMIT"))
        return true;
    setError(sqlite::lastError(m_db));
    sqlite::exec(m_db, "ROLLBACK");
    return false;
}

void CatalogWriter::post(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(task));
    }
    m_changed.notify_all();
}

void CatalogWriter::run()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            // Stopping still writes what was sent.
            if (m_queue.empty())
                return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
            m_busy = true;
        }
        if (m_db)
            task();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_busy = false;
        }
        m_changed.notify_all();
    }
}

void CatalogWriter::flush()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_changed.wait(lock, [this] { return m_queue.empty() && !m_busy; });
}

qint64 CatalogWriter::beginScan()
{
    if (!m_db)
        return 0;
    auto generation = std::make_shared<std::promise<qint64>>();
    std::future<qint64> result = generation->get_future();
    post([this, generation] {
        qint64 next = 0;
        transaction([&] {
            next = metaValue(m_db, "generation").toLongLong() + 1;
            return setMeta(m_db, "generation", QString::number(next));
        });
        generation->set_value(next);
    });
    return result.get();
}

void CatalogWriter::addScanned(qint64 generation, QList<ScannedImage> images, int firstPosition)
{
    post([this, generation, images = std::move(images), firstPosition] {
        transaction([&] {
            sqlite::Statement upsert(
                m_db,
                "INSERT INTO images(path, position, name_collision, image_size, image_mtime, annotation_name, "
                "annotation_size, annotation_mtime, seen_generation, missing) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 0) "
                "ON CONFLICT(path) DO UPDATE SET position = excluded.position, "
                "name_collision = excluded.name_collision, image_size = excluded.image_size, "
                "image_mtime = excluded.image_mtime, annotation_name = excluded.annotation_name, "
                "annotation_size = excluded.annotation_size, annotation_mtime = excluded.annotation_mtime, "
                "seen_generation = excluded.seen_generation, missing = 0");
            int position = firstPosition;
            for (const ScannedImage& image : images) {
                upsert.bind(1, image.path)
                    .bind(2, position++)
                    .bind(3, image.nameCollision ? 1 : 0)
                    .bind(4, image.image.size)
                    .bind(5, image.image.modifiedMs)
                    .bind(6, image.annotationName)
                    .bind(7, image.annotation.size)
                    .bind(8, image.annotation.modifiedMs)
                    .bind(9, generation);
                if (!upsert.run())
                    return false;
            }
            return true;
        });
    });
}

void CatalogWriter::finishScan(qint64 generation, bool complete, ScanReports reports)
{
    if (!complete)
        return;  // architecture 6.2: an incomplete scan says nothing about what is gone
    post([this, generation, reports = std::move(reports)] {
        transaction([&] {
            sqlite::Statement missing(m_db, "UPDATE images SET missing = 1 WHERE seen_generation < ?1");
            if (!missing.bind(1, generation).run() || !sqlite::exec(m_db, "DELETE FROM reports"))
                return false;
            sqlite::Statement insert(m_db, "INSERT INTO reports(kind, path) VALUES(?1, ?2)");
            for (const auto& [kind, paths] : {std::pair{"unsupported", &reports.unsupported},
                                              std::pair{"orphan", &reports.orphanAnnotations},
                                              std::pair{"long", &reports.longPaths}}) {
                for (const QString& path : *paths) {
                    if (!insert.bind(1, QString::fromLatin1(kind)).bind(2, path).run())
                        return false;
                }
            }
            return setMeta(m_db, "completeGeneration", QString::number(generation));
        });
    });
}

void CatalogWriter::record(StatusObservation observation)
{
    post([this, observation = std::move(observation)] {
        transaction([&] {
            // Architecture 6.3: what was read before the last save, or before the last check
            // that already got through, is older than what the catalog has.
            sqlite::Statement update(
                m_db, observation.written
                          ? "UPDATE images SET status = ?2, status_image_size = ?3, status_image_mtime = ?4, "
                            "status_annotation_size = ?5, status_annotation_mtime = ?6, status_sequence = ?7, "
                            "annotation_size = ?5, annotation_mtime = ?6, annotation_name = ?8 "
                            "WHERE path = ?1 AND status_sequence < ?7"
                          : "UPDATE images SET status = ?2, status_image_size = ?3, status_image_mtime = ?4, "
                            "status_annotation_size = ?5, status_annotation_mtime = ?6, status_sequence = ?7 "
                            "WHERE path = ?1 AND status_sequence < ?7");
            update.bind(1, observation.path)
                .bind(2, QString::fromLatin1(annotationStatusName(observation.status)))
                .bind(3, observation.image.size)
                .bind(4, observation.image.modifiedMs)
                .bind(5, observation.annotation.size)
                .bind(6, observation.annotation.modifiedMs)
                .bind(7, observation.sequence);
            if (observation.written)
                update.bind(8, observation.annotationName);
            return update.run();
        });
    });
}

} // namespace liteanno::dataset
