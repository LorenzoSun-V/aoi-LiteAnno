#include "ui/MainWindow.h"

#include "application/RecoveryService.h"
#include "imaging/ImageLoader.h"
#include "ui/Texts.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

namespace liteanno::ui {

using application::BackgroundSaver;
using application::DocumentSession;
using application::ProjectSession;
using application::RecoveryCandidate;
using application::RecoveryService;
using application::RecoveryState;
using formats::SaveStatus;

namespace {

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString openErrorText(const ProjectSession::OpenResult& result, const QString& root)
{
    switch (result.error) {
    case ProjectSession::OpenError::None:
        return QString();
    case ProjectSession::OpenError::NoProjectFile:
        return QStringLiteral(u"文件夹 %1 中没有项目文件 liteanno/project.json。").arg(root);
    case ProjectSession::OpenError::InvalidProjectFile:
        return QStringLiteral(u"项目文件有错误，不能打开。\n出错的字段：%1\n原因：%2")
            .arg(result.field.isEmpty() ? QStringLiteral(u"（整个文件）") : result.field, result.message);
    case ProjectSession::OpenError::LocationUnknown:
        return QStringLiteral(u"无法识别项目所在的磁盘：%1").arg(result.message);
    case ProjectSession::OpenError::Locked:
        return QStringLiteral(u"这个项目已经在另一个窗口或命令行中打开（进程 %1，%2）。请先关闭那边再打开。")
            .arg(result.holder.pid)
            .arg(result.holder.hostName);
    case ProjectSession::OpenError::LockFailed:
        return QStringLiteral(u"无法创建项目锁文件：%1").arg(result.message);
    }
    return QString();
}

} // namespace

MainWindow::MainWindow(Options options, QWidget* parent)
    : QMainWindow(parent)
    , m_options(std::move(options))
    , m_prompts(Prompts::dialogs(this))
{
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(m_options.idleSaveMs);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] { commitCurrent(); });
    buildUi();
    refresh();
}

