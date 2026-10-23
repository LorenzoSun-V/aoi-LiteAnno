#include "ui/ListDelegates.h"

#include "ui/Texts.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>

namespace liteanno::ui {

namespace {

const QColor kText(0x1F, 0x29, 0x33);
const QColor kSecondary(0x5B, 0x66, 0x72);
const QColor kSelected(0xE8, 0xF0, 0xFE);
const QColor kHover(0xF3, 0xF5, 0xF8);
const QColor kAccent(0x1F, 0x6F, 0xEB);
const QColor kProblem(0xC0, 0x2A, 0x23);

QColor toneColor(const QString& tone)
{
    if (tone == QLatin1String("ok"))
        return QColor(0x1E, 0x8A, 0x46);
    if (tone == QLatin1String("wait"))
        return QColor(0xC2, 0x7A, 0x00);
    if (tone == QLatin1String("skip"))
        return QColor(0x6E, 0x40, 0xC9);
    if (tone == QLatin1String("problem"))
        return kProblem;
    return QColor(0x9A, 0xA4, 0xAE);
}

// The row background: a rounded tint when selected or under the mouse, and a bar at the
// left edge for the selected row.
QRect paintRowBackground(QPainter* painter, const QStyleOptionViewItem& option, bool bar)
{
    const QRect row = option.rect.adjusted(6, 1, -6, -1);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    if (selected || option.state.testFlag(QStyle::State_MouseOver)) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? kSelected : kHover);
        painter->drawRoundedRect(row, 5, 5);
    }
    if (selected && bar) {
        painter->setBrush(kAccent);
        painter->drawRoundedRect(QRect(option.rect.left(), row.top() + 4, 3, row.height() - 8), 1.5, 1.5);
    }
    return row.adjusted(10, 0, -8, 0);
}

void paintStatusMark(QPainter* painter, const QRectF& box, AnnotationStatus status)
{
    const QString tone = statusTone(status);
    const QColor color = toneColor(tone);
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (tone == QLatin1String("new")) {
        painter->setPen(QPen(color, 1.6));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(box.adjusted(1, 1, -1, -1));
        return;
    }
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawEllipse(box);
    if (tone == QLatin1String("ok")) {
        QPainterPath tick;
        tick.moveTo(box.left() + box.width() * 0.27, box.top() + box.height() * 0.52);
        tick.lineTo(box.left() + box.width() * 0.44, box.top() + box.height() * 0.69);
        tick.lineTo(box.left() + box.width() * 0.74, box.top() + box.height() * 0.35);
        painter->setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(tick);
    } else if (tone == QLatin1String("problem")) {
        painter->setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap));
        const double x = box.center().x();
        painter->drawLine(QPointF(x, box.top() + box.height() * 0.26), QPointF(x, box.top() + box.height() * 0.58));
        painter->drawPoint(QPointF(x, box.top() + box.height() * 0.76));
    }
}

} // namespace

QString statusTone(AnnotationStatus status)
{
    switch (status) {
    case AnnotationStatus::Confirmed:
        return QStringLiteral("ok");
    case AnnotationStatus::PendingConfirmation:
        return QStringLiteral("wait");
    case AnnotationStatus::Unprocessed:
        return QStringLiteral("new");
    case AnnotationStatus::Skipped:
        return QStringLiteral("skip");
    case AnnotationStatus::Missing:
    case AnnotationStatus::NameCollision:
    case AnnotationStatus::Corrupt:
    case AnnotationStatus::OrientationPending:
    case AnnotationStatus::StateConflict:
    case AnnotationStatus::ConfirmedWithProblems:
        return QStringLiteral("problem");
    }
    return QStringLiteral("new");
}

ImageListDelegate::ImageListDelegate(std::function<Row(int)> row, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_row(std::move(row))
{
}

void ImageListDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QRect content = paintRowBackground(painter, option, true);
    const Row row = m_row(index.row());

    QFont small = option.font;
    small.setPointSizeF(option.font.pointSizeF() * 0.92);
    if (row.status || row.unwritten) {
        const QFontMetrics metrics(small);
        int right = content.right();
        painter->setFont(small);
        if (row.status) {
            const QString text = statusText(*row.status);
            const int width = metrics.horizontalAdvance(text);
            const QRect textRect(right - width + 1, content.top(), width, content.height());
            painter->setPen(*row.status == AnnotationStatus::Unprocessed ? kSecondary : toneColor(statusTone(*row.status)));
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignRight, text);
            const int mark = 12;
            paintStatusMark(painter, QRectF(textRect.left() - mark - 6, content.center().y() - mark / 2.0 + 0.5, mark, mark),
                            *row.status);
            right = textRect.left() - mark - 14;
        }
        if (row.unwritten) {
            const QString text = QStringLiteral(u"未写入");
            const int width = metrics.horizontalAdvance(text);
            painter->setPen(kProblem);
            painter->drawText(QRect(right - width + 1, content.top(), width, content.height()),
                              Qt::AlignVCenter | Qt::AlignRight, text);
            right -= width + 8;
        }
        content.setRight(right - 4);
    }
    painter->setFont(option.font);
    painter->setPen(kText);
    painter->drawText(content, Qt::AlignVCenter | Qt::AlignLeft,
                      option.fontMetrics.elidedText(row.name, Qt::ElideMiddle, content.width()));
    painter->restore();
}

QSize ImageListDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& /*index*/) const
{
    return QSize(200, qMax(30, option.fontMetrics.height() + 12));
}

ShapeListDelegate::ShapeListDelegate(std::function<std::optional<Row>(int)> row, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_row(std::move(row))
{
}

void ShapeListDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const std::optional<Row> row = m_row(index.row());
    if (!row) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QRect content = paintRowBackground(painter, option, false);

    const int swatch = 12;
    painter->setPen(Qt::NoPen);
    painter->setBrush(row->color);
    painter->drawRoundedRect(QRectF(content.left(), content.center().y() - swatch / 2.0 + 0.5, swatch, swatch), 3, 3);
    content.setLeft(content.left() + swatch + 10);

    const int eye = 16;
    const QIcon icon = themeIconVariant(row->hidden ? QStringLiteral("eye-off") : QStringLiteral("eye"));
    painter->setOpacity(row->hidden ? 0.9 : 0.55);
    icon.paint(painter, QRect(content.right() - eye + 1, content.center().y() - eye / 2 + 1, eye, eye));
    painter->setOpacity(1);
    content.setRight(content.right() - eye - 10);

    QFont small = option.font;
    small.setPointSizeF(option.font.pointSizeF() * 0.92);
    const int typeWidth = QFontMetrics(small).horizontalAdvance(row->type);
    painter->setFont(small);
    painter->setPen(kSecondary);
    painter->drawText(QRect(content.right() - typeWidth + 1, content.top(), typeWidth, content.height()),
                      Qt::AlignVCenter | Qt::AlignRight, row->type);
    content.setRight(content.right() - typeWidth - 10);

    QFont label = option.font;
    label.setWeight(QFont::DemiBold);
    painter->setFont(label);
    painter->setPen(row->hidden ? kSecondary : kText);
    const QString text = row->label.isEmpty() ? QStringLiteral("-") : row->label;
    painter->drawText(content, Qt::AlignVCenter | Qt::AlignLeft,
                      QFontMetrics(label).elidedText(text, Qt::ElideRight, content.width()));
    painter->restore();
}

QSize ShapeListDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& /*index*/) const
{
    return QSize(200, qMax(32, option.fontMetrics.height() + 14));
}

} // namespace liteanno::ui
