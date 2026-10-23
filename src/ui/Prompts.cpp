#include "ui/Prompts.h"

#include "ui/Texts.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
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

std::optional<ExportChoice> askExportOptions(QWidget* parent, const ExportQuestion& question)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral(u"导出"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* format = new QComboBox;
    for (formats::ExportFormat f : formats::exportFormats()) {
        if (f == formats::ExportFormat::Classification && question.plainFolder)
            continue;  // image-level labels are set up in a project file
        format->addItem(exportFormatText(f), int(f));
    }
    form->addRow(QStringLiteral(u"格式："), format);
    const int batchNumber = question.batchNumber;
    auto* batch = new QRadioButton(batchNumber > 0 ? QStringLiteral(u"当前批次（第 %1 批）").arg(batchNumber)
                                                   : QStringLiteral(u"当前批次"));
    auto* project = new QRadioButton(question.plainFolder ? QStringLiteral(u"整个文件夹") : QStringLiteral(u"整个项目"));
    batch->setEnabled(batchNumber > 0);
    (batchNumber > 0 ? batch : project)->setChecked(true);
    if (!question.plainFolder) {
        auto* scope = new QVBoxLayout;
        scope->addWidget(batch);
        scope->addWidget(project);
        form->addRow(QStringLiteral(u"范围："), scope);
    }

    // Formats with class numbers take them from a classes.txt, as X-AnyLabeling does.
    auto* classes = new QLineEdit(question.classesFile);
    classes->setObjectName(QStringLiteral("classesFile"));
    if (question.projectClasses)
        classes->setPlaceholderText(QStringLiteral(u"不选：用项目文件里的类别编号"));
    auto* browseClasses = new QPushButton(QStringLiteral(u"选择…"));
    QObject::connect(browseClasses, &QPushButton::clicked, &dialog, [&dialog, classes] {
        const QString chosen = QFileDialog::getOpenFileName(&dialog, QStringLiteral(u"选择类别文件"), classes->text(),
                                                            QStringLiteral(u"类别文件 (*.txt);;所有文件 (*)"));
        if (!chosen.isEmpty())
            classes->setText(chosen);
    });
    auto* classesRow = new QHBoxLayout;
    classesRow->addWidget(classes, 1);
    classesRow->addWidget(browseClasses);
    form->addRow(QStringLiteral(u"类别文件："), classesRow);
    const auto needsClasses = [format] {
        return formats::exportNeedsClassIds(formats::ExportFormat(format->currentData().toInt()));
    };
    const auto updateClasses = [=] {
        classes->setEnabled(needsClasses());
        browseClasses->setEnabled(needsClasses());
    };
    QObject::connect(format, &QComboBox::currentIndexChanged, &dialog, updateClasses);
    updateClasses();

    auto* folder = new QLineEdit;
    auto* browse = new QPushButton(QStringLiteral(u"选择…"));
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, folder] {
        const QString chosen = QFileDialog::getExistingDirectory(&dialog, QStringLiteral(u"导出到（选择空文件夹）"));
        if (!chosen.isEmpty())
            folder->setText(chosen);
    });
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(folder, 1);
    folderRow->addWidget(browse);
    form->addRow(QStringLiteral(u"导出到："), folderRow);
    auto* copy = new QCheckBox(QStringLiteral(u"同时复制图片"));
    form->addRow(QString(), copy);
    layout->addLayout(form);
    auto* note = new QLabel(question.plainFolder
                                ? QStringLiteral(u"导出有标注的图片（跳过的图片除外）；YOLO、COCO、掩膜的类别编号按类别文件的行号，第一行是 0。"
                                                 u"导出文件夹必须是空文件夹或新文件夹，并且不能在数据文件夹里。")
                                : QStringLiteral(u"只导出已确认的图片；导出文件夹必须是空文件夹或新文件夹，并且不能在数据文件夹里。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral(u"导出"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, &question, folder, classes, needsClasses] {
        if (needsClasses() && !question.projectClasses && classes->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral(u"导出"),
                                 QStringLiteral(u"这个格式需要类别编号，请选择类别文件（classes.txt，每行一个类别）。"));
            return;
        }
        if (folder->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral(u"导出"), QStringLiteral(u"请选择导出文件夹。"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    ExportChoice choice;
    choice.format = formats::ExportFormat(format->currentData().toInt());
    choice.wholeProject = project->isChecked();
    choice.outputDir = folder->text().trimmed();
    choice.copyImages = copy->isChecked();
    if (needsClasses())
        choice.classesFile = classes->text().trimmed();
    return choice;
}

std::optional<ImportChoice> askImportOptions(QWidget* parent, const ImportQuestion& question)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral(u"导入"));
    dialog.setMinimumWidth(620);  // the hints in the file boxes stay readable
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* format = new QComboBox;
    format->setObjectName(QStringLiteral("importFormat"));
    for (formats::ImportFormat f : formats::importFormats())
        format->addItem(importFormatText(f), int(f));
    form->addRow(QStringLiteral(u"格式："), format);
    const int batchNumber = question.batchNumber;
    auto* batch = new QRadioButton(batchNumber > 0 ? QStringLiteral(u"当前批次（第 %1 批）").arg(batchNumber)
                                                   : QStringLiteral(u"当前批次"));
    auto* project = new QRadioButton(question.plainFolder ? QStringLiteral(u"整个文件夹") : QStringLiteral(u"整个项目"));
    batch->setEnabled(batchNumber > 0);
    (batchNumber > 0 ? batch : project)->setChecked(true);
    if (!question.plainFolder) {
        auto* scope = new QVBoxLayout;
        scope->addWidget(batch);
        scope->addWidget(project);
        form->addRow(QStringLiteral(u"范围："), scope);
    }
    const auto chosenFormat = [format] { return formats::ImportFormat(format->currentData().toInt()); };

    // A file chooser row: the text box and its "选择…" button.
    const auto fileRow = [&dialog, form](const QString& name, const QString& label, const std::function<QString(const QString&)>& choose) {
        auto* edit = new QLineEdit;
        edit->setObjectName(name);
        auto* browse = new QPushButton(QStringLiteral(u"选择…"));
        QObject::connect(browse, &QPushButton::clicked, &dialog, [edit, choose] {
            const QString chosen = choose(edit->text());
            if (!chosen.isEmpty())
                edit->setText(chosen);
        });
        auto* row = new QHBoxLayout;
        row->addWidget(edit, 1);
        row->addWidget(browse);
        form->addRow(label, row);
        return std::pair{edit, browse};
    };
    auto* labelsLabel = new QLabel;
    const auto [labels, browseLabels] = fileRow(QStringLiteral("importLabels"), QString(), [&dialog, chosenFormat](const QString& start) {
        if (formats::importReadsOneFile(chosenFormat()))
            return QFileDialog::getOpenFileName(&dialog, QStringLiteral(u"选择 COCO 标注文件"), start,
                                                QStringLiteral(u"COCO 标注文件 (*.json);;所有文件 (*)"));
        return QFileDialog::getExistingDirectory(&dialog, QStringLiteral(u"选择标注文件夹"), start);
    });
    form->setWidget(form->rowCount() - 1, QFormLayout::LabelRole, labelsLabel);
    const auto [classes, browseClasses] = fileRow(QStringLiteral("importClasses"), QStringLiteral(u"类别文件："), [&dialog](const QString& start) {
        return QFileDialog::getOpenFileName(&dialog, QStringLiteral(u"选择类别文件"), start,
                                            QStringLiteral(u"类别文件 (*.txt);;所有文件 (*)"));
    });
    classes->setPlaceholderText(question.plainFolder ? QStringLiteral(u"不选：用标注文件夹里的 classes.txt")
                                                     : QStringLiteral(u"不选：用标注文件夹里的 classes.txt，没有就用项目的类别"));
    const auto [mapping, browseMapping] = fileRow(QStringLiteral("importMapping"), QStringLiteral(u"颜色对照："), [&dialog](const QString& start) {
        return QFileDialog::getOpenFileName(&dialog, QStringLiteral(u"选择掩膜颜色对照文件"), start,
                                            QStringLiteral(u"颜色对照文件 (*.json);;所有文件 (*)"));
    });
    mapping->setPlaceholderText(QStringLiteral(u"不选：用掩膜文件夹或上一级里的 mask_colors.json"));

    auto* onlyNew = new QRadioButton(QStringLiteral(u"只导入还没有标注的图片"));
    auto* append = new QRadioButton(QStringLiteral(u"已有标注的图片：追加在原有形状后面"));
    auto* replace = new QRadioButton(QStringLiteral(u"已有标注的图片：替换原有形状"));
    onlyNew->setChecked(true);
    auto* modes = new QVBoxLayout;
    modes->addWidget(onlyNew);
    modes->addWidget(append);
    modes->addWidget(replace);
    form->addRow(QStringLiteral(u"已有标注："), modes);
    layout->addLayout(form);

    const auto update = [=] {
        const formats::ImportFormat f = chosenFormat();
        labelsLabel->setText(formats::importReadsOneFile(f) ? QStringLiteral(u"标注文件：") : QStringLiteral(u"标注文件夹："));
        classes->setEnabled(formats::importNeedsClassNames(f));
        browseClasses->setEnabled(formats::importNeedsClassNames(f));
        mapping->setEnabled(f == formats::ImportFormat::Mask);
        browseMapping->setEnabled(f == formats::ImportFormat::Mask);
    };
    QObject::connect(format, &QComboBox::currentIndexChanged, &dialog, update);
    update();

    auto* note = new QLabel(QStringLiteral(u"标注文件按图片的相对路径配对，找不到时按文件名在标注文件夹第一层找。"
                                           u"导入的图片都是“待确认”，请逐张检查后确认。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral(u"导入"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, labels, chosenFormat] {
        if (labels->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral(u"导入"),
                                 formats::importReadsOneFile(chosenFormat()) ? QStringLiteral(u"请选择 COCO 标注文件。")
                                                                             : QStringLiteral(u"请选择标注文件夹。"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    ImportChoice choice;
    choice.format = chosenFormat();
    choice.wholeProject = project->isChecked();
    choice.labels = labels->text().trimmed();
    if (formats::importNeedsClassNames(choice.format))
        choice.classesFile = classes->text().trimmed();
    if (choice.format == formats::ImportFormat::Mask)
        choice.mappingFile = mapping->text().trimmed();
    choice.mode = append->isChecked()    ? application::ImportMode::Append
                  : replace->isChecked() ? application::ImportMode::Replace
                                         : application::ImportMode::OnlyNew;
    return choice;
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
    prompts.chooseLabel = [parent](const QStringList& classes, const QString& suggested,
                                   bool freeText) -> std::optional<QString> {
        bool ok = false;
        QString label;
        if (classes.isEmpty()) {
            label = QInputDialog::getText(parent, QStringLiteral(u"类别"), QStringLiteral(u"类别名称："), QLineEdit::Normal,
                                          suggested, &ok);
        } else if (freeText) {
            // The names used so far are offered; a new one is typed in.
            QStringList offered = classes;
            if (!suggested.isEmpty() && !offered.contains(suggested))
                offered.prepend(suggested);
            label = QInputDialog::getItem(parent, QStringLiteral(u"类别"), QStringLiteral(u"类别名称（可选择或输入）："), offered,
                                          std::max<qsizetype>(0, offered.indexOf(suggested)), true, &ok);
        } else {
            label = QInputDialog::getItem(parent, QStringLiteral(u"类别"), QStringLiteral(u"选择类别："), classes,
                                          std::max<qsizetype>(0, classes.indexOf(suggested)), false, &ok);
        }
        // A name chosen from the list stays as it is written there (a classes.txt keeps its
        // spaces); only what is typed loses the spaces around it.
        if (label != suggested && !classes.contains(label))
            label = label.trimmed();
        if (!ok || label.trimmed().isEmpty())
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
    prompts.exportOptions = [parent](const ExportQuestion& question) { return askExportOptions(parent, question); };
    prompts.importOptions = [parent](const ImportQuestion& question) { return askImportOptions(parent, question); };
    prompts.skipUnconvertible = [parent](const QString& text) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral(u"有无法转换的对象"), text, QMessageBox::NoButton, parent);
        QPushButton* stop = box.addButton(QStringLiteral(u"不导出"), QMessageBox::RejectRole);
        box.addButton(QStringLiteral(u"跳过这些图片继续导出"), QMessageBox::AcceptRole);
        box.setDefaultButton(stop);
        box.exec();
        return box.clickedButton() != stop;
    };
    prompts.setAsideUnusable = [parent](const QString& text) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral(u"无法使用的恢复记录"), text, QMessageBox::NoButton, parent);
        QPushButton* keep = box.addButton(QStringLiteral(u"先不动"), QMessageBox::RejectRole);
        box.addButton(QStringLiteral(u"移到 unusable 文件夹"), QMessageBox::AcceptRole);
        box.setDefaultButton(keep);
        box.exec();
        return box.clickedButton() != keep;
    };
    prompts.chooseBatch = [parent](const QStringList& batches, int current) -> std::optional<int> {
        bool ok = false;
        const QString chosen = QInputDialog::getItem(parent, QStringLiteral(u"选择批次"), QStringLiteral(u"批次："), batches,
                                                     std::max(0, current), false, &ok);
        if (!ok)
            return std::nullopt;
        const qsizetype index = batches.indexOf(chosen);
        if (index < 0)
            return std::nullopt;
        return int(index);
    };
    return prompts;
}

} // namespace liteanno::ui