MainWindow::~MainWindow()
{
    closeProject();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("aoi-LiteAnno"));
    resize(1280, 800);

    m_canvas = new Canvas(this);
    m_canvas->setObjectName(QStringLiteral("canvas"));
    setCentralWidget(m_canvas);
    connect(m_canvas, &Canvas::shapeDrawn, this, &MainWindow::addDrawnShape);
    connect(m_canvas, &Canvas::shapePointsChanged, this, [this](qsizetype index, const PointList& points) {
        if (m_session && m_session->setShapePoints(index, points))
            edited();
        else
            refresh();
    });
    connect(m_canvas, &Canvas::selectionChanged, this, [this](qsizetype index) {
        m_shapeList->setCurrentRow(int(index));
        refresh();
    });
    connect(m_canvas, &Canvas::labelEditRequested, this, &MainWindow::editLabel);
    connect(m_canvas, &Canvas::deleteRequested, this, [this] { deleteSelectedShape(); });

    // Images.
    m_imageList = new QListWidget;
    m_imageList->setObjectName(QStringLiteral("imageList"));
    connect(m_imageList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row != m_current)
            goToImage(row);
    });
    auto* imageDock = new QDockWidget(QStringLiteral(u"图片"), this);
    imageDock->setObjectName(QStringLiteral("imageDock"));
    imageDock->setWidget(m_imageList);
    imageDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, imageDock);

    // Current image.
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    m_imageStatus = new QLabel;
    m_imageStatus->setObjectName(QStringLiteral("imageStatus"));
    QFont bold = m_imageStatus->font();
    bold.setBold(true);
    m_imageStatus->setFont(bold);
    layout->addWidget(m_imageStatus);
    m_readOnlyNote = new QLabel;
    m_readOnlyNote->setWordWrap(true);
    m_readOnlyNote->setStyleSheet(QStringLiteral("color: #c0392b;"));
    layout->addWidget(m_readOnlyNote);
    m_correctSizeButton = new QPushButton(QStringLiteral(u"按当前图片尺寸更正"));
    connect(m_correctSizeButton, &QPushButton::clicked, this, [this] {
        if (m_session && m_session->correctImageSize())
            edited();
    });
    layout->addWidget(m_correctSizeButton);

    layout->addWidget(new QLabel(QStringLiteral(u"形状")));
    m_shapeList = new QListWidget;
    m_shapeList->setObjectName(QStringLiteral("shapeList"));
    connect(m_shapeList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_canvas->setSelectedShape(row);
        m_deleteButton->setEnabled(m_session && !m_session->isReadOnly() && row >= 0);
    });
    connect(m_shapeList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) { editLabel(m_shapeList->row(item)); });
    layout->addWidget(m_shapeList, 1);
    m_deleteButton = new QPushButton(QStringLiteral(u"删除形状"));
    connect(m_deleteButton, &QPushButton::clicked, this, [this] { deleteSelectedShape(); });
    layout->addWidget(m_deleteButton);

    m_imageLabelPanel = new QGroupBox(QStringLiteral(u"图片级标签"));
    m_imageLabelLayout = new QVBoxLayout(m_imageLabelPanel);
    layout->addWidget(m_imageLabelPanel);

    m_blockers = new QLabel;
    m_blockers->setObjectName(QStringLiteral("blockers"));
    m_blockers->setWordWrap(true);
    m_blockers->setStyleSheet(QStringLiteral("color: #b9770e;"));
    layout->addWidget(m_blockers);
    m_confirmButton = new QPushButton(QStringLiteral(u"确认"));
    m_confirmButton->setObjectName(QStringLiteral("confirmButton"));
    connect(m_confirmButton, &QPushButton::clicked, this, [this] { confirmCurrent(); });
    layout->addWidget(m_confirmButton);
    m_skipButton = new QPushButton(QStringLiteral(u"跳过…"));
    connect(m_skipButton, &QPushButton::clicked, this, [this] { skipCurrent(); });
    layout->addWidget(m_skipButton);
    m_unskipButton = new QPushButton(QStringLiteral(u"取消跳过"));
    connect(m_unskipButton, &QPushButton::clicked, this, [this] { unskipCurrent(); });
    layout->addWidget(m_unskipButton);

    auto* panelDock = new QDockWidget(QStringLiteral(u"当前图片"), this);
    panelDock->setObjectName(QStringLiteral("panelDock"));
    panelDock->setWidget(panel);
    panelDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, panelDock);

    m_saveState = new QLabel;
    statusBar()->addPermanentWidget(m_saveState);

    // Menus and tools.
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral(u"文件(&F)"));
    QAction* open = fileMenu->addAction(QStringLiteral(u"打开项目…"), this, &MainWindow::chooseProject);
    open->setShortcut(QKeySequence::Open);
    QAction* save = fileMenu->addAction(QStringLiteral(u"立即保存"), this, &MainWindow::saveNow);
    save->setShortcut(QKeySequence::Save);
    fileMenu->addAction(QStringLiteral(u"处理未写入的编辑…"), this, [this] {
        if (m_project) {
            flushSaves();
            handleRecoveryRecords();
            reloadCurrent();
        }
    });
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral(u"退出"), this, &QWidget::close);

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral(u"编辑(&E)"));
    m_undoAction = editMenu->addAction(QStringLiteral(u"撤销"), this, [this] { undo(); });
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = editMenu->addAction(QStringLiteral(u"重做"), this, [this] { redo(); });
    m_redoAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});

    QToolBar* tools = addToolBar(QStringLiteral(u"工具"));
    tools->setObjectName(QStringLiteral("tools"));
    auto* toolGroup = new QActionGroup(this);
    m_selectTool = tools->addAction(QStringLiteral(u"选择 (V)"));
    m_selectTool->setShortcut(Qt::Key_V);
    m_rectangleTool = tools->addAction(QStringLiteral(u"矩形 (R)"));
    m_rectangleTool->setShortcut(Qt::Key_R);
    m_polygonTool = tools->addAction(QStringLiteral(u"多边形 (P)"));
    m_polygonTool->setShortcut(Qt::Key_P);
    for (QAction* action : {m_selectTool, m_rectangleTool, m_polygonTool}) {
        action->setCheckable(true);
        toolGroup->addAction(action);
    }
    m_selectTool->setChecked(true);
    connect(m_selectTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Select); });
    connect(m_rectangleTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Rectangle); });
    connect(m_polygonTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Polygon); });
    tools->addSeparator();
    QAction* previous = tools->addAction(QStringLiteral(u"上一张 (A)"), this, [this] { previousImage(); });
    previous->setShortcut(Qt::Key_A);
    QAction* next = tools->addAction(QStringLiteral(u"下一张 (D)"), this, [this] { nextImage(); });
    next->setShortcut(Qt::Key_D);
    tools->addAction(QStringLiteral(u"适合窗口"), m_canvas, &Canvas::fitToWindow);
}

