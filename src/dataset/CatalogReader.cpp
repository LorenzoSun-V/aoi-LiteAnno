#include "dataset/CatalogReader.h"

#include "dataset/Sqlite.h"

#include <QFile>

namespace liteanno::dataset {

namespace {

const char* const kColumns =
    "SELECT path, position, name_collision, missing, image_size, image_mtime, annotation_name, "
    "annotation_size, annotation_mtime, status, "
    "status IS NOT NULL AND status_image_size IS image_size AND status_image_mtime IS image_mtime "
    "AND status_annotation_size IS annotation_size AND status_annotation_mtime IS annotation_mtime "
    "FROM images ";

const char* const kUnchecked =
    "(status IS NULL OR status_image_size IS NOT image_size OR status_image_mtime IS NOT image_mtime "
    "OR status_annotation_size IS NOT annotation_size OR status_annotation_mtime IS NOT annotation_mtime)";

CatalogEntry entryOf(const sqlite::Statement& row)
{
    CatalogEntry entry;
    entry.path = row.text(0);
    entry.position = int(row.integer(1));
    entry.nameCollision = row.integer(2) != 0;
    entry.missing = row.integer(3) != 0;
    entry.image = {row.integer(4), row.integer(5)};
    entry.annotationName = row.text(6);
    entry.annotation = {row.integer(7), row.integer(8)};
    if (!row.isNull(9))
        entry.status = annotationStatusFromName(row.text(9));
    entry.verified = entry.status.has_value() && row.integer(10) != 0;
    return entry;
}

} // namespace

CatalogReader::CatalogReader(const QString& databasePath)
{
    const QByteArray file = QFile::encodeName(databasePath);
    if (sqlite3_open_v2(file.constData(), &m_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(m_db);
        m_db = nullptr;
        return;
    }
    sqlite3_busy_timeout(m_db, 5000);
}

CatalogReader::~CatalogReader()
{
    sqlite3_close(m_db);
}

QList<CatalogEntry> CatalogReader::images(bool includeMissing) const
{
    QList<CatalogEntry> entries;
    sqlite::Statement select(m_db, (QByteArray(kColumns)
                                    + (includeMissing ? "ORDER BY missing, position" : "WHERE missing = 0 ORDER BY position"))
                                       .constData());
    while (select.step() == SQLITE_ROW)
        entries.append(entryOf(select));
    return entries;
}

std::optional<CatalogEntry> CatalogReader::image(const QString& path) const
{
    sqlite::Statement select(m_db, (QByteArray(kColumns) + "WHERE path = ?1").constData());
    select.bind(1, path);
    if (select.step() != SQLITE_ROW)
        return std::nullopt;
    return entryOf(select);
}

QList<CatalogEntry> CatalogReader::needingCheck(int limit) const
{
    QList<CatalogEntry> entries;
    sqlite::Statement select(
        m_db, (QByteArray(kColumns) + "WHERE missing = 0 AND " + kUnchecked + " ORDER BY position LIMIT ?1").constData());
    select.bind(1, limit);
    while (select.step() == SQLITE_ROW)
        entries.append(entryOf(select));
    return entries;
}

int CatalogReader::needingCheckCount() const
{
    sqlite::Statement count(m_db, (QByteArray("SELECT count(*) FROM images WHERE missing = 0 AND ") + kUnchecked).constData());
    return count.step() == SQLITE_ROW ? int(count.integer(0)) : 0;
}

QHash<AnnotationStatus, int> CatalogReader::statusCounts() const
{
    QHash<AnnotationStatus, int> counts;
    sqlite::Statement select(m_db, "SELECT status, count(*) FROM images WHERE missing = 0 AND status IS NOT NULL "
                                   "GROUP BY status");
    while (select.step() == SQLITE_ROW) {
        if (const auto status = annotationStatusFromName(select.text(0)))
            counts.insert(*status, int(select.integer(1)));
    }
    return counts;
}

qint64 CatalogReader::completeGeneration() const
{
    sqlite::Statement select(m_db, "SELECT value FROM meta WHERE key = 'completeGeneration'");
    return select.step() == SQLITE_ROW ? select.text(0).toLongLong() : 0;
}

ScanReports CatalogReader::reports() const
{
    ScanReports reports;
    sqlite::Statement select(m_db, "SELECT kind, path FROM reports ORDER BY rowid");
    while (select.step() == SQLITE_ROW) {
        const QString kind = select.text(0);
        if (kind == QLatin1String("unsupported"))
            reports.unsupported.append(select.text(1));
        else if (kind == QLatin1String("orphan"))
            reports.orphanAnnotations.append(select.text(1));
        else if (kind == QLatin1String("long"))
            reports.longPaths.append(select.text(1));
    }
    return reports;
}

} // namespace liteanno::dataset
