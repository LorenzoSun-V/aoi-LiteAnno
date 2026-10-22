#pragma once

#include "dataset/Catalog.h"

#include <QHash>
#include <QList>
#include <QString>

#include <optional>

struct sqlite3;

namespace liteanno::dataset {

// A read-only connection to the catalog (architecture 6.3). Each thread that reads opens
// its own. Sees what the writer committed.
class CatalogReader
{
public:
    explicit CatalogReader(const QString& databasePath);
    ~CatalogReader();

    CatalogReader(const CatalogReader&) = delete;
    CatalogReader& operator=(const CatalogReader&) = delete;

    bool isOpen() const { return m_db != nullptr; }

    // The images the last scans found, in natural order; missing ones only when asked for.
    QList<CatalogEntry> images(bool includeMissing = false) const;
    std::optional<CatalogEntry> image(const QString& path) const;
    // Present images whose status is not known for their files as listed, in order.
    QList<CatalogEntry> needingCheck(int limit) const;
    int needingCheckCount() const;
    // Present images by checked status; unchecked ones are not counted.
    QHash<AnnotationStatus, int> statusCounts() const;

    // The generation of the last complete scan; 0 when no scan has completed yet.
    qint64 completeGeneration() const;
    ScanReports reports() const;

private:
    sqlite3* m_db = nullptr;
};

} // namespace liteanno::dataset
