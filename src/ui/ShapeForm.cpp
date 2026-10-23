#include "ui/ShapeForm.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QWheelEvent>

#include <algorithm>
#include <limits>

namespace liteanno::ui {

namespace {

// A number field without arrows that the mouse wheel does not change: wheeling over the
// panel must not move shapes.
class NumberField : public QDoubleSpinBox
{
public:
    NumberField()
    {
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        setDecimals(1);
        setLocale(QLocale::c());  // a decimal point, also on systems that write a comma
        setKeyboardTracking(false);  // a value only when Enter is pressed or the field is left
        setFocusPolicy(Qt::StrongFocus);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

void setNumber(QDoubleSpinBox* field, double value, double low, double high, bool keepTyping)
{
    if (keepTyping && field->hasFocus())
        return;
    const QSignalBlocker block(field);
    // The range holds the value, also for a shape already outside the image.
    field->setRange(std::min(low, value), std::max(high, value));
    field->setValue(value);
}

} // namespace

ShapeForm::ShapeForm(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("shapeForm"));
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    const auto caption = [](const QString& text) {
        auto* label = new QLabel(text);
        label->setProperty("role", QStringLiteral("secondary"));
        return label;
    };
    auto* title = new QLabel(QStringLiteral(u"选中形状"));
    title->setProperty("role", QStringLiteral("section"));
    grid->addWidget(title, 0, 0, 1, 3);

    m_label = new QComboBox;
    m_label->setObjectName(QStringLiteral("shapeLabel"));
    m_label->setInsertPolicy(QComboBox::NoInsert);
    m_label->setFocusPolicy(Qt::StrongFocus);
    grid->addWidget(caption(QStringLiteral(u"类别")), 1, 0);
    grid->addWidget(m_label, 1, 1, 1, 2);
    connect(m_label, &QComboBox::activated, this, [this](int row) { commitLabel(m_label->itemText(row), false); });

    m_type = new QLabel;
    m_type->setObjectName(QStringLiteral("shapeType"));
    m_type->setProperty("role", QStringLiteral("field"));
    grid->addWidget(caption(QStringLiteral(u"形状类型")), 2, 0);
    grid->addWidget(m_type, 2, 1, 1, 2);

    m_x = new NumberField;
    m_y = new NumberField;
    m_width = new NumberField;
    m_height = new NumberField;
    m_x->setObjectName(QStringLiteral("shapeX"));
    m_y->setObjectName(QStringLiteral("shapeY"));
    m_width->setObjectName(QStringLiteral("shapeWidth"));
    m_height->setObjectName(QStringLiteral("shapeHeight"));
    grid->addWidget(caption(QStringLiteral(u"位置 (x, y)")), 3, 0);
    grid->addWidget(m_x, 3, 1);
    grid->addWidget(m_y, 3, 2);
    grid->addWidget(caption(QStringLiteral(u"尺寸 (w, h)")), 4, 0);
    grid->addWidget(m_width, 4, 1);
    grid->addWidget(m_height, 4, 2);
    for (QDoubleSpinBox* field : {m_x, m_y, m_width, m_height})
        connect(field, &QDoubleSpinBox::valueChanged, this, [this, field] { numberEdited(field); });

