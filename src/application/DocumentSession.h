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
    quint64 sessionId = 0;                 // the DocumentSession it came from
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
    bool isReadOnly() const { return m_nameCollision || m_blocked || m_annotation.file.isReadOnly(); }
    // Refuses edits for a reason outside the file, e.g. a recovery record the user has not
    // decided on yet: saving would replace that record.
    void setBlocked(bool blocked) { m_blocked = blocked; }
    bool isBlocked() const { return m_blocked; }
    // Another image with the same stem appeared or went away (format spec 2): with a
    // collision the shared annotation file must not be written.
    void setNameCollision(bool collision) { m_nameCollision = collision; }
    bool hasNameCollision() const { return m_nameCollision; }

    // Edits. Each one that changes something becomes one undo step; read-only documents
    // refuse all of them.
    qsizetype addShape(const Shape& shape);
    bool setShapePoints(qsizetype index, const PointList& points);
    bool setShapeRotation(qsizetype index, const RotatedBox& box);
    bool setShapeLabel(qsizetype index, const QString& label);
    bool removeShape(qsizetype index);
    bool setImageLabel(const QString& name, bool value);
    // Single choice: selects `name` and clears the other configured options in one step.
    bool selectSingleImageLabel(const QString& name);
    bool confirm();  // refused while confirmBlockers() is not empty
    bool skip(const QString& reason);
    bool unskip();

    // "按当前图片尺寸更正" (format spec 5.3): lifts a size mismatch; the next save writes the
    // image's actual size. Not an undo step.
    bool correctImageSize();

    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index + 1 < m_history.size(); }
    bool undo();
    bool redo();

    QList<ConfirmBlocker> confirmBlockers() const;
    AnnotationStatus status() const;

    // Saving. needsSave() says whether the current content differs from the file on disk.
    bool needsSave() const { return m_annotation.file.needsWrite(); }
    // The current content and the stamp of the file it was read from, without handing it
    // to a save: edits kept in memory while they cannot be written.
    formats::LoadedAnnotation copy() const { return {m_annotation.file.clone(), m_annotation.stamp}; }
    // A copy to hand to the saver. Until its save is acknowledged with savedSnapshot(), the
    // file may change under the session, so going back to the saved entry still needs a save.
    SaveSnapshot snapshot();
    // A save of `snapshot` finished; `saved` is the clone it wrote and `stamp` the file after.
    void savedSnapshot(quint64 entryId, const formats::AnnotationFile& saved, const formats::FileStamp& stamp);
    // "用我的版本覆盖" needs nothing here; "重新加载" replaces the session with a new one.

    // Grows with every change, undo and redo included.
    qint64 version() const { return m_version; }
    // Unique among the sessions of this process; a reload makes a new session.
    quint64 id() const { return m_id; }

private:
    struct Entry {
        AnnotationDocument state;
        quint64 id = 0;
    };

    template <typename Edit>
    bool edit(Edit&& apply);
    void moveTo(qsizetype index);

    quint64 m_id = 0;
    QString m_imagePath;
    formats::LoadedAnnotation m_annotation;
    ProjectConfig m_project;
    bool m_nameCollision = false;
    bool m_blocked = false;
    QList<Entry> m_history;
    qsizetype m_index = 0;
    quint64 m_nextEntryId = 1;
    quint64 m_savedEntryId = 0;  // the entry whose content is on disk; 0 when none
    // Snapshots handed out and not acknowledged. Replaced or failed saves are never
    // acknowledged; the count then stays high, which only costs an extra write.
    int m_unacknowledged = 0;
    qint64 m_version = 0;
};

} // namespace liteanno::application
