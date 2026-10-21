#include "application/DocumentSession.h"

#include <utility>

namespace liteanno::application {

DocumentSession::DocumentSession(QString imagePath, formats::LoadedAnnotation annotation,
                                 const ProjectConfig& project, bool nameCollision)
    : m_imagePath(std::move(imagePath))
    , m_annotation(std::move(annotation))
    , m_project(project)
    , m_nameCollision(nameCollision)
{
    const quint64 id = m_nextEntryId++;
    m_history.append({m_annotation.file.document(), id});
    // A new document is not on disk, but writing it unedited is not wanted either.
    m_savedEntryId = id;
}

template <typename Edit>
bool DocumentSession::edit(Edit&& apply)
{
    if (isReadOnly())
        return false;
    if (!apply(m_annotation.file.document()))
        return false;
    m_history.resize(m_index + 1);
    m_history.append({m_annotation.file.document(), m_nextEntryId++});
    m_index = m_history.size() - 1;
    ++m_version;
    return true;
}

qsizetype DocumentSession::addShape(const Shape& shape)
{
    qsizetype index = -1;
    edit([&](AnnotationDocument& document) {
        index = document.addShape(shape);
        return index >= 0;
    });
    return index;
}

bool DocumentSession::setShapePoints(qsizetype index, const PointList& points)
{
    return edit([&](AnnotationDocument& document) { return document.setShapePoints(index, points); });
}

bool DocumentSession::setShapeLabel(qsizetype index, const QString& label)
{
    return edit([&](AnnotationDocument& document) { return document.setShapeLabel(index, label); });
}

bool DocumentSession::removeShape(qsizetype index)
{
    return edit([&](AnnotationDocument& document) { return document.removeShape(index); });
}

bool DocumentSession::setImageLabel(const QString& name, bool value)
{
    return edit([&](AnnotationDocument& document) { return document.setImageLabel(name, value); });
}

bool DocumentSession::selectSingleImageLabel(const QString& name)
{
    return edit([&](AnnotationDocument& document) {
        bool changed = false;
        for (const QString& option : m_project.imageLabels.options)
            changed = document.setImageLabel(option, option == name) || changed;
        return changed;
    });
}

bool DocumentSession::confirm()
{
    if (!confirmBlockers().isEmpty())
        return false;
    return edit([](AnnotationDocument& document) { return document.confirm(); });
}

bool DocumentSession::skip(const QString& reason)
{
    return edit([&](AnnotationDocument& document) { return document.skip(reason); });
}

bool DocumentSession::unskip()
{
    return edit([](AnnotationDocument& document) { return document.unskip(); });
}

void DocumentSession::moveTo(qsizetype index)
{
    m_index = index;
    const Entry& entry = m_history[index];
    m_annotation.file.document().restoreContent(entry.state, entry.id == m_savedEntryId);
    ++m_version;
}

bool DocumentSession::undo()
{
    if (!canUndo() || isReadOnly())
        return false;
    moveTo(m_index - 1);
    return true;
}

bool DocumentSession::redo()
{
    if (!canRedo() || isReadOnly())
        return false;
    moveTo(m_index + 1);
    return true;
}

QList<ConfirmBlocker> DocumentSession::confirmBlockers() const
{
    const bool sizeMismatch = m_annotation.file.readOnlyReason() == formats::ReadOnlyReason::ImageSizeMismatch;
    return liteanno::confirmBlockers(document(), m_project, sizeMismatch);
}

AnnotationStatus DocumentSession::status() const
{
    using formats::ReadOnlyReason;
    const formats::AnnotationFile& file = m_annotation.file;
    StatusFacts facts;
    facts.nameCollision = m_nameCollision;
    facts.jsonError = file.readOnlyReason() == ReadOnlyReason::JsonError
                      || file.readOnlyReason() == ReadOnlyReason::Unreadable;
    facts.orientationPending = file.readOnlyReason() == ReadOnlyReason::ExifOrientationUnconfirmed;
    facts.hasJson = !file.isNew() || needsSave();
    facts.review = document().review();
    facts.hasContent = !document().shapes().isEmpty();
    for (const ImageLabel& label : document().imageLabels())
        facts.hasContent = facts.hasContent || label.value.value_or(false);
    facts.confirmable = confirmBlockers().isEmpty();
    return deriveStatus(facts);
}

SaveSnapshot DocumentSession::snapshot() const
{
    return {m_imagePath, {m_annotation.file.clone(), m_annotation.stamp}, m_history[m_index].id, m_version};
}

void DocumentSession::savedSnapshot(quint64 entryId, const formats::AnnotationFile& saved,
                                    const formats::FileStamp& stamp)
{
    m_annotation.stamp = stamp;
    m_savedEntryId = entryId;
    if (m_history[m_index].id == entryId)
        m_annotation.file.markSaved();
    else
        m_annotation.file.adoptSavedBase(saved);
}

} // namespace liteanno::application