void MainWindow::buildImageLabelPanel()
{
    qDeleteAll(m_imageLabelButtons);
    m_imageLabelButtons.clear();
    const bool hasOptions = m_project && !m_project->config().imageLabels.options.isEmpty();
    m_imageLabelPanel->setVisible(hasOptions);
    if (!hasOptions)
        return;
    const ImageLabelConfig& config = m_project->config().imageLabels;
    m_imageLabelPanel->setTitle(config.mode == ImageLabelMode::Single ? QStringLiteral(u"图片级标签（单选）")
                                                                      : QStringLiteral(u"图片级标签（多选）"));
    for (const QString& option : config.options) {
        QAbstractButton* button = config.mode == ImageLabelMode::Single
                                      ? static_cast<QAbstractButton*>(new QRadioButton(option))
                                      : static_cast<QAbstractButton*>(new QCheckBox(option));
        button->setAutoExclusive(false);
        button->setObjectName(QStringLiteral("imageLabel:") + option);
        connect(button, &QAbstractButton::clicked, this, [this, option, single = config.mode == ImageLabelMode::Single](bool checked) {
            if (!m_session)
                return;
            const bool changed = single ? m_session->selectSingleImageLabel(option)
                                        : m_session->setImageLabel(option, checked);
            if (changed)
                edited();
            else
                refresh();
        });
        m_imageLabelLayout->addWidget(button);
        m_imageLabelButtons.append(button);
    }
}

void MainWindow::chooseProject()
{
    const QString root = QFileDialog::getExistingDirectory(this, QStringLiteral(u"打开项目（选择数据根目录）"));
    if (!root.isEmpty())
        openProject(root);
}

bool MainWindow::openProject(const QString& root)
{
    closeProject();
    ProjectSession::OpenResult result = ProjectSession::open(root, m_options.localData);
    if (!result.session) {
        m_prompts.message(QStringLiteral(u"无法打开项目"), openErrorText(result, root));
        return false;
    }
    m_project = std::move(result.session);
    application::SaveCoordinator coordinator(m_project->paths(), m_project->location());
    coordinator.stageHook = m_options.saveStageHook;
    m_saver = std::make_unique<BackgroundSaver>(coordinator, m_options.saveLimits);
    connect(m_saver.get(), &BackgroundSaver::resultsReady, this, &MainWindow::processSaveResults,
            Qt::QueuedConnection);

    QHash<QString, QColor> colors;
    for (const ClassDef& c : m_project->config().classes)
        colors.insert(c.name, QColor(c.color));
    m_canvas->setClassColors(colors);
    const QList<ShapeType>& types = m_project->config().shapeTypes;
    m_rectangleTool->setEnabled(types.contains(ShapeType::Rectangle));
    m_polygonTool->setEnabled(types.contains(ShapeType::Polygon));
    m_selectTool->setChecked(true);
    m_canvas->setTool(Canvas::Tool::Select);
    buildImageLabelPanel();

    handleRecoveryRecords();

    const dataset::ScanResult scan = dataset::scanDirectory(m_project->rootPath());
    m_images = scan.images;
    m_statuses.clear();
    m_imageList->blockSignals(true);
    m_imageList->clear();
    for (const dataset::ScannedImage& image : std::as_const(m_images))
        m_imageList->addItem(image.path);
    m_imageList->blockSignals(false);
    const QString name = m_project->config().name.isEmpty() ? m_project->rootPath() : m_project->config().name;
    setWindowTitle(QStringLiteral("%1 - aoi-LiteAnno").arg(name));
    if (!scan.unsupported.isEmpty()) {
        statusBar()->showMessage(QStringLiteral(u"有 %1 个文件是不支持的图片格式（例如 %2），请转换为 JPG 或 PNG。")
                                     .arg(scan.unsupported.size())
                                     .arg(scan.unsupported.first()),
                                 15000);
    }
    if (!m_images.isEmpty())
        goToImage(0);
    else
        refresh();
    return true;
}

