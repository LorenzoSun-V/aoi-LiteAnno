#include "ui/MainWindow.h"

#include "application/ExportService.h"
#include "application/FileStatus.h"
#include "application/RecoveryService.h"
#include "dataset/DirectoryScanner.h"
#include "imaging/ImageLoader.h"
#include "ui/Texts.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

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

QString openErrorText(const ProjectSession::OpenResult& result)
{
    switch (result.error) {
    case ProjectSession::OpenError::None:
        return QString();
    case ProjectSession::OpenError::ProjectFileUnreadable:
        return QStringLiteral(u"文件夹里有 liteanno 文件夹，但其中的项目文件 project.json 不存在或无法读取（%1）。"
                              u"请检查文件夹权限，或交给工程师处理。")
            .arg(result.message);
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
{
    setPrompts(Prompts::dialogs(this));
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(m_options.idleSaveMs);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] { commitCurrent(); });
    m_progressTimer.setSingleShot(true);
    m_progressTimer.setInterval(1000);
    connect(&m_progressTimer, &QTimer::timeout, this, &MainWindow::writeProgress);
    buildUi();
    refresh();
}

MainWindow::~MainWindow()
{
    closeProject(true);
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
    connect(m_canvas, &Canvas::shapeRotated, this, [this](qsizetype index, const RotatedBox& box) {
        if (m_session && m_session->setShapeRotation(index, box))
            edited();
    });
    connect(m_canvas, &Canvas::visibilityChanged, this, &MainWindow::refresh);
    connect(m_canvas, &Canvas::selectionChanged, this, [this](qsizetype index) {
        m_shapeList->setCurrentRow(int(index));
        refresh();
    });
    connect(m_canvas, &Canvas::labelEditRequested, this, &MainWindow::editLabel);
    connect(m_canvas, &Canvas::deleteRequested, this, [this] { deleteSelectedShape(); });

    // Images.
    m_imageList = new QListWidget;
    m_imageList->setObjectName(QStringLiteral("imageList"));
    // Every row is one line: without this the list measures each of its rows (seconds at
    // 200 000 images).
    m_imageList->setUniformItemSizes(true);
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

    m_scanState = new QLabel;
    m_scanState->setObjectName(QStringLiteral("scanState"));
    statusBar()->addPermanentWidget(m_scanState);
    m_batchState = new QLabel;
    m_batchState->setObjectName(QStringLiteral("batchState"));
    statusBar()->addPermanentWidget(m_batchState);
    m_saveState = new QLabel;
    statusBar()->addPermanentWidget(m_saveState);

    // Menus and tools. Shortcuts are X-AnyLabeling 4.2.0's defaults (its
    // xanylabeling_config.yaml), so people used to it need not learn new ones.
    const auto named = [](QAction* action, const char* name) {
        action->setObjectName(QLatin1String(name));
        return action;
    };
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral(u"文件(&F)"));
    QAction* open = named(fileMenu->addAction(QStringLiteral(u"打开文件夹…"), this, &MainWindow::chooseProject), "open_dir");
    open->setShortcut(QKeySequence(QStringLiteral("Ctrl+U")));
    QAction* save = named(fileMenu->addAction(QStringLiteral(u"立即保存"), this, &MainWindow::saveNow), "save");
    save->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
    fileMenu->addAction(QStringLiteral(u"处理未写入的编辑…"), this, &MainWindow::resolveRecoveryRecords);
    // No shortcut: X-AnyLabeling has no such action.
    fileMenu->addAction(QStringLiteral(u"重新扫描文件夹"), this, &MainWindow::rescan);
    fileMenu->addAction(QStringLiteral(u"导出…"), this, [this] { exportAnnotations(); });
    fileMenu->addSeparator();
    QAction* quit = named(fileMenu->addAction(QStringLiteral(u"退出"), this, &QWidget::close), "quit");
    quit->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral(u"编辑(&E)"));
    m_undoAction = named(editMenu->addAction(QStringLiteral(u"撤销"), this, [this] { undo(); }), "undo");
    m_undoAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Z")));
    // X-AnyLabeling has no redo; Ctrl+Y is its "use the last label" switch, so it stays free.
    m_redoAction = named(editMenu->addAction(QStringLiteral(u"重做"), this, [this] { redo(); }), "redo");
    m_redoAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Z")));
    editMenu->addSeparator();
    QAction* editLabelAction = named(editMenu->addAction(QStringLiteral(u"修改类别"), this,
                                                         [this] { editLabel(m_canvas->selectedShape()); }),
                                     "edit_label");
    editLabelAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    QAction* deleteAction = named(editMenu->addAction(QStringLiteral(u"删除选中形状"), this,
                                                      [this] { deleteSelectedShape(); }),
                                  "delete_polygon");
    deleteAction->setShortcut(QKeySequence(QStringLiteral("Delete")));
    QAction* copyAction =
        named(editMenu->addAction(QStringLiteral(u"复制形状"), this, [this] { copySelectedShape(); }), "copy_polygon");
    copyAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+C")));
    QAction* pasteAction =
        named(editMenu->addAction(QStringLiteral(u"粘贴形状"), this, [this] { pasteShape(); }), "paste_polygon");
    pasteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+V")));
    QAction* checkAction = named(editMenu->addAction(QStringLiteral(u"确认当前图片"), this, [this] { confirmCurrent(); }),
                                 "toggle_annotation_checked");
    checkAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+K")));

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral(u"视图(&V)"));
    QAction* zoomIn = named(viewMenu->addAction(QStringLiteral(u"放大"), this, [this] { m_canvas->zoomBy(1.1); }), "zoom_in");
    zoomIn->setShortcut(QKeySequence(QStringLiteral("Ctrl++")));
    QAction* zoomOut = named(viewMenu->addAction(QStringLiteral(u"缩小"), this, [this] { m_canvas->zoomBy(1 / 1.1); }), "zoom_out");
    zoomOut->setShortcut(QKeySequence(QStringLiteral("Ctrl+-")));
    QAction* original = named(viewMenu->addAction(QStringLiteral(u"原始大小"), m_canvas, &Canvas::zoomToOriginalSize),
                              "zoom_to_original");
    original->setShortcut(QKeySequence(QStringLiteral("Ctrl+=")));
    QAction* fit = named(viewMenu->addAction(QStringLiteral(u"适合窗口"), m_canvas, &Canvas::fitToWindow), "fit_window");
    fit->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
    viewMenu->addSeparator();
    QAction* hide = named(viewMenu->addAction(QStringLiteral(u"隐藏选中形状"), m_canvas, &Canvas::hideSelected),
                          "hide_selected_polygons");
    hide->setShortcut(QKeySequence(QStringLiteral("S")));
    QAction* showHidden = named(viewMenu->addAction(QStringLiteral(u"显示上一个隐藏的形状"), m_canvas, &Canvas::showLastHidden),
                                "show_hidden_polygons");
    showHidden->setShortcut(QKeySequence(QStringLiteral("W")));
    QAction* allShapes = named(viewMenu->addAction(QStringLiteral(u"显示全部形状")), "toggle_visibility_shapes");
    allShapes->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    allShapes->setCheckable(true);
    allShapes->setChecked(true);
    connect(allShapes, &QAction::toggled, m_canvas, &Canvas::setShapesVisible);
    QAction* labels = named(viewMenu->addAction(QStringLiteral(u"显示类别文字")), "show_labels");
    labels->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    labels->setCheckable(true);
    labels->setChecked(true);
    connect(labels, &QAction::toggled, m_canvas, &Canvas::setLabelsVisible);

    // Architecture 7: fixed batches, picked up where they were left.
    QMenu* batchMenu = menuBar()->addMenu(QStringLiteral(u"批次(&B)"));
    // A plain folder has no batches (no project file to keep them with).
    m_batchActions = {
        batchMenu->addAction(QStringLiteral(u"继续上次批次"), this, [this] { continueLastBatch(); }),
        batchMenu->addAction(QStringLiteral(u"开始下一批"), this, [this] { startNextBatch(); }),
        batchMenu->addAction(QStringLiteral(u"选择批次…"), this, [this] { chooseBatch(); }),
        batchMenu->addAction(QStringLiteral(u"查看未完成项"), this, [this] { showUnfinished(); }),
    };
    batchMenu->addSeparator();
    m_batchActions.append(batchMenu->addAction(QStringLiteral(u"显示全部图片"), this, &MainWindow::showAllImages));
    batchMenu->addSeparator();
    QAction* nextUnchecked = named(batchMenu->addAction(QStringLiteral(u"下一张未完成"), this, [this] { nextUnfinished(); }),
                                   "open_next_unchecked");
    nextUnchecked->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    QAction* previousUnchecked = named(
        batchMenu->addAction(QStringLiteral(u"上一张未完成"), this, [this] { previousUnfinished(); }), "open_prev_unchecked");
    previousUnchecked->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));

    QToolBar* tools = addToolBar(QStringLiteral(u"工具"));
    tools->setObjectName(QStringLiteral("tools"));
    auto* toolGroup = new QActionGroup(this);
    m_selectTool = named(tools->addAction(QStringLiteral(u"选择 (Ctrl+J)")), "edit_polygon");
    m_selectTool->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    m_rectangleTool = named(tools->addAction(QStringLiteral(u"矩形 (R)")), "create_rectangle");
    m_rectangleTool->setShortcut(QKeySequence(QStringLiteral("R")));
    m_polygonTool = named(tools->addAction(QStringLiteral(u"多边形 (P)")), "create_polygon");
    m_polygonTool->setShortcut(QKeySequence(QStringLiteral("P")));
    m_rotationTool = named(tools->addAction(QStringLiteral(u"旋转框 (O)")), "create_rotation");
    m_rotationTool->setShortcut(QKeySequence(QStringLiteral("O")));
    m_rotationTool->setToolTip(QStringLiteral(u"旋转框 (O)：拖出后选中，按 Z/V 转 1°，X/C 转 0.1°"));
    // Upstream's keys; the toolbar names them.
    const struct {
        const char* name;
        const char16_t* text;
        const char* keys;
        Canvas::Tool tool;
        ShapeType type;
    } moreTools[] = {
        {"create_point", u"点 (Shift+P)", "Shift+P", Canvas::Tool::Point, ShapeType::Point},
        {"create_line", u"线段 (Shift+L)", "Shift+L", Canvas::Tool::Line, ShapeType::Line},
        {"create_linestrip", u"折线 (Shift+S)", "Shift+S", Canvas::Tool::LineStrip, ShapeType::LineStrip},
        {"create_circle", u"圆 (Shift+C)", "Shift+C", Canvas::Tool::Circle, ShapeType::Circle},
    };
    QList<QAction*> toolActions = {m_selectTool, m_rectangleTool, m_polygonTool, m_rotationTool};
    for (const auto& more : moreTools) {
        QAction* action = named(tools->addAction(QString::fromUtf16(more.text)), more.name);
        action->setShortcut(QKeySequence(QLatin1String(more.keys)));
        const Canvas::Tool tool = more.tool;
        connect(action, &QAction::triggered, this, [this, tool] { m_canvas->setTool(tool); });
        toolActions.append(action);
        m_moreTools.append({more.type, action});
    }
    for (QAction* action : toolActions) {
        action->setCheckable(true);
        toolGroup->addAction(action);
    }
    m_selectTool->setChecked(true);
    connect(m_selectTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Select); });
    connect(m_rectangleTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Rectangle); });
    connect(m_polygonTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Polygon); });
    connect(m_rotationTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Rotation); });
    tools->addSeparator();
    QAction* previous = named(tools->addAction(QStringLiteral(u"上一张 (A)"), this, [this] { previousImage(); }), "open_prev");
    previous->setShortcut(QKeySequence(QStringLiteral("A")));
    QAction* next = named(tools->addAction(QStringLiteral(u"下一张 (D)"), this, [this] { nextImage(); }), "open_next");
    next->setShortcut(QKeySequence(QStringLiteral("D")));
    tools->addAction(fit);
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
    QString root;
    {
        const Busy busy(this);  // the dialog runs events; a close waits until it is answered
        root = QFileDialog::getExistingDirectory(this, QStringLiteral(u"打开图片文件夹"));
    }
    if (!root.isEmpty())
        openProject(root);
}

