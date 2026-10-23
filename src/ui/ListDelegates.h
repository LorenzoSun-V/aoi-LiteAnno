#pragma once

#include "core/AnnotationStatus.h"

#include <QColor>
#include <QStyledItemDelegate>

#include <functional>
#include <optional>

namespace liteanno::ui {

// The theme's colour group of a status: "ok", "wait", "new", "skip" or "problem".
QString statusTone(AnnotationStatus status);

// Draws a row of the image list: the name, then the status as a coloured mark and text.
// The rows come from the window, so the item texts stay as they are (and cost nothing more
// at 200 000 images).
class ImageListDelegate : public QStyledItemDelegate
{
public:
    struct Row {
        QString name;
        std::optional<AnnotationStatus> status;
        bool unwritten = false;
    };
    explicit ImageListDelegate(std::function<Row(int)> row, QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    std::function<Row(int)> m_row;
};

// Draws a row of the shape list: the class colour, the label, the shape type and whether
// the shape is hidden.
class ShapeListDelegate : public QStyledItemDelegate
{
public:
    struct Row {
        QString label;
        QString type;
        QColor color;
        bool hidden = false;
    };
    explicit ShapeListDelegate(std::function<std::optional<Row>(int)> row, QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    std::function<std::optional<Row>(int)> m_row;
};

} // namespace liteanno::ui