void MainWindow::closeProject()
{
    if (!m_project)
        return;
    m_idleTimer.stop();
    commitCurrent();
    flushSaves();
    m_session.reset();
    m_saver.reset();
    m_project.reset();
    m_images.clear();
    m_statuses.clear();
    m_unwritten.clear();
    m_current = -1;
    m_imageList->clear();
    m_canvas->setDocument(nullptr, true);
    m_canvas->setImage(QImage());
    setWindowTitle(QStringLiteral("aoi-LiteAnno"));
    refresh();
}

void MainWindow::handleRecoveryRecords(const QString& onlyImage)
{
    const RecoveryService recovery(m_project->paths(), m_project->location());
    application::RecoveryScan scan = recovery.scan();
    recovery.removeAlreadySaved(scan);
    for (const RecoveryCandidate& candidate : std::as_const(scan.candidates)) {
        if (candidate.state == RecoveryState::AlreadySaved)
            continue;
        if (!onlyImage.isEmpty() && candidate.record.imagePath != onlyImage)
            continue;
        const RecoveryChoice choice = m_prompts.recovery(candidate, readFile(candidate.annotationPath));
        application::RecordResult result;
        if (choice == RecoveryChoice::Restore)
            result = recovery.restore(candidate, candidate.state == RecoveryState::Conflict);
        else if (choice == RecoveryChoice::Discard)
            result = recovery.discard(candidate);
        else
            continue;
        if (result.status == application::RecordStatus::Done) {
            m_unwritten.remove(candidate.record.imagePath);
        } else {
            m_prompts.message(QStringLiteral(u"未完成"),
                              QStringLiteral(u"图片 %1 的未写入编辑没有处理完：%2")
                                  .arg(candidate.record.imagePath, result.error));
        }
    }
    if (onlyImage.isEmpty() && !scan.unusable.isEmpty()) {
        statusBar()->showMessage(QStringLiteral(u"有 %1 份本机恢复记录无法使用，已保留。").arg(scan.unusable.size()), 15000);
    }
}

bool MainWindow::goToImage(int index)
{
    if (!m_project || index < 0 || index >= m_images.size())
        return false;
    if (index == m_current && m_session) {
        refresh();
        return true;
    }
    m_idleTimer.stop();
    commitCurrent();
    // Architecture 8.2: at the limit, wait for saves before showing the next image.
    if (m_saver->atLimit())
        waitForSaver([this](int ms) { return m_saver->waitUntilBelowLimit(ms); });
    const QString path = m_images[index].path;
    // A save of that image may still be running; its file is read after it.
    if (m_saver->isPending(path))
        waitForSaver([this, path](int ms) { return m_saver->waitUntilIdle(path, ms); });
    processSaveResults();

    m_session.reset();
    m_current = index;
    if (m_unwritten.contains(path))
        handleRecoveryRecords(path);
    loadCurrentImage();
    m_imageList->blockSignals(true);
    m_imageList->setCurrentRow(index);
    m_imageList->blockSignals(false);
    return true;
}

bool MainWindow::nextImage()
{
    return goToImage(m_current + 1);
}

bool MainWindow::previousImage()
{
    return goToImage(m_current - 1);
}