bool MainWindow::openProject(const QString& root)
{
    if (!closeProject())
        return false;
    ProjectSession::OpenResult result = ProjectSession::open(root, m_options.localData);
    if (!result.session) {
        m_prompts.message(QStringLiteral(u"无法打开文件夹"), openErrorText(result));
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
    m_rotationTool->setEnabled(types.contains(ShapeType::Rotation));
    for (const auto& [type, action] : std::as_const(m_moreTools))
        action->setEnabled(types.contains(type));
    m_selectTool->setChecked(true);
    m_canvas->setTool(Canvas::Tool::Select);
    buildImageLabelPanel();
    loadKnownLabels();
    for (QAction* action : std::as_const(m_batchActions))
        action->setEnabled(!m_project->isPlainFolder());

    handleRecoveryRecords();
    if (!m_project->isPlainFolder())
        loadWorksets();

    const QString name = m_project->config().name.isEmpty() ? QDir::toNativeSeparators(m_project->rootPath())
                                                            : m_project->config().name;
    setWindowTitle(QStringLiteral("%1 - aoi-LiteAnno").arg(name));

    // Architecture 6: the list the catalog has is shown at once; the scan brings it up to date.
    m_catalog = std::make_unique<application::ProjectCatalog>(*m_project, imaging::readImageInfo);
    if (!m_catalog->isOpen()) {
        // The catalog is only a cache: without one the folder is listed here, every time.
        statusBar()->showMessage(
            QStringLiteral(u"无法建立本机目录索引（%1），每次打开和重新扫描都要完整列一遍文件夹，也不显示各图状态。")
                .arg(m_catalog->error()),
            15000);
        m_catalog.reset();
        listWithoutCatalog();
    } else {
        if (m_catalog->isTemporary()) {
            statusBar()->showMessage(QStringLiteral(u"本机缓存文件夹不能用（%1），这次的目录索引放在临时文件夹，关闭项目后不保留。")
                                         .arg(m_project->paths().cacheDir(m_project->config().projectId,
                                                                          m_project->location().bindingId)),
                                     15000);
        }
        connect(m_catalog.get(), &application::ProjectCatalog::scanProgress, this, [this](int found) {
            m_scanState->setText(QStringLiteral(u"正在扫描文件夹：已找到 %1 张").arg(found));
        });
        connect(m_catalog.get(), &application::ProjectCatalog::scanFinished, this, &MainWindow::scanFinished);
        connect(m_catalog.get(), &application::ProjectCatalog::checkProgress, this, [this](int done, int total) {
            m_scanState->setText(total > 0 ? QStringLiteral(u"正在核对标注状态：%1 / %2").arg(done).arg(total) : QString());
        });
        connect(m_catalog.get(), &application::ProjectCatalog::checkFinished, this, [this] {
            m_scanState->clear();
            reloadStatuses();
        });
        reloadImages();
        m_scanState->setText(QStringLiteral(u"正在扫描文件夹…"));
        m_catalog->start();
        if (m_options.waitForScan) {
            m_catalog->waitForScan(-1);
            const Busy busy(this);
            QCoreApplication::processEvents();
        }
    }
    if (m_current < 0 && !m_images.isEmpty())
        goToImage(startIndex());
    else
        refresh();
    return true;
}

void MainWindow::rescan()
{
    if (!m_project)
        return;
    if (!m_catalog) {
        listWithoutCatalog();
        return;
    }
    m_scanState->setText(QStringLiteral(u"正在扫描文件夹…"));
    m_catalog->start();
}

void MainWindow::scanFinished(bool complete)
{
    if (!m_catalog)
        return;
    m_scanComplete = complete;
    reloadImages();
    const dataset::ScanReports reports = m_catalog->reader().reports();
    showScanReports(reports.unsupported, reports.unreadable);
    if (m_current < 0 && !m_images.isEmpty())
        goToImage(startIndex());
}

bool MainWindow::exportAnnotations()
{
    if (!m_project)
        return false;
    const Busy busy(this);
    // What is exported is what the files hold.
    m_idleTimer.stop();
    flushSaves();
    writeProgress();
    const application::WorksetStore::Batch* batch = m_worksets ? m_worksets->find(m_batchStem) : nullptr;
    const int batchNumber = batch && batch->list ? batch->number : 0;
    ExportQuestion question;
    question.batchNumber = batchNumber;
    question.plainFolder = m_project->isPlainFolder();
    question.projectClasses = !m_project->config().classes.isEmpty();
    const QString classesFile = m_project->rootPath() + QStringLiteral("/classes.txt");
    if (!question.projectClasses && QFileInfo(classesFile).isFile())
        question.classesFile = QDir::toNativeSeparators(classesFile);
    const std::optional<ExportChoice> choice = m_prompts.exportOptions(question);
    if (!choice)
        return false;
    application::ExportRequest request;
    request.format = choice->format;
    request.outputDir = choice->outputDir;
    request.copyImages = choice->copyImages;
    request.classesFile = choice->classesFile;
    if (!choice->wholeProject && batchNumber > 0)
        request.batches = {m_batchStem};
    const application::ExportService service(*m_project, imaging::readImageInfo);
    // The progress dialog runs events; a list a scan brings meanwhile waits.
    const ListHold hold(this);
    const auto run = [&] {
        QProgressDialog progress(QStringLiteral(u"正在导出…"), QStringLiteral(u"取消"), 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(400);
        application::ExportReport result = service.run(request, [&](int done, int total) {
            progress.setMaximum(std::max(total, 0));
            progress.setValue(done);  // runs events once the dialog shows, which blocks the window
            // Until then only what keeps the window painted: no clicks or keys reach it.
            if (!progress.isVisible())
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            return !progress.wasCanceled() && !m_closeAsked;
        });
        return result;
    };
    application::ExportReport report = run();
    // A close asked for meanwhile happens when this returns (Busy).
    if (m_closeAsked)
        return false;
    const QList<application::ExportItem> unconvertible = report.unconvertible();
    if (!report.written && !report.cancelled && !unconvertible.isEmpty()) {
        QStringList lines;
        for (const application::ExportItem& item : unconvertible.mid(0, 10))
            lines.append(QStringLiteral(u"%1：%2").arg(item.image, item.reason));
        if (unconvertible.size() > 10)
            lines.append(QStringLiteral(u"……共 %1 张").arg(unconvertible.size()));
        const QString text = QStringLiteral(u"有 %1 张图片包含不能导出为“%2”的对象，这些图片不能导出：\n%3\n\n"
                                            u"可以跳过这些图片导出其余图片，跳过的图片会列在导出清单里；也可以先修改标注再导出。")
                                 .arg(unconvertible.size())
                                 .arg(exportFormatText(request.format), lines.join(QLatin1Char('\n')));
        if (!m_prompts.skipUnconvertible(text))
            return false;
        request.skipUnconvertible = true;
        report = run();
        if (m_closeAsked)
            return false;
    }
    if (!report.written) {
        if (!report.cancelled)
            m_prompts.message(QStringLiteral(u"没有导出"), report.error);
        return false;
    }
    QString text = QStringLiteral(u"已导出 %1 张图片（其中无目标 %2 张）。\n跳过 %3 张，未完成 %4 张，无法转换 %5 张，没有导出。\n"
                                  u"导出清单：%6")
                       .arg(report.count(application::ExportResult::Exported)
                            + report.count(application::ExportResult::Negative))
                       .arg(report.count(application::ExportResult::Negative))
                       .arg(report.count(application::ExportResult::Skipped))
                       .arg(report.count(application::ExportResult::NotDone))
                       .arg(report.count(application::ExportResult::Unconvertible))
                       .arg(QDir::toNativeSeparators(report.manifestPath));
    if (!m_unwritten.isEmpty())
        text += QStringLiteral(u"\n\n有 %1 张图片的修改还没写入标注文件，导出的是文件里原来的内容。").arg(m_unwritten.size());
    m_prompts.message(QStringLiteral(u"导出完成"), text);
    return true;
}

void MainWindow::showScanReports(const QStringList& unsupported, const QStringList& unreadable)
{
    if (!unreadable.isEmpty()) {
        // Architecture 6.2: their images stay as the last listing had them.
        statusBar()->showMessage(QStringLiteral(u"有 %1 个文件夹无法读取（例如 %2），里面的图片按上次的列表显示。请检查文件夹权限。")
                                     .arg(unreadable.size())
                                     .arg(unreadable.first()),
                                 15000);
    } else if (!unsupported.isEmpty()) {
        statusBar()->showMessage(QStringLiteral(u"有 %1 个文件是不支持的图片格式（例如 %2），请转换为 JPG 或 PNG。")
                                     .arg(unsupported.size())
                                     .arg(unsupported.first()),
                                 15000);
    }
}

void MainWindow::listWithoutCatalog()
{
    const dataset::ScanResult scan = dataset::scanDirectory(m_project->rootPath());
    QList<dataset::CatalogEntry> images;
    images.reserve(scan.images.size());
    for (const dataset::ScannedImage& scanned : scan.images) {
        dataset::CatalogEntry entry;
        entry.path = scanned.path;
        entry.position = int(images.size());
        entry.nameCollision = scanned.nameCollision;
        entry.image = scanned.image;
        entry.annotationName = scanned.annotationName;
        entry.annotation = scanned.annotation;
        images.append(entry);
    }
    m_scanComplete = scan.unreadable.isEmpty();
    setImages(std::move(images));
    showScanReports(scan.unsupported, scan.unreadable);
}

void MainWindow::reloadImages()
{
    if (m_catalog)
        setImages(m_catalog->reader().images());
}

MainWindow::Busy::Busy(MainWindow* window) : m_window(window)
{
    ++m_window->m_busy;
}

MainWindow::Busy::~Busy()
{
    if (--m_window->m_busy == 0 && m_window->m_closeAsked)
        QTimer::singleShot(0, m_window, &QWidget::close);
}

MainWindow::ListHold::ListHold(MainWindow* window)
    : m_window(window)
{
    ++m_window->m_listHolds;
}

MainWindow::ListHold::~ListHold()
{
    if (--m_window->m_listHolds > 0 || !m_window->m_pendingImages)
        return;
    QList<dataset::CatalogEntry> images = std::move(*m_window->m_pendingImages);
    m_window->m_pendingImages.reset();
    if (!m_window->m_project)
        return;
    // The catalog may have moved on while the list waited (statuses checked, another scan):
    // what it has now is applied, not the list that was kept aside.
    if (m_window->m_catalog)
        m_window->reloadImages();
    else
        m_window->setImages(std::move(images));
}

void MainWindow::setPrompts(Prompts prompts)
{
    // Each question runs an event loop; the list waits until it is answered, and so does a
    // close asked for meanwhile (Busy): the code that asked goes on with the project after it.
    auto held = [this](auto ask) {
        return [this, ask](auto&&... args) {
            ListHold hold(this);
            const Busy busy(this);
            return ask(std::forward<decltype(args)>(args)...);
        };
    };
    m_prompts.chooseLabel = held(std::move(prompts.chooseLabel));
    m_prompts.skipReason = held(std::move(prompts.skipReason));
    m_prompts.externalChange = held(std::move(prompts.externalChange));
    m_prompts.recovery = held(std::move(prompts.recovery));
    m_prompts.message = held(std::move(prompts.message));
    m_prompts.confirmDiscard = held(std::move(prompts.confirmDiscard));
    m_prompts.chooseBatch = held(std::move(prompts.chooseBatch));
    m_prompts.exportOptions = held(std::move(prompts.exportOptions));
    m_prompts.skipUnconvertible = held(std::move(prompts.skipUnconvertible));
    m_prompts.setAsideUnusable = held(std::move(prompts.setAsideUnusable));
}

void MainWindow::setImages(QList<dataset::CatalogEntry> images)
{
    if (m_listHolds > 0) {
        m_pendingImages = std::move(images);
        return;
    }
    const QString currentPath = m_current >= 0 ? m_images[m_current].path : QString();
    m_allImages = std::move(images);
    m_images = m_batchStem.isEmpty() ? m_allImages : batchEntries();
    m_imageIndex.clear();
    m_imageIndex.reserve(m_images.size());
    for (int i = 0; i < m_images.size(); ++i)
        m_imageIndex.insert(m_images[i].path, i);
    if (!currentPath.isEmpty()) {
        const int found = m_imageIndex.value(currentPath, -1);
        if (found >= 0) {
            m_current = found;
        } else {
            // The image shown was deleted or renamed by another program. Its edits are kept
            // as a new image would be; the next image is the one after it.
            m_images.append(dataset::CatalogEntry{});
            m_images.last().path = currentPath;
            m_images.last().missing = true;
            m_images.last().nameCollision = m_session && m_session->hasNameCollision();
            m_imageIndex.insert(currentPath, int(m_images.size()) - 1);
            m_current = int(m_images.size()) - 1;
        }
    }
    // Of all images: other batches are counted too ("选择批次…").
    m_statuses.clear();
    for (const dataset::CatalogEntry& entry : std::as_const(m_allImages)) {
        if (entry.status && entry.verified)
            m_statuses.insert(entry.path, *entry.status);
    }
    // The list follows the images at once: everything below may open a question, and the
    // event loop of a question may refresh the list.
    m_imageList->blockSignals(true);
    m_imageList->clear();
    QStringList texts;
    texts.reserve(m_images.size());
    for (const dataset::CatalogEntry& image : std::as_const(m_images))
        texts.append(imageItemText(image.path));
    m_imageList->addItems(texts);
    if (m_current >= 0)
        m_imageList->setCurrentRow(m_current);
    m_imageList->blockSignals(false);
    // Format spec 2: a name collision found while the image is shown makes it read-only at
    // once; edits not saved yet stay on screen but are not written to the shared file.
    if (m_session && m_current >= 0 && m_session->hasNameCollision() != m_images[m_current].nameCollision) {
        m_session->setNameCollision(m_images[m_current].nameCollision);
        if (m_session->needsSave()) {
            if (m_images[m_current].nameCollision) {
                statusBar()->showMessage(
                    QStringLiteral(u"这张图片出现了同名图片，改为只能查看。还没保存的修改留在这台电脑的内存里，不写入标注文件；"
                                   u"关闭项目前会再询问。"),
                    15000);
                commitCurrent();
            } else {
                m_idleTimer.start();  // the held edits can be written now
            }
        }
    }
    if (m_session && m_current >= 0)
        m_statuses.insert(m_session->imagePath(), m_session->status());
    refreshImageItem(m_current);
    // A record held back for a name collision stays held back (the image read-only) until
    // it is restored or given up; once the collision is gone it is asked about. Asked after
    // this update is complete: the question runs an event loop, in which a status check or
    // another scan may update the list again.
    queueOfferIfFree();

    refresh();
}

bool MainWindow::recordHeldBack(const QString& imagePath) const
{
    return m_deferredRecords.contains(imagePath) || m_collisionRecords.contains(imagePath);
}

bool MainWindow::collidesNow(const QString& imagePath) const
{
    return dataset::hasNameCollision(m_project->imageFilePath(imagePath)) || latestNameCollision(imagePath);
}

void MainWindow::queueOfferIfFree()
{
    if (!m_session || m_current < 0 || !m_session->isBlocked())
        return;
    const QString& path = m_session->imagePath();
    if (m_deferredRecords.contains(path) || !m_collisionRecords.contains(path) || collidesNow(path))
        return;
    QMetaObject::invokeMethod(this, &MainWindow::offerUnblockedRecords, Qt::QueuedConnection);
}

void MainWindow::offerUnblockedRecords()
{
    // Checked again: the image shown, or the folder, may have changed since this was queued.
    if (!m_session || m_current < 0 || !m_session->isBlocked() || m_deferredRecords.contains(m_session->imagePath())
        || !m_collisionRecords.contains(m_session->imagePath()) || collidesNow(m_session->imagePath()))
        return;
    const QString path = m_session->imagePath();
    dropSession();
    handleRecoveryRecords(path);
    const int index = m_imageIndex.value(path, -1);
    if (index < 0)
        return;
    m_current = index;
    loadCurrentImage();
}

void MainWindow::loadWorksets()
{
    m_worksets = std::make_unique<application::WorksetStore>(m_project->rootPath(), m_project->config().projectId);
    if (!m_worksets->load()) {
        statusBar()->showMessage(QStringLiteral(u"无法读取批次文件夹 %1，暂时显示全部图片，也不能开始下一批。")
                                     .arg(QDir::toNativeSeparators(m_worksets->directory())),
                                 15000);
        return;
    }
    QStringList damaged;
    for (const application::WorksetStore::Batch& batch : m_worksets->batches()) {
        if (!batch.list)
            damaged.append(QStringLiteral("%1（%2）").arg(batch.stem, batch.error));
    }
    const QStringList duplicates = m_worksets->duplicates();
    if (!damaged.isEmpty()) {
        // Format spec 7.1: a damaged list is not repaired; its images are not given out again.
        statusBar()->showMessage(QStringLiteral(u"有 %1 个批次清单已损坏，不能打开，例如 %2。请交给工程师处理。")
                                     .arg(damaged.size())
                                     .arg(damaged.first()),
                                 15000);
    } else if (!duplicates.isEmpty()) {
        statusBar()->showMessage(QStringLiteral(u"有 %1 张图片同时在多个批次里，例如 %2。不影响标注结果。")
                                     .arg(duplicates.size())
                                     .arg(duplicates.first()),
                                 15000);
    }
    if (const application::WorksetStore::Batch* last = m_worksets->lastWorkedOn()) {
        m_batchStem = last->stem;
        m_batchProgress = m_worksets->progress(*last);
    }
}

QList<dataset::CatalogEntry> MainWindow::batchEntries() const
{
    QList<dataset::CatalogEntry> entries;
    const application::WorksetStore::Batch* batch = m_worksets ? m_worksets->find(m_batchStem) : nullptr;
    if (!batch || !batch->list)
        return entries;
    QHash<QString, qsizetype> where;
    where.reserve(m_allImages.size());
    for (qsizetype i = 0; i < m_allImages.size(); ++i)
        where.insert(m_allImages[i].path, i);
    QSet<QString> listed;
    entries.reserve(batch->list->items.size());
    for (const QString& item : batch->list->items) {
        if (listed.contains(item))
            continue;  // listed twice by a script: shown once
        listed.insert(item);
        const qsizetype found = where.value(item, -1);
        if (found >= 0) {
            entries.append(m_allImages[found]);
        } else {
            // In the batch, but not in the folder (any more).
            entries.append(dataset::CatalogEntry{});
            entries.last().path = item;
            entries.last().missing = true;
        }
        entries.last().position = int(entries.size()) - 1;
    }
    return entries;
}

int MainWindow::startIndex() const
{
    if (m_images.isEmpty())
        return -1;
    if (m_batchProgress && !m_batchProgress->lastItem.isEmpty())
        return std::max(0, m_imageIndex.value(m_batchProgress->lastItem, 0));
    return 0;
}

bool MainWindow::isDone(const QString& imagePath) const
{
    const auto status = m_statuses.constFind(imagePath);
    if (status == m_statuses.cend() || !countsAsDone(*status))
        return false;
    if (m_unwritten.contains(imagePath) || m_heldEdits.contains(imagePath))
        return false;
    if (m_saver && m_saver->isPending(imagePath))
        return false;
    // The status of the image shown follows the edits; it counts once they are written.
    return !(m_session && m_session->imagePath() == imagePath && m_session->needsSave());
}

bool MainWindow::isDoneAt(int index) const
{
    return index >= 0 && index < m_images.size() && !m_images[index].missing && isDone(m_images[index].path);
}

QString MainWindow::batchStateText() const
{
    if (!m_project)
        return QString();
    const application::WorksetStore::Batch* batch = m_worksets ? m_worksets->find(m_batchStem) : nullptr;
    if (!batch || !batch->list)
        return QStringLiteral(u"全部图片：%1 张").arg(m_images.size());
    int done = 0;
    for (int i = 0; i < m_images.size(); ++i) {
        if (isDoneAt(i))
            ++done;
    }
    QString text = QStringLiteral(u"第 %1 批：已完成 %2 / %3 张").arg(batch->number).arg(done).arg(m_images.size());
    if (done == m_images.size())
        text += QStringLiteral(u"（本批已完成）");
    return text;
}

formats::WorksetProgress* MainWindow::batchProgress()
{
    const application::WorksetStore::Batch* batch = m_worksets ? m_worksets->find(m_batchStem) : nullptr;
    if (!batch || !batch->list)
        return nullptr;
    if (!m_batchProgress) {
        // Missing or damaged: started again, only the last position was lost (format spec 7.2).
        m_batchProgress = formats::WorksetProgress{};
        m_batchProgress->batchId = batch->list->batchId;
        m_batchProgress->listDigest = batch->list->itemsDigest;
    }
    return &*m_batchProgress;
}

void MainWindow::recordCompletion(int done)
{
    if (m_batchStem.isEmpty() || m_images.isEmpty() || done != m_images.size())
        return;
    formats::WorksetProgress* progress = batchProgress();
    if (!progress || (!progress->events.isEmpty() && progress->events.last().type == QLatin1String("completed")))
        return;
    progress->events.append({QStringLiteral("completed"), application::WorksetStore::timestamp(QDateTime::currentDateTime())});
    m_progressChanged = true;
    writeProgress();
}

void MainWindow::rememberPosition()
{
    if (m_current < 0 || !batchProgress())
        return;
    const QString& path = m_images[m_current].path;
    if (m_batchProgress->lastItem == path)
        return;
    m_batchProgress->lastItem = path;
    m_progressChanged = true;
    m_progressTimer.start();
}

void MainWindow::writeProgress()
{
    m_progressTimer.stop();
    if (!m_progressChanged || !m_batchProgress || !m_worksets)
        return;
    const application::WorksetStore::Batch* batch = m_worksets->find(m_batchStem);
    if (!batch || !batch->list)
        return;
    m_progressChanged = false;
    const formats::SaveResult written = m_worksets->writeProgress(*batch, *m_batchProgress);
    if (written.status != SaveStatus::Saved)
        statusBar()->showMessage(QStringLiteral(u"没能记下这一批看到哪张（%1），标注本身不受影响。").arg(written.error), 10000);
}

void MainWindow::switchBatch(const QString& stem, const QString& preferred)
{
    if (!m_project)
        return;
    writeProgress();
    m_idleTimer.stop();
    commitCurrent();
    dropSession();
    processSaveResults();
    m_current = -1;
    if (!m_batchStem.isEmpty())
        m_lastBatchStem = m_batchStem;
    m_batchStem = stem;
    m_batchProgress.reset();
    m_progressChanged = false;
    if (const application::WorksetStore::Batch* batch = m_worksets ? m_worksets->find(stem) : nullptr)
        m_batchProgress = m_worksets->progress(*batch);
    if (m_catalog)
        reloadImages();
    else
        setImages(m_allImages);
    int index = preferred.isEmpty() ? -1 : m_imageIndex.value(preferred, -1);
    if (index < 0)
        index = startIndex();
    if (index >= 0)
        goToImage(index);
    else
        refresh();
    // Written now: the batch opened last is the one to continue next time.
    if (batchProgress()) {
        m_progressChanged = true;
        writeProgress();
    }
}

bool MainWindow::startNextBatch()
{
    if (!m_project || !m_worksets)
        return false;
    // Format spec 7.1: only images of a complete scan are given out.
    if (!m_scanComplete || (m_catalog && m_catalog->isScanning())) {
        m_prompts.message(QStringLiteral(u"还不能开始下一批"),
                          QStringLiteral(u"文件夹还没有完整扫描一遍，或者有文件夹无法读取。等扫描完成后再开始下一批；"
                                         u"有文件夹无法读取时，请先检查权限，再用“文件 > 重新扫描文件夹”。"));
        return false;
    }
    QStringList candidates;
    const QList<dataset::CatalogEntry> all = m_catalog ? m_catalog->reader().images() : m_allImages;
    candidates.reserve(all.size());
    for (const dataset::CatalogEntry& entry : all) {
        if (!entry.missing && !entry.nameCollision)
            candidates.append(entry.path);
    }
    writeProgress();
    const application::WorksetStore::Created created =
        m_worksets->createNext(candidates, m_project->config().batchSize);
    if (!created.batch) {
        m_prompts.message(QStringLiteral(u"没有开始下一批"),
                          created.error.isEmpty()
                              ? QStringLiteral(u"所有图片都已经分到批次里了，没有新的图片可以分。")
                              : QStringLiteral(u"无法创建批次清单：%1").arg(created.error));
        return false;
    }
    const QString stem = created.batch->stem;
    const int number = created.batch->number;
    const qsizetype count = created.batch->list->items.size();
    switchBatch(stem, QString());
    statusBar()->showMessage(QStringLiteral(u"已开始第 %1 批，共 %2 张。").arg(number).arg(count), 10000);
    return true;
}

bool MainWindow::continueLastBatch()
{
    if (!m_project || !m_worksets)
        return false;
    writeProgress();
    const application::WorksetStore::Batch* last = nullptr;
    if (m_worksets->load()) {
        last = m_worksets->find(m_lastBatchStem);
        if (!last || !last->list)
            last = m_worksets->lastWorkedOn();
    }
    if (!last) {
        m_prompts.message(QStringLiteral(u"没有可以继续的批次"), QStringLiteral(u"还没有批次。请用“批次 > 开始下一批”分一批图片。"));
        return false;
    }
    switchBatch(last->stem, QString());
    return true;
}

bool MainWindow::chooseBatch()
{
    if (!m_project || !m_worksets)
        return false;
    writeProgress();
    m_worksets->load();
    QStringList texts;
    QStringList stems;
    int current = 0;
    // Missing images are not done (format spec 7.1), whatever their last status was.
    QSet<QString> present;
    present.reserve(m_allImages.size());
    for (const dataset::CatalogEntry& entry : std::as_const(m_allImages)) {
        if (!entry.missing)
            present.insert(entry.path);
    }
    for (const application::WorksetStore::Batch& batch : m_worksets->batches()) {
        if (!batch.list)
            continue;
        int done = 0;
        for (const QString& item : batch.list->items) {
            if (present.contains(item) && isDone(item))
                ++done;
        }
        if (batch.stem == m_batchStem)
            current = int(texts.size());
        texts.append(QStringLiteral(u"第 %1 批（已完成 %2 / %3 张）").arg(batch.number).arg(done).arg(batch.list->items.size()));
        stems.append(batch.stem);
    }
    if (texts.isEmpty()) {
        m_prompts.message(QStringLiteral(u"没有批次"), QStringLiteral(u"还没有批次。请用“批次 > 开始下一批”分一批图片。"));
        return false;
    }
    const std::optional<int> chosen = m_prompts.chooseBatch(texts, current);
    if (!chosen || *chosen < 0 || *chosen >= stems.size())
        return false;
    switchBatch(stems[*chosen], QString());
    return true;
}

void MainWindow::showAllImages()
{
    if (!m_project || m_batchStem.isEmpty())
        return;
    switchBatch(QString(), m_current >= 0 ? m_images[m_current].path : QString());
}

int MainWindow::showUnfinished()
{
    if (!m_project)
        return 0;
    int count = 0;
    int first = -1;
    for (int i = 0; i < m_images.size(); ++i) {
        if (isDoneAt(i))
            continue;
        ++count;
        if (first < 0)
            first = i;
    }
    const QString scope = m_batchStem.isEmpty() ? QStringLiteral(u"全部图片") : QStringLiteral(u"本批");
    if (count == 0) {
        statusBar()->showMessage(QStringLiteral(u"%1都已完成。").arg(scope), 10000);
        return 0;
    }
    goToImage(first);
    statusBar()->showMessage(QStringLiteral(u"%1还有 %2 张未完成，按 Ctrl+Shift+D 跳到下一张未完成。").arg(scope).arg(count), 10000);
    return count;
}

bool MainWindow::nextUnfinished()
{
    for (int i = m_current + 1; i < m_images.size(); ++i) {
        if (!isDoneAt(i))
            return goToImage(i);
    }
    statusBar()->showMessage(QStringLiteral(u"后面没有未完成的图片了。"), 5000);
    return false;
}

bool MainWindow::previousUnfinished()
{
    for (int i = std::min<int>(m_current, int(m_images.size())) - 1; i >= 0; --i) {
        if (!isDoneAt(i))
            return goToImage(i);
    }
    statusBar()->showMessage(QStringLiteral(u"前面没有未完成的图片了。"), 5000);
    return false;
}

void MainWindow::reloadStatuses()
{
    if (!m_catalog)
        return;
    m_statuses.clear();
    for (const dataset::CatalogEntry& entry : m_catalog->reader().images()) {
        if (entry.status && entry.verified)
            m_statuses.insert(entry.path, *entry.status);
    }
    if (m_session && m_current >= 0)
        m_statuses.insert(m_session->imagePath(), m_session->status());
    for (int i = 0; i < m_imageList->count(); ++i)
        refreshImageItem(i);
    refreshBatchState();
}

bool MainWindow::closeProject(bool force)
{
    if (!m_project)
        return true;
    m_idleTimer.stop();
    commitCurrent();
    flushSaves();
    if (!m_heldEdits.isEmpty() && !force) {
        QStringList images = m_heldEdits.keys();
        images.sort();
        const QString text =
            QStringLiteral(u"%1 张图片的修改既没能写入标注文件，也没能存到这台电脑上（%2）。\n"
                           u"关闭项目会丢失这些修改：\n%3\n\n"
                           u"可以先检查磁盘空间或文件夹权限，回到这些图片再保存一次。")
                .arg(images.size())
                .arg(m_heldEdits.value(images.first()).error, images.mid(0, 10).join(QLatin1Char('\n')));
        if (!m_prompts.confirmDiscard(text)) {
            refresh();
            return false;
        }
    }
    m_progressTimer.stop();
    writeProgress();
    dropSession();
    m_saver.reset();
    m_catalog.reset();
    m_scanState->clear();
    m_project.reset();
    m_knownLabels.clear();
    m_worksets.reset();
    m_batchStem.clear();
    m_lastBatchStem.clear();
    m_batchProgress.reset();
    m_progressChanged = false;
    m_scanComplete = false;
    m_allImages.clear();
    m_images.clear();
    m_imageIndex.clear();
    m_statuses.clear();
    m_unwritten.clear();
    m_deferredRecords.clear();
    m_collisionRecords.clear();
    m_pendingImages.reset();
    m_heldEdits.clear();
    m_current = -1;
    m_imageList->clear();
    m_canvas->setImage(QImage());
    setWindowTitle(QStringLiteral("aoi-LiteAnno"));
    refresh();
    return true;
}

void MainWindow::resolveRecoveryRecords()
{
    if (!m_project)
        return;
    m_idleTimer.stop();
    flushSaves();
    dropSession();
    m_deferredRecords.clear();
    handleRecoveryRecords();
    // Records this program cannot use stay until the user moves them aside here: otherwise
    // nothing ever clears them (liteanno-cli init waits for none to be left, 13.6).
    const RecoveryService recovery(m_project->paths(), m_project->location());
    const application::RecoveryScan scan = recovery.scan();
    const QString folder =
        QDir::toNativeSeparators(m_project->paths().recoveryDir(m_project->location().projectId, m_project->location().bindingId));
    if (!scan.error.isEmpty()) {
        m_prompts.message(QStringLiteral(u"无法读取恢复记录"),
                          QStringLiteral(u"无法读取本机恢复记录文件夹 %1（%2），里面可能有未写入的编辑。请检查这个文件夹的权限。")
                              .arg(folder, scan.error));
    } else if (!scan.unusable.isEmpty()) {
        QStringList lines;
        for (const application::UnusableRecord& record : scan.unusable.mid(0, 10))
            lines.append(QStringLiteral(u"%1：%2").arg(QFileInfo(record.recordPath).fileName(), record.error));
        if (scan.unusable.size() > 10)
            lines.append(QStringLiteral(u"……共 %1 份").arg(scan.unusable.size()));
        const QString text = QStringLiteral(u"有 %1 份恢复记录本程序无法使用（损坏、版本更新或属于别的位置）：\n%2\n\n"
                                            u"可以把它们原样移到 %3 下的 unusable 文件夹保留，交给工程师查看；不会删除。")
                                 .arg(scan.unusable.size())
                                 .arg(lines.join(QLatin1Char('\n')), folder);
        if (m_prompts.setAsideUnusable(text)) {
            QStringList stuck;
            for (const application::UnusableRecord& record : scan.unusable) {
                if (!application::moveRecordToUnusable(record.recordPath))
                    stuck.append(QFileInfo(record.recordPath).fileName());
            }
            if (!stuck.isEmpty()) {
                m_prompts.message(QStringLiteral(u"没有全部移走"),
                                  QStringLiteral(u"有 %1 份记录没能移动（例如 %2），仍留在原处。").arg(stuck.size()).arg(stuck.first()));
            }
        }
    }
    if (m_current >= 0)
        loadCurrentImage();
}

void MainWindow::dropSession()
{
    m_session.reset();
    m_canvas->setDocument(nullptr, true);
}

void MainWindow::handleRecoveryRecords(const QString& onlyImage)
{
    const RecoveryService recovery(m_project->paths(), m_project->location());
    application::RecoveryScan scan = recovery.scan();
    recovery.removeAlreadySaved(scan);
    // A record that is gone (saved over, removed by hand) holds nothing back any more.
    if (!onlyImage.isEmpty()
        && std::none_of(scan.candidates.cbegin(), scan.candidates.cend(), [&](const RecoveryCandidate& candidate) {
               return candidate.record.imagePath == onlyImage && candidate.state != RecoveryState::AlreadySaved;
           })) {
        m_collisionRecords.remove(onlyImage);
        m_deferredRecords.remove(onlyImage);
    }
    for (const RecoveryCandidate& candidate : std::as_const(scan.candidates)) {
        if (candidate.state == RecoveryState::AlreadySaved)
            continue;
        if (!onlyImage.isEmpty() && candidate.record.imagePath != onlyImage)
            continue;
        // Format spec 2: restoring would write the annotation file the images share. The
        // record stays, and the image read-only, until the collision is gone.
        if (collidesNow(candidate.record.imagePath)) {
            m_collisionRecords.insert(candidate.record.imagePath);
            m_unwritten.insert(candidate.record.imagePath);
            continue;
        }
        m_collisionRecords.remove(candidate.record.imagePath);
        RecoveryChoice choice = m_prompts.recovery(candidate, readFile(candidate.annotationPath));
        // Checked again after the question: a same-name image may have appeared meanwhile.
        if (choice == RecoveryChoice::Restore && collidesNow(candidate.record.imagePath)) {
            m_collisionRecords.insert(candidate.record.imagePath);
            m_unwritten.insert(candidate.record.imagePath);
            m_prompts.message(QStringLiteral(u"没有恢复"),
                              QStringLiteral(u"图片 %1 旁边出现了同名图片，不能写入共用的标注文件。未写入的编辑仍保存在这台电脑上，"
                                             u"同名图片移走后会再询问。")
                                  .arg(candidate.record.imagePath));
            continue;
        }
        application::RecordResult result;
        if (choice == RecoveryChoice::Restore)
            result = recovery.restore(candidate, candidate.state == RecoveryState::Conflict);
        else if (choice == RecoveryChoice::Discard)
            result = recovery.discard(candidate);
        if (choice == RecoveryChoice::Later) {
            m_deferredRecords.insert(candidate.record.imagePath);
            m_unwritten.insert(candidate.record.imagePath);
            continue;
        }
        if (result.status == application::RecordStatus::Done) {
            const auto held = m_heldEdits.find(candidate.record.imagePath);
            if (held == m_heldEdits.end()) {
                m_unwritten.remove(candidate.record.imagePath);
            } else if (choice == RecoveryChoice::Restore) {
                // The held edits are newer than the record and go on from the file this
                // program just wrote, not from an external change.
                held->stamp = formats::readFileStamp(candidate.annotationPath);
            }
            m_deferredRecords.remove(candidate.record.imagePath);
        } else if (result.status == application::RecordStatus::NameCollision) {
            // Found while writing: held back like a collision found before the question.
            m_collisionRecords.insert(candidate.record.imagePath);
            m_unwritten.insert(candidate.record.imagePath);
            m_prompts.message(QStringLiteral(u"没有恢复"),
                              QStringLiteral(u"图片 %1 旁边出现了同名图片，不能写入共用的标注文件。未写入的编辑仍保存在这台电脑上，"
                                             u"同名图片移走后会再询问。")
                                  .arg(candidate.record.imagePath));
        } else {
            m_deferredRecords.insert(candidate.record.imagePath);
            m_unwritten.insert(candidate.record.imagePath);
            m_prompts.message(QStringLiteral(u"未完成"),
                              QStringLiteral(u"图片 %1 的未写入编辑没有处理完：%2")
                                  .arg(candidate.record.imagePath, result.error));
        }
    }
    if (onlyImage.isEmpty() && !scan.error.isEmpty()) {
        statusBar()->showMessage(QStringLiteral(u"无法读取本机恢复记录文件夹（%1），里面可能有未写入的编辑。请检查权限。").arg(scan.error),
                                 15000);
    } else if (onlyImage.isEmpty() && !scan.unusable.isEmpty()) {
        statusBar()->showMessage(QStringLiteral(u"有 %1 份本机恢复记录无法使用，已保留；可在“文件 > 处理未写入的编辑”中移到 unusable 文件夹。")
                                     .arg(scan.unusable.size()), 15000);
    }
}

bool MainWindow::goToImage(int index)
{
    if (!m_project || index < 0 || index >= m_images.size())
        return false;
    // `index` and the rows below stay valid to the end: a list a scan brings meanwhile is
    // applied after the switch, which maps the image shown to its new row.
    const ListHold hold(this);
    if (index == m_current && m_session) {
        refresh();
        return true;
    }
    const QString path = m_images[index].path;
    m_idleTimer.stop();
    commitCurrent();
    // Architecture 8.2: at the limit, wait for saves before showing the next image.
    if (m_saver->atLimit())
        waitForSaver([this](int ms) { return m_saver->waitUntilBelowLimit(ms); });
    // A save of that image may still be running; its file is read after it.
    if (m_saver->isPending(path))
        waitForSaver([this, path](int ms) { return m_saver->waitUntilIdle(path, ms); });
    // A scan that finished during the waits may have reordered the list.
    index = m_imageIndex.value(path, -1);
    if (index < 0)
        return false;
    // The session goes first: a save of the image being left that already finished is
    // reported like any other image's, not taken as the state of an image still shown.
    dropSession();
    processSaveResults();

    m_current = index;
    // A record the user already put off is not asked about again; the image stays read-only.
    if (m_unwritten.contains(path) && !m_deferredRecords.contains(path)) {
        handleRecoveryRecords(path);
        // The question's event loop may have brought a new list.
        m_current = m_imageIndex.value(path, -1);
        if (m_current < 0) {
            refresh();
            return false;
        }
    }
    loadCurrentImage();
    m_imageList->blockSignals(true);
    m_imageList->setCurrentRow(m_current);
    m_imageList->blockSignals(false);
    rememberPosition();
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
    const dataset::CatalogEntry& scanned = m_images[m_current];
    const imaging::LoadedImage image = imaging::loadImage(m_project->imageFilePath(scanned.path));
    m_canvas->setImage(image.image);
    if (image.image.isNull()) {
        dropSession();
        statusBar()->showMessage(QStringLiteral(u"图片无法打开：%1").arg(image.error), 15000);
        refresh();
        return;
    }
    if (image.hasMoreFrames)
        statusBar()->showMessage(QStringLiteral(u"这张图片有多页或多帧，只显示第一页。"), 10000);
    // Edits that could not be saved are opened, not the file without them.
    const auto held = m_heldEdits.constFind(scanned.path);
    formats::LoadedAnnotation loaded = held != m_heldEdits.cend()
                                           ? formats::LoadedAnnotation{held->file->clone(), held->stamp}
                                           : formats::loadAnnotationFile(m_project->annotationFilePath(scanned.path),
                                                                         image.info,
                                                                         m_project->config().imageLabels.options);
    if (held != m_heldEdits.cend())
        m_saveError = held->error;
    m_session = std::make_unique<DocumentSession>(scanned.path, std::move(loaded), m_project->config(),
                                                  scanned.nameCollision);
    m_session->setBlocked(recordHeldBack(scanned.path));
    for (const Shape& shape : m_session->document().shapes())
        addKnownLabel(shape.label);
    m_canvas->setDocument(&m_session->document(), m_session->isReadOnly());
    if (held != m_heldEdits.cend())
        m_idleTimer.start();
    queueOfferIfFree();
    refresh();
}

void MainWindow::reloadCurrent()
{
    if (m_current < 0)
        return;
    dropSession();
    loadCurrentImage();
}

void MainWindow::commitCurrent(const formats::SaveOptions& options, bool force)
{
    // Format spec 2: edits made before a name collision was found must not go into the
    // annotation file the images share, nor be lost. They are held like edits whose save
    // failed: shown again when the image is opened, written once the collision is gone,
    // and closing the project asks before giving them up.
    // The collision the catalog knows now counts, also while a new list waits for a
    // question to close.
    if (m_session && !m_session->hasNameCollision() && latestNameCollision(m_session->imagePath())) {
        m_session->setNameCollision(true);
        statusBar()->showMessage(QStringLiteral(u"这张图片出现了同名图片，改为只能查看。还没保存的修改留在这台电脑的内存里，不写入标注文件；"
                                                u"关闭项目前会再询问。"),
                                 15000);
    }
    if (m_session && m_session->hasNameCollision() && m_session->needsSave()) {
        formats::LoadedAnnotation copy = m_session->copy();
        m_heldEdits.insert(m_session->imagePath(),
                           HeldEdit{std::make_shared<formats::AnnotationFile>(std::move(copy.file)), copy.stamp,
                                    QStringLiteral(u"同一目录下有同名图片，不能写入共用的标注文件"),
                                    m_session->id(), m_session->version()});
        if (!m_unwritten.contains(m_session->imagePath())) {
            m_unwritten.insert(m_session->imagePath());
            refreshImageItem(m_current);
        }
        return;
    }
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
    const ListHold hold(this);
    const Busy busy(this);
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
    // Answering a prompt can hand over another save ("用我的版本覆盖"). That save may already
    // be finished when the loop checks again; its result still has to be handled.
    do {
        if (m_saver->pendingCount() > 0)
            waitForSaver([this](int ms) { return m_saver->waitUntilAllDone(ms); });
        processSaveResults();
    } while (m_saver->pendingCount() > 0 || (!m_handlingResults && m_saver->hasResults()));
}

bool MainWindow::latestNameCollision(const QString& imagePath) const
{
    if (m_catalog) {
        const std::optional<dataset::CatalogEntry> entry = m_catalog->reader().image(imagePath);
        if (entry && entry->nameCollision)
            return true;
    }
    if (m_pendingImages) {
        for (const dataset::CatalogEntry& entry : *m_pendingImages) {
            if (entry.path == imagePath && entry.nameCollision)
                return true;
        }
    }
    const int index = m_imageIndex.value(imagePath, -1);
    return index >= 0 && m_images[index].nameCollision;
}

bool MainWindow::heldNewerThan(const QString& imagePath, quint64 sessionId, qint64 version) const
{
    const auto held = m_heldEdits.constFind(imagePath);
    if (held == m_heldEdits.cend())
        return false;
    return held->sessionId > sessionId || (held->sessionId == sessionId && held->version > version);
}

void MainWindow::processSaveResults()
{
    if (!m_saver || m_handlingResults)
        return;
    const ListHold hold(this);
    m_handlingResults = true;
    for (application::SaveJobResult& result : m_saver->takeResults()) {
        const SaveStatus status = result.outcome.result.status;
        // Results of an earlier session of the same image (before "重新加载") are not this one's.
        const bool current = m_session && m_session->id() == result.sessionId;
        // Edits held after this save was handed over (a name collision found meanwhile) are
        // not what it wrote or failed to write; they stay.
        const bool newerHeld = heldNewerThan(result.imagePath, result.sessionId, result.documentVersion);
        if (status == SaveStatus::Saved || status == SaveStatus::NotModified) {
            // A saved edit replaced the image's recovery record (architecture 8.1): nothing
            // of it waits any more.
            if (status == SaveStatus::Saved) {
                m_collisionRecords.remove(result.imagePath);
                m_deferredRecords.remove(result.imagePath);
            }
            if (!newerHeld) {
                m_unwritten.remove(result.imagePath);
                m_heldEdits.remove(result.imagePath);
            } else if (status == SaveStatus::Saved && m_heldEdits[result.imagePath].sessionId == result.sessionId) {
                // The held edits go on from this session's own write; when they can be written
                // the file they replace is this one, not an external change.
                m_heldEdits[result.imagePath].stamp = result.stamp;
            }
            if (status == SaveStatus::Saved && result.saved) {
                // Format spec 5.4: what this program wrote counts without reading it again.
                const int index = m_imageIndex.value(result.imagePath, -1);
                const bool nameCollision = index >= 0 && m_images[index].nameCollision;
                const AnnotationStatus saved =
                    application::statusOfFile(*result.saved, m_project->config(), nameCollision, true);
                if (m_catalog)
                    m_catalog->recordSaved(result.imagePath, saved, result.stamp, nameCollision);
                // The image shown has its status from the session; others, of this batch or
                // another one ("选择批次…"), from what was written.
                if (!current)
                    m_statuses.insert(result.imagePath, saved);
            }
            if (current && status == SaveStatus::Saved) {
                m_session->savedSnapshot(result.entryId, *result.saved, result.stamp);
                m_saveError.clear();
                m_externalChangeDeferred = false;
                // Undone or edited while the save ran: the file has to follow.
                if (m_session->needsSave())
                    m_idleTimer.start();
            }
            continue;
        }
        if (newerHeld) {
            // Kept as they are; the failed older version is in the recovery record, if any.
        } else if (result.outcome.recoveryRecordKept) {
            m_heldEdits.remove(result.imagePath);
        } else if (result.saved) {
            m_heldEdits.insert(result.imagePath,
                               HeldEdit{result.saved, result.stamp,
                                        status == SaveStatus::ExternallyModified
                                            ? QStringLiteral(u"文件被其他程序修改过")
                                            : result.outcome.result.error,
                                        result.sessionId, result.documentVersion});
        }
        // The next commit hands the edits over again.
        if (current)
            m_submittedVersion = -1;
        if (current && result.outcome.nameCollision) {
            // The edits are in the recovery record and on screen; the image is read-only
            // until the same-name image is gone, and the record is offered then.
            m_session->setNameCollision(true);
            m_collisionRecords.insert(result.imagePath);
            m_unwritten.insert(result.imagePath);
            m_saveError = result.outcome.result.error;
            continue;
        }
        if (current && status == SaveStatus::ExternallyModified) {
            m_unwritten.insert(result.imagePath);
            m_saveError = QStringLiteral(u"标注文件被其他程序修改，修改未写入");
            if (m_externalChangeDeferred)
                continue;
            switch (m_prompts.externalChange(result.imagePath)) {
            case ExternalChangeChoice::Reload:
                // The user gives up the edits for the file as it is now.
                m_saver->coordinator().discardRecoveryRecord(result.imagePath);
                m_unwritten.remove(result.imagePath);
                m_heldEdits.remove(result.imagePath);
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
        const QString reason = status == SaveStatus::ExternallyModified ? QStringLiteral(u"文件被其他程序修改过")
                                                                        : result.outcome.result.error;
        if (result.outcome.recoveryRecordKept) {
            m_unwritten.insert(result.imagePath);
            m_prompts.message(QStringLiteral(u"修改没有写入"),
                              QStringLiteral(u"图片 %1 的修改没有写入标注文件（%2）。\n修改已保存在这台电脑上，"
                                             u"再次打开这张图片时会提示处理。")
                                  .arg(result.imagePath, reason));
        } else {
            m_prompts.message(QStringLiteral(u"修改没有写入"),
                              QStringLiteral(u"图片 %1 的修改没有写入标注文件，也没能存到这台电脑上（%2）。\n"
                                             u"修改还在程序里：回到这张图片会接着保存。关闭项目前请先处理。")
                                  .arg(result.imagePath, reason));
        }
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

std::optional<QString> MainWindow::askLabel(const QString& suggested)
{
    const QStringList classes = classNames();
    // Without a class list any name goes, as in X-AnyLabeling; the names used are offered.
    const std::optional<QString> label =
        classes.isEmpty() ? m_prompts.chooseLabel(m_knownLabels, suggested, true) : m_prompts.chooseLabel(classes, suggested, false);
    if (label)
        addKnownLabel(*label);
    return label;
}

void MainWindow::loadKnownLabels()
{
    m_knownLabels.clear();
    // A classes.txt next to the images is offered first, in its order.
    const QString classesFile = m_project->rootPath() + QStringLiteral("/classes.txt");
    if (QFileInfo(classesFile).isFile()) {
        const formats::ClassesFile read = formats::readClassesFile(classesFile);
        for (const QString& name : read.names)
            addKnownLabel(name);
    }
}

void MainWindow::addKnownLabel(const QString& label)
{
    if (!label.isEmpty() && !m_knownLabels.contains(label))
        m_knownLabels.append(label);
}

void MainWindow::addDrawnShape(const Shape& drawn)
{
    if (!m_session || m_session->isReadOnly())
        return;
    const std::optional<QString> label = askLabel(m_lastLabel);
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
    const std::optional<QString> label = askLabel(m_session->document().shapes()[index].label);
    if (label && m_session->setShapeLabel(index, *label)) {
        m_lastLabel = *label;
        edited();
    }
}

bool MainWindow::copySelectedShape()
{
    const qsizetype index = m_canvas->selectedShape();
    if (!m_session || index < 0 || index >= m_session->document().shapes().size())
        return false;
    const Shape& shape = m_session->document().shapes()[index];
    if (shape.kind != ShapeKind::Editable)
        return false;
    m_copiedShape = shape;
    m_copiedShape->copiedObject = m_session->file().shapeObject(index);
    return true;
}

bool MainWindow::pasteShape()
{
    if (!m_session || m_session->isReadOnly() || !m_copiedShape)
        return false;
    // As upstream: the copy lands where the original was, unlocked.
    Shape shape = *m_copiedShape;
    shape.locked = false;
    const qsizetype index = m_session->addShape(shape);
    if (index < 0)
        return false;
    m_canvas->setSelectedShape(index);
    edited();
    return true;
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
    // While a polygon is being drawn, Ctrl+Z takes back its last point, as in X-AnyLabeling.
    if (m_canvas->removeLastPolygonPoint())
        return true;
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
    if (index < 0 || index >= m_imageList->count() || index >= m_images.size())
        return;
    m_imageList->item(index)->setText(imageItemText(m_images[index].path));
}

QString MainWindow::imageItemText(const QString& path) const
{
    QString text = path;
    const int index = m_imageIndex.value(path, -1);
    if (index >= 0 && index < m_images.size() && m_images[index].missing)
        text += QStringLiteral(u"  [缺失]");
    const auto status = m_statuses.constFind(path);
    if (status != m_statuses.cend())
        text += QStringLiteral(u"  [%1]").arg(statusText(*status));
    if (m_unwritten.contains(path))
        text += QStringLiteral(u"  [未写入]");
    return text;
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
        if (m_session->isBlocked())
            note = QStringLiteral(u"这张图片有上次没写入的修改，还没有处理。处理前不能编辑，以免覆盖它："
                                  u"请用“文件 > 处理未写入的编辑…”恢复或放弃。");
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
        const QList<Shape>& shapes = m_session->document().shapes();
        for (qsizetype i = 0; i < shapes.size(); ++i) {
            const Shape& shape = shapes[i];
            const QString type = shape.kind == ShapeKind::Unrecognized ? QStringLiteral(u"无法识别的对象")
                                                                       : shapeTypeText(shape.type);
            QString text = QStringLiteral("%1  (%2)").arg(shape.label.isEmpty() ? QStringLiteral("-") : shape.label, type);
            if (m_canvas->isHidden(i))
                text += QStringLiteral(u"  [已隐藏]");
            m_shapeList->addItem(text);
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
    refreshBatchState();
}

void MainWindow::refreshBatchState()
{
    m_batchState->setText(batchStateText());
    if (!m_batchStem.isEmpty()) {
        int done = 0;
        for (int i = 0; i < m_images.size(); ++i) {
            if (isDoneAt(i))
                ++done;
        }
        recordCompletion(done);
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_busy > 0) {
        m_closeAsked = true;
        event->ignore();
        return;
    }
    m_closeAsked = false;
    if (closeProject())
        event->accept();
    else
        event->ignore();
}

} // namespace liteanno::ui
