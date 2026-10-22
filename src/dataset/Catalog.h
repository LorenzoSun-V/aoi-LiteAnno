#pragma once

#include "core/AnnotationStatus.h"
#include "dataset/DirectoryScanner.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace liteanno::dataset {

// The local catalog (architecture 6): one SQLite file in the cache folder of a project
// location (format spec 8). It only repeats what the data folder says, so it can be deleted
// at any time and is built again by the next scan.

// Which project and location the catalog belongs to. A catalog file that says otherwise is
// rebuilt rather than trusted.
struct CatalogIdentity {
    QString projectId;
    QString bindingId;
    // The project's rules (its project file, by digest). Not part of the identity: statuses
    // derived under other rules are only no longer current (format spec 5.1).
    QString rules;
};

// What the second scan stage (architecture 6.1) read for one image, or what a save of this
// program wrote. `sequence` comes from CatalogWriter::nextSequence() when the reading
// started, or when the save was acknowledged; an observation older than the one the catalog
// has is dropped (architecture 6.3).
struct StatusObservation {
    QString path;
    qint64 sequence = 0;
    AnnotationStatus status = AnnotationStatus::Unprocessed;
    FileVersion image;       // the versions the status was derived from
    FileVersion annotation;  // size -1: no annotation file
    bool nameCollision = false;  // the status was derived with or without a name collision
    // A save of this program: the annotation file is now this one, whatever the last
    // scan listed.
    bool written = false;
    QString annotationName;  // with `written`: the file's name
};

struct CatalogEntry {
    QString path;
    int position = 0;            // natural order of the last scan that listed it
    bool nameCollision = false;
    bool missing = false;        // not found by the last complete scan
    FileVersion image;
    QString annotationName;      // empty when there is no annotation file
    FileVersion annotation;
    // Status read from the files; empty until checked. It holds for the files as listed, the
    // project's rules and the name collision only when `verified`.
    std::optional<AnnotationStatus> status;
    bool verified = false;
};

// Recognized files the scan reports on (format spec 2), from the last complete scan.
struct ScanReports {
    QStringList unsupported;
    QStringList orphanAnnotations;
    QStringList longPaths;
    QStringList unreadable;  // folders that could not be listed
};

} // namespace liteanno::dataset