void MainWindow::loadCurrentImage()
{
    m_saveError.clear();
    m_submittedVersion = -1;
    m_externalChangeDeferred = false;
    const dataset::ScannedImage& scanned = m_images[m_current];
    const imaging::LoadedImage image = imaging::loadImage(m_project->imageFilePath(scanned.path));
    m_canvas->setImage(image.image);
    if (image.image.isNull()) {
        m_session.reset();
        m_canvas->setDocument(nullptr, true);
        statusBar()->showMessage(QStringLiteral(u"图片无法打开：%1").arg(image.error), 15000);
        refresh();
        return;
    }
    if (image.hasMoreFrames)
        statusBar()->showMessage(QStringLiteral(u"这张图片有多页或多帧，只显示第一页。"), 10000);
    formats::LoadedAnnotation loaded = formats::loadAnnotationFile(
        m_project->annotationFilePath(scanned.path), image.info, m_project->config().imageLabels.options);
    m_session = std::make_unique<DocumentSession>(scanned.path, std::move(loaded), m_project->config(),
                                                  scanned.nameCollision);
    m_canvas->setDocument(&m_session->document(), m_session->isReadOnly());
    refresh();
}

void MainWindow::reloadCurrent()
{
    if (m_current < 0)
        return;
    m_session.reset();
    loadCurrentImage();
}

void MainWindow::commitCurrent(const formats::SaveOptions& options, bool force)
{
    if (!m_session || !m_saver || m_session->isReadOnly() || !m_session->needsSave())
        return;
    if (!force && m_session->version() == m_submittedVersion)
        return;
    m_submittedVersion = m_session->version();
    m_saver->submit(m_session->snapshot(), options);
    refresh();
}

void MainWindow::saveNow()
{
    m_idleTimer.stop();
    // An explicit save asks again about an external change answered with "以后再说".
    const bool retry = std::exchange(m_externalChangeDeferred, false);
    commitCurrent({}, retry);
}

bool MainWindow::waitForSaver(const std::function<bool(int)>& wait)
{
    ++m_savingWaits;
    m_saveState->setText(QStringLiteral(u"正在保存…"));
    // Results are handled after the wait, not in the middle of switching images.
    const bool handling = std::exchange(m_handlingResults, true);
    while (!wait(50))
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    m_handlingResults = handling;
    return true;
}

void MainWindow::flushSaves()
{
    if (!m_saver)
        return;
    commitCurrent();
    // Answering a prompt can hand over another save ("用我的版本覆盖").
    do {
        if (m_saver->pendingCount() > 0)
            waitForSaver([this](int ms) { return m_saver->waitUntilAllDone(ms); });
        processSaveResults();
    } while (m_saver->pendingCount() > 0);
}

void MainWindow::processSaveResults()
{
    if (!m_saver || m_handlingResults)
        return;
    m_handlingResults = true;
    for (application::SaveJobResult& result : m_saver->takeResults()) {
        const SaveStatus status = result.outcome.result.status;
        const bool current = m_session && m_session->imagePath() == result.imagePath;
        if (status == SaveStatus::Saved || status == SaveStatus::NotModified) {
            m_unwritten.remove(result.imagePath);
            if (current && status == SaveStatus::Saved) {
                m_session->savedSnapshot(result.entryId, *result.saved, result.stamp);
                m_saveError.clear();
                m_externalChangeDeferred = false;
            }
            continue;
        }
        // The next commit hands the edits over again.
        if (current)
            m_submittedVersion = -1;
        if (current && status == SaveStatus::ExternallyModified) {
            m_unwritten.insert(result.imagePath);
            m_saveError = QStringLiteral(u"标注文件被其他程序修改，修改未写入");
            if (m_externalChangeDeferred)
                continue;
            switch (m_prompts.externalChange(result.imagePath)) {
            case ExternalChangeChoice::Reload:
                m_saver->coordinator().discardRecoveryRecord(result.imagePath);
                m_unwritten.remove(result.imagePath);
                reloadCurrent();
                break;
            case ExternalChangeChoice::Overwrite: {
                formats::SaveOptions overwrite;
                overwrite.overwriteExternalChanges = true;
                commitCurrent(overwrite, true);
                break;
            }
            case ExternalChangeChoice::Later:
                m_externalChangeDeferred = true;
                break;
            }
            continue;
        }
        if (current) {
            m_saveError = result.outcome.result.error;
            if (result.outcome.recoveryRecordKept)
                m_unwritten.insert(result.imagePath);
            continue;
        }
        if (result.outcome.recoveryRecordKept)
            m_unwritten.insert(result.imagePath);
        m_prompts.message(QStringLiteral(u"修改没有写入"),
                          QStringLiteral(u"图片 %1 的修改没有写入标注文件（%2）。\n修改已保存在这台电脑上，"
                                         u"再次打开这张图片时会提示处理。")
                              .arg(result.imagePath,
                                   status == SaveStatus::ExternallyModified
                                       ? QStringLiteral(u"文件被其他程序修改过")
                                       : result.outcome.result.error));
    }
    m_handlingResults = false;
    refresh();
}

