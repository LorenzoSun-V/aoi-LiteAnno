#pragma once

#include <QLabel>
#include <QPainter>

namespace liteanno::ui {

// One line of text that ends in "…" where it does not fit (QLabel only cuts it off).
class ElidedLabel : public QLabel
{
public:
    using QLabel::QLabel;

    QSize minimumSizeHint() const override { return QSize(0, QLabel::minimumSizeHint().height()); }

protected:
    void paintEvent(QPaintEvent* /*event*/) override
    {
        QPainter painter(this);
        const QRect area = contentsRect();
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(area, int(alignment()) | Qt::TextSingleLine,
                         fontMetrics().elidedText(text(), Qt::ElideMiddle, area.width()));
    }
};

} // namespace liteanno::ui
