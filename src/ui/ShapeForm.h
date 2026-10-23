#pragma once

#include <QRectF>
#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

namespace liteanno::ui {

// "选中形状": the class, the type and the position and size of the selected shape. Only a
// rectangle's numbers can be changed here; for other shapes they show its bounding box.
class ShapeForm : public QWidget
{
    Q_OBJECT

public:
    struct Shown {
        qsizetype index = -1;  // the shape's row; -1 hides the form
        QString imagePath;  // with `index`, which shape it is
        QString label;
        std::optional<qint64> groupId;
        QString type;
        QRectF box;  // in image pixels
        bool editable = false;  // editable at all (label)
        bool rectangle = false;  // the numbers can be changed
        bool keypoint = false;  // a point: "遮挡" is shown
        bool difficult = false;
        QSize imageSize;
        QStringList labels;  // offered in the list
        bool freeText = false;  // any name may be typed (no class list)
    };

    explicit ShapeForm(QWidget* parent = nullptr);

    // Shows `shown`. A field being typed in keeps its text while the same shape is shown.
    void setShape(const Shown& shown);
    const Shown& shown() const { return m_shown; }
    // Takes what is typed in the field being edited, as Enter would. The window calls it
    // before it leaves the image or the shape by a way that does not move the focus (the
    // buttons under the canvas, keys, menus), while the shape shown is still the one edited.
    void commitPending();

signals:
    void labelChosen(const QString& label);
    // A rectangle's new position and size (x, y, width, height) in image pixels.
    void rectangleChanged(const QRectF& box);
    // "组号" (group_id): joins a pose's rectangle and keypoints. Empty is no group.
    void groupChosen(std::optional<qint64> groupId);
    // "遮挡" on a keypoint ("difficult"): pose export writes v=1 instead of 2.
    void occludedChosen(bool occluded);

private:
    void numberEdited(QDoubleSpinBox* field);
    void commitLabel(const QString& text, bool typed);
    void commitGroup();

    Shown m_shown;
    QComboBox* m_label = nullptr;
    QLineEdit* m_labelEdit = nullptr;  // the one connected
    QLabel* m_type = nullptr;
    QLineEdit* m_group = nullptr;
    QDoubleSpinBox* m_x = nullptr;
    QDoubleSpinBox* m_y = nullptr;
    QDoubleSpinBox* m_width = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QCheckBox* m_occluded = nullptr;
    QLabel* m_note = nullptr;
};

} // namespace liteanno::ui