void MainWindow::edited()
{
    m_idleTimer.start();
    if (m_session && m_current >= 0)
        m_statuses.insert(m_session->imagePath(), m_session->status());
    refresh();
}

QStringList MainWindow::classNames() const
{
    QStringList names;
    if (m_project) {
        for (const ClassDef& c : m_project->config().classes)
            names.append(c.name);
    }
    return names;
}

void MainWindow::addDrawnShape(const Shape& drawn)
{
    if (!m_session || m_session->isReadOnly())
        return;
    const std::optional<QString> label = m_prompts.chooseLabel(classNames(), m_lastLabel);
    if (!label) {
        refresh();
        return;
    }
    Shape shape = drawn;
    shape.label = *label;
    m_lastLabel = *label;
    const qsizetype index = m_session->addShape(shape);
    if (index >= 0) {
        m_canvas->setSelectedShape(index);
        edited();
    }
}

void MainWindow::editLabel(qsizetype index)
{
    if (!m_session || m_session->isReadOnly() || index < 0 || index >= m_session->document().shapes().size())
        return;
    const std::optional<QString> label =
        m_prompts.chooseLabel(classNames(), m_session->document().shapes()[index].label);
    if (label && m_session->setShapeLabel(index, *label)) {
        m_lastLabel = *label;
        edited();
    }
}

bool MainWindow::deleteSelectedShape()
{
    if (!m_session)
        return false;
    const qsizetype index = m_canvas->selectedShape();
    if (index < 0 || !m_session->removeShape(index))
        return false;
    m_canvas->setSelectedShape(-1);
    edited();
    return true;
}

bool MainWindow::confirmCurrent()
{
    if (!m_session || !m_session->confirm())
        return false;
    edited();
    return true;
}

bool MainWindow::skipCurrent()
{
    if (!m_session || m_session->isReadOnly())
        return false;
    const std::optional<QString> reason = m_prompts.skipReason();
    if (!reason || !m_session->skip(*reason))
        return false;
    edited();
    return true;
}

bool MainWindow::unskipCurrent()
{
    if (!m_session || !m_session->unskip())
        return false;
    edited();
    return true;
}

bool MainWindow::undo()
{
    if (!m_session || !m_session->undo())
        return false;
    m_canvas->setSelectedShape(-1);
    edited();
    return true;
}

bool MainWindow::redo()
{
    if (!m_session || !m_session->redo())
        return false;
    m_canvas->setSelectedShape(-1);
    edited();
    return true;
}

QString MainWindow::saveStateText() const
{
    if (!m_session)
        return QString();
    if (m_saver && m_saver->isPending(m_session->imagePath()))
        return QStringLiteral(u"保存中…");
    if (!m_saveError.isEmpty())
        return QStringLiteral(u"保存失败：") + m_saveError;
    if (m_session->needsSave())
        return QStringLiteral(u"有未保存的修改");
    return QStringLiteral(u"已保存");
}

void MainWindow::refreshImageItem(int index)
{
    if (index < 0 || index >= m_imageList->count())
        return;
    const QString path = m_images[index].path;
    QString text = path;
    const auto status = m_statuses.constFind(path);
    if (status != m_statuses.cend())
        text += QStringLiteral(u"  [%1]").arg(statusText(*status));
    if (m_unwritten.contains(path))
        text += QStringLiteral(u"  [未写入]");
    m_imageList->item(index)->setText(text);
}