    m_note = caption(QString());
    m_note->setWordWrap(true);
    grid->addWidget(m_note, 5, 0, 1, 3);
    setVisible(false);
}

void ShapeForm::setShape(const Shown& shown)
{
    const bool same = shown.index >= 0 && shown.index == m_shown.index && shown.imagePath == m_shown.imagePath;
    m_shown = shown;
    setVisible(shown.index >= 0);
    if (shown.index < 0)
        return;

    if (!(same && m_label->hasFocus())) {
        const QSignalBlocker block(m_label);
        m_label->clear();
        m_label->setEditable(shown.freeText);
        QStringList items = shown.labels;
        if (!shown.label.isEmpty() && !items.contains(shown.label))
            items.prepend(shown.label);
        m_label->addItems(items);
        m_label->setCurrentIndex(int(items.indexOf(shown.label)));
        if (shown.freeText) {
            m_label->setEditText(shown.label);
            if (m_labelEdit != m_label->lineEdit()) {  // made by setEditable()
                m_labelEdit = m_label->lineEdit();
                connect(m_labelEdit, &QLineEdit::editingFinished, this,
                        [this] { commitLabel(m_label->currentText(), true); });
            }
        }
    }
    m_label->setEnabled(shown.editable);
    m_type->setText(shown.type);

    const double width = shown.imageSize.width();
    const double height = shown.imageSize.height();
    const QRectF& box = shown.box;
    setNumber(m_x, box.x(), 0, width - box.width(), same);
    setNumber(m_y, box.y(), 0, height - box.height(), same);
    setNumber(m_width, box.width(), 1, width - box.x(), same);
    setNumber(m_height, box.height(), 1, height - box.y(), same);
    const bool numbers = shown.editable && shown.rectangle;
    for (QDoubleSpinBox* field : {m_x, m_y, m_width, m_height})
        field->setEnabled(numbers);
    m_note->setText(shown.editable && !shown.rectangle ? QStringLiteral(u"这里显示外接框；只有矩形可以在这里改数字，其他形状请在图上拖动。")
                                                       : QString());
    m_note->setVisible(!m_note->text().isEmpty());
}

void ShapeForm::commitPending()
{
    if (m_shown.index < 0)
        return;
    for (QDoubleSpinBox* field : {m_x, m_y, m_width, m_height}) {
        if (field->hasFocus())
            field->interpretText();  // sends valueChanged when the text is a new value
    }
    if (m_shown.freeText && m_label->hasFocus())
        commitLabel(m_label->currentText(), true);
}

void ShapeForm::numberEdited(QDoubleSpinBox* field)
{
    if (m_shown.index < 0 || !m_shown.editable || !m_shown.rectangle)
        return;
    // Only the field changed counts; the others keep the shape's exact values, not the
    // rounded ones shown.
    // The fields' limits are rounded to the decimal shown, so the image's edges are kept
    // here, against the exact box (one that is already outside stays where it is).
    const QRectF& old = m_shown.box;
    const bool sized = !m_shown.imageSize.isEmpty();
    const double imageWidth = sized ? m_shown.imageSize.width() : std::numeric_limits<double>::max();
    const double imageHeight = sized ? m_shown.imageSize.height() : std::numeric_limits<double>::max();
    const auto limit = [](double value, double low, double high, double current) {
        return std::clamp(value, std::min(low, current), std::max(high, current));
    };
    QRectF box = old;
    if (field == m_x)
        box.moveLeft(limit(m_x->value(), 0, imageWidth - old.width(), old.x()));
    else if (field == m_y)
        box.moveTop(limit(m_y->value(), 0, imageHeight - old.height(), old.y()));
    else if (field == m_width)
        box.setWidth(limit(m_width->value(), 1, imageWidth - old.x(), old.width()));
    else if (field == m_height)
        box.setHeight(limit(m_height->value(), 1, imageHeight - old.y(), old.height()));
    else
        return;
    if (box != m_shown.box)
        emit rectangleChanged(box);
}

void ShapeForm::commitLabel(const QString& text, bool typed)
{
    if (m_shown.index < 0 || !m_shown.editable)
        return;
    // A name from the list is taken as it is; a typed one without spaces at its ends, as in
    // the label dialog.
    const QString label = typed && !m_shown.labels.contains(text) ? text.trimmed() : text;
    if (label.isEmpty()) {
        const QSignalBlocker block(m_label);
        m_label->setEditText(m_shown.label);
        return;
    }
    if (label != m_shown.label)
        emit labelChosen(label);
}

} // namespace liteanno::ui
