#pragma once

#include "formats/AnnotationStore.h"
#include "formats/WorksetFiles.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace liteanno::application {

// The batches of a project (format spec 7): fixed lists of images in liteanno/worksets/,
// created when the user starts the next one. Which images belong to a batch, its number and
// its ID come only from these files, never from the local catalog.
class WorksetStore
{
public:
    struct Batch {
        QString stem;  // "b000037"; the list is <stem>.list.json, its progress <stem>.progress.json
        int number = 0;
        std::optional<formats::WorksetList> list;  // nothing when the list is damaged
        QString error;            // why the list cannot be used
        QStringList damagedItems;  // a damaged list: the paths it names, as far as they can be read
    };

    // `root` is the data root; `projectId` the one of its project file.
    WorksetStore(QString root, QString projectId);

    QString directory() const { return m_directory; }

    // Reads the list files again. False when the folder exists but cannot be listed: then
    // nothing is known about which images are given out, and no batch may be created.
    bool load();
    // Ordered by number.
    const QList<Batch>& batches() const { return m_batches; }
    const Batch* find(const QString& stem) const;
    // Whether a list, damaged or not, names the image.
    bool isAssigned(const QString& imagePath) const { return m_assigned.contains(imagePath); }
    // Images named by more than one list (a warning only, format spec 7.1).
    QStringList duplicates() const;
    bool loaded() const { return m_loaded; }

    struct Created {
        const Batch* batch = nullptr;
        QString error;
    };
    // Creates the next batch from `candidates`, the images of the last complete scan without
    // a name collision, in natural order: the first `batchSize` not in any list. The file is
    // created exclusively; when another program took the number meanwhile, the next is used.
    // No batch when nothing is left to give out (error then empty).
    Created createNext(const QStringList& candidates, int batchSize,
                       const QDateTime& now = QDateTime::currentDateTime());

    // The progress of a batch; nothing when it is missing, damaged or belongs to another list
    // (format spec 7.2: only the last position is lost).
    std::optional<formats::WorksetProgress> progress(const Batch& batch) const;
    formats::SaveResult writeProgress(const Batch& batch, const formats::WorksetProgress& progress) const;
    // The batch to continue: the one whose progress was written last, else the usable one
    // with the lowest number. nullptr when no list is usable.
    const Batch* lastWorkedOn() const;

    static QString timestamp(const QDateTime& time);

private:
    QString listPath(const QString& stem) const;
    QString progressPath(const QString& stem) const;

    QString m_root;
    QString m_projectId;
    QString m_directory;
    QList<Batch> m_batches;
    QHash<QString, int> m_assigned;  // image path -> number of lists naming it
    bool m_loaded = false;
};

} // namespace liteanno::application
