#include "ui/Prompts.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace liteanno::ui {

namespace {

QPlainTextEdit* jsonView(const QString& text)
{
    auto* view = new QPlainTextEdit(text);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    view->setFont(font);
    return view;
}

RecoveryChoice askRecovery(QWidget* parent, const application::RecoveryCandidate& candidate, const QByteArray& current)
{
    using application::RecoveryState;
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral(u"有未写入的编辑"));
    auto* layout = new QVBoxLayout(&dialog);

    QString explanation;
    switch (candidate.state) {
    case RecoveryState::Restorable:
        explanation = QStringLiteral(u"图片 %1 有一份上次没有写入标注文件的编辑。标注文件之后没有被改过，可以直接恢复。");
        break;
    case RecoveryState::Conflict:
        explanation = QStringLiteral(u"图片 %1 有一份上次没有写入标注文件的编辑，但标注文件后来被其他程序改过。"
                                     u"请对比两份内容后选择保留哪一份。");
        break;
    default:
        explanation = QStringLiteral(u"图片 %1 有一份上次没有写入标注文件的编辑，但现在读不到标注文件，请稍后再处理。");
        break;
    }
    auto* label = new QLabel(explanation.arg(candidate.record.imagePath)
                             + QStringLiteral(u"\n这份编辑只保存在这台电脑上，不会随 U 盘带到其他电脑。"));
    label->setWordWrap(true);
    layout->addWidget(label);

    auto* views = new QHBoxLayout;
    auto* left = new QVBoxLayout;
    left->addWidget(new QLabel(QStringLiteral(u"未写入的编辑")));
    left->addWidget(jsonView(QString::fromUtf8(candidate.record.annotation)));
    views->addLayout(left);
    if (candidate.state == RecoveryState::Conflict) {
        auto* right = new QVBoxLayout;
        right->addWidget(new QLabel(QStringLiteral(u"现在的标注文件")));
        right->addWidget(jsonView(current.isEmpty() ? QStringLiteral(u"（文件不存在）") : QString::fromUtf8(current)));
        views->addLayout(right);
    }
    layout->addLayout(views);

    auto* buttons = new QDialogButtonBox;
    QPushButton* restore = nullptr;
    if (candidate.state == RecoveryState::Restorable || candidate.state == RecoveryState::Conflict) {
        restore = buttons->addButton(candidate.state == RecoveryState::Conflict ? QStringLiteral(u"用未写入的编辑")
                                                                                : QStringLiteral(u"恢复"),
                                     QDialogButtonBox::AcceptRole);
    }
    QPushButton* discard = buttons->addButton(candidate.state == RecoveryState::Conflict
                                                  ? QStringLiteral(u"保留现在的文件")
                                                  : QStringLiteral(u"丢弃这份编辑"),
                                              QDialogButtonBox::DestructiveRole);
    buttons->addButton(QStringLiteral(u"以后再说"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);

    RecoveryChoice choice = RecoveryChoice::Later;
    QObject::connect(buttons, &QDialogButtonBox::clicked, &dialog, [&](QAbstractButton* button) {
        if (button == restore)
            choice = RecoveryChoice::Restore;
        else if (button == discard)
            choice = RecoveryChoice::Discard;
        dialog.accept();
    });
    dialog.resize(900, 600);
    dialog.exec();
    return choice;
}

} // namespace

Prompts Prompts::dialogs(QWidget* parent)
{
    Prompts prompts;
    prompts.chooseLabel = [parent](const QStringList& classes, const QString& suggested) -> std::optional<QString> {
        bool ok = false;
        QString label;
        if (classes.isEmpty()) {
            label = QInputDialog::getText(parent, QStringLiteral(u"类别"), QStringLiteral(u"类别名称："), QLineEdit::Normal,
                                          suggested, &ok);
        } else {
            label = QInputDialog::getItem(parent, QStringLiteral(u"类别"), QStringLiteral(u"选择类别："), classes,
                                          std::max<qsizetype>(0, classes.indexOf(suggested)), false, &ok);
        }
        label = label.trimmed();
        if (!ok || label.isEmpty())
            return std::nullopt;
        return label;
    };
    prompts.skipReason = [parent]() -> std::optional<QString> {
        bool ok = false;
        const QString reason = QInputDialog::getText(parent, QStringLiteral(u"跳过"), QStringLiteral(u"跳过原因（必填）："),
                                                     QLineEdit::Normal, QString(), &ok)
                                   .trimmed();
        if (!ok || reason.isEmpty())
            return std::nullopt;
        return reason;
    };
    prompts.externalChange = [parent](const QString& imagePath) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral(u"标注文件被其他程序修改"),
                        QStringLiteral(u"图片 %1 的标注文件在打开后被其他程序修改过，你的修改没有写入。\n"
                                       u"“重新加载”会放弃你在这张图上的修改；“用我的版本覆盖”会替换其他程序的修改。")
                            .arg(imagePath),
                        QMessageBox::NoButton, parent);
        QPushButton* reload = box.addButton(QStringLiteral(u"重新加载"), QMessageBox::DestructiveRole);
        QPushButton* overwrite = box.addButton(QStringLiteral(u"用我的版本覆盖"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral(u"稍后处理"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == reload)
            return ExternalChangeChoice::Reload;
        if (box.clickedButton() == overwrite)
            return ExternalChangeChoice::Overwrite;
        return ExternalChangeChoice::Later;
    };
    prompts.recovery = [parent](const application::RecoveryCandidate& candidate, const QByteArray& current) {
        return askRecovery(parent, candidate, current);
    };
    prompts.message = [parent](const QString& title, const QString& text) {
        QMessageBox::warning(parent, title, text);
    };
    prompts.confirmDiscard = [parent](const QString& text) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral(u"修改没有保存"), text, QMessageBox::NoButton, parent);
        QPushButton* stay = box.addButton(QStringLiteral(u"不关闭，回去处理"), QMessageBox::RejectRole);
        box.addButton(QStringLiteral(u"放弃这些修改"), QMessageBox::DestructiveRole);
        box.setDefaultButton(stay);
        box.exec();
        return box.clickedButton() != stay;
    };
    return prompts;
}

} // namespace liteanno::ui
