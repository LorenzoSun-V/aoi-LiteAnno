#pragma once

#include "core/AnnotationStatus.h"
#include "core/ProjectConfig.h"
#include "core/Validation.h"
#include "formats/AnnotationStore.h"

#include <QList>
#include <QString>

namespace liteanno::application {

// What a background save works on: a copy of the file at one point of the edit history.
struct SaveSnapshot {
    QString imagePath;
    formats::LoadedAnnotation annotation;  // a clone; the session keeps editing its own
    quint64 entryId = 0;                   // the history entry the copy was taken at
    qint64 documentVersion = 0;
};

// One image being edited: its file, the edit history for undo and redo (format spec 5.2),
// and the confirm conditions and status for the project (5.1, 5.3).
class DocumentSession
{
public:
    DocumentSession(QString imagePath, formats::LoadedAnnotation annotation, const ProjectConfig& project,
                    bool nameCollision = false);

    const QString& imagePath() const { return m_imagePath; }
    const AnnotationDocument& document() const { return m_annotation.file.document(); }
    const formats::AnnotationFile& file() const { return m_annotation.file; }
    bool isReadOnly() const { return m_nameCollision || m_annotation.file.isReadOnly(); }

    // Edits. Each one that changes something becomes one undo step; read-only documents
    // refuse all of them.
    qsizetype addShape(const Shape& shape);
    bool setShapePoints(qsizetype index, const PointList& points);
    bool setShapeLabel(qsizetype index, const QString& label);
    bool removeShape(qsizetype index);
    bool setImageLabel(const QString& name, bool value);
    // Single choice: selects `name` and clears the other configured options in one step.
    bool selectSingleImageLabel(const QString& name);
    bool confirm();  // refused while confirmBlockers() is not empty
    bool skip(const QString& reason);
    bool unskip();

    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index + 1 < m_history.size(); }
    bool undo();
    bool redo();

    QList<ConfirmBlocker> confirmBlockers() const;
    AnnotationStatus status() const;

    // Saving. needsSave() says whether the current content differs from the file on disk.
    bool needsSave() const { return m_annotation.file.needsWrite(); }
    SaveSnapshot snapshot() const;
    // A save of `snapshot` finished; `saved` is the clone it wrote and `stamp` the file after.
    void savedSnapshot(quint64 entryId, const formats::AnnotationFile& saved, const formats::FileStamp& stamp);
    // "用我的版本覆盖" needs nothing here; "重新加载" replaces the session with a new one.

    // Grows with every change, undo and redo included.
    qint64 version() const { return m_version; }

private:
    struct Entry {
        AnnotationDocument state;
        quint64 id = 0;
    };

    template <typename Edit>
    bool edit(Edit&& apply);
    void moveTo(qsizetype index);

    QString m_imagePath;
    formats::LoadedAnnotation m_annotation;
    ProjectConfig m_project;
    bool m_nameCollision = false;
    QList<Entry> m_history;
    qsizetype m_index = 0;
    quint64 m_nextEntryId = 1;
    quint64 m_savedEntryId = 0;  // the entry whose content is on disk; 0 when none
    qint64 m_version = 0;
};

} // namespace liteanno::application