void MainWindow::refresh()
{
    const bool hasSession = m_session != nullptr;
    const bool editable = hasSession && !m_session->isReadOnly();

    if (hasSession) {
        m_canvas->setDocument(&m_session->document(), m_session->isReadOnly());
        m_statuses.insert(m_session->imagePath(), m_session->status());
        refreshImageItem(m_current);
    }

    // Status and read-only note.
    if (hasSession) {
        const ReviewState& review = m_session->document().review();
        QString text = QStringLiteral(u"%1  —  %2").arg(m_session->imagePath(), statusText(m_session->status()));
        if (review.skip == SkipState::Skipped && !review.skipReason.isEmpty())
            text += QStringLiteral(u"（%1）").arg(review.skipReason);
        m_imageStatus->setText(text);
        QString note = readOnlyText(m_session->file().readOnlyReason());
        if (m_images[m_current].nameCollision)
            note = QStringLiteral(u"同一目录下有主文件名相同的图片（例如 001.jpg 和 001.png），它们会共用一个标注文件。"
                                  u"只能查看，请交给工程师改名后重新打开项目。");
        m_readOnlyNote->setText(note);
        m_readOnlyNote->setVisible(!note.isEmpty());
        m_correctSizeButton->setVisible(m_session->file().readOnlyReason()
                                        == formats::ReadOnlyReason::ImageSizeMismatch
                                        && !m_images[m_current].nameCollision);
    } else {
        m_imageStatus->setText(m_project ? QString() : QStringLiteral(u"请从“文件”菜单打开项目"));
        m_readOnlyNote->setVisible(false);
        m_correctSizeButton->setVisible(false);
    }

    // Shapes.
    m_shapeList->blockSignals(true);
    m_shapeList->clear();
    if (hasSession) {
        for (const Shape& shape : m_session->document().shapes()) {
            const QString type = shape.kind == ShapeKind::Unrecognized ? QStringLiteral(u"无法识别的对象")
                                                                       : shapeTypeText(shape.type);
            m_shapeList->addItem(QStringLiteral("%1  (%2)").arg(shape.label.isEmpty() ? QStringLiteral("-") : shape.label,
                                                               type));
        }
        m_shapeList->setCurrentRow(int(m_canvas->selectedShape()));
    }
    m_shapeList->blockSignals(false);
    m_deleteButton->setEnabled(editable && m_canvas->selectedShape() >= 0);

    // Image-level labels.
    for (QAbstractButton* button : std::as_const(m_imageLabelButtons)) {
        const QString option = button->text();
        button->setEnabled(editable);
        button->setChecked(hasSession && m_session->document().imageLabel(option).value_or(false));
    }

    // Review.
    const QList<ConfirmBlocker> blockers = hasSession ? m_session->confirmBlockers() : QList<ConfirmBlocker>();
    QStringList blockerLines;
    for (const ConfirmBlocker& blocker : blockers)
        blockerLines.append(blockerText(blocker));
    m_blockers->setText(blockerLines.join(QLatin1Char('\n')));
    m_blockers->setVisible(editable && !blockerLines.isEmpty());
    const bool checked = hasSession && m_session->document().review().checked;
    const bool noShapes = hasSession && m_session->document().shapes().isEmpty();
    const bool shapeProject = m_project && !m_project->config().shapeTypes.isEmpty();
    m_confirmButton->setText(noShapes && shapeProject ? QStringLiteral(u"确认无目标") : QStringLiteral(u"确认"));
    m_confirmButton->setEnabled(editable && blockers.isEmpty() && !checked);
    const bool skipped = hasSession && m_session->document().review().skip == SkipState::Skipped;
    m_skipButton->setEnabled(editable);
    m_unskipButton->setVisible(skipped);
    m_unskipButton->setEnabled(editable);

    m_undoAction->setEnabled(editable && m_session->canUndo());
    m_redoAction->setEnabled(editable && m_session->canRedo());
    m_saveState->setText(saveStateText());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    closeProject();
    event->accept();
}

} // namespace liteanno::ui
