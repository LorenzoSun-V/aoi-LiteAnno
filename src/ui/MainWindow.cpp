#include "ui/MainWindow.h"

#include "application/FileStatus.h"
#include "application/RecoveryService.h"
#include "imaging/ImageLoader.h"
#include "ui/Texts.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
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
    m_saveState = new QLabel;
    statusBar()->addPermanentWidget(m_saveState);

    // Menus and tools. Shortcuts are X-AnyLabeling 4.2.0's defaults (its
    // xanylabeling_config.yaml), so people used to it need not learn new ones.
    const auto named = [](QAction* action, const char* name) {
        action->setObjectName(QLatin1String(name));
        return action;
    };
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral(u"文件(&F)"));
    QAction* open = named(fileMenu->addAction(QStringLiteral(u"打开项目…"), this, &MainWindow::chooseProject), "open_dir");
    open->setShortcut(QKeySequence(QStringLiteral("Ctrl+U")));
    QAction* save = named(fileMenu->addAction(QStringLiteral(u"立即保存"), this, &MainWindow::saveNow), "save");
    save->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
    fileMenu->addAction(QStringLiteral(u"处理未写入的编辑…"), this, &MainWindow::resolveRecoveryRecords);
    // No shortcut: X-AnyLabeling has no such action.
    fileMenu->addAction(QStringLiteral(u"重新扫描文件夹"), this, &MainWindow::rescan);
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

    QToolBar* tools = addToolBar(QStringLiteral(u"工具"));
    tools->setObjectName(QStringLiteral("tools"));
    auto* toolGroup = new QActionGroup(this);
    m_selectTool = named(tools->addAction(QStringLiteral(u"选择 (Ctrl+J)")), "edit_polygon");
    m_selectTool->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    m_rectangleTool = named(tools->addAction(QStringLiteral(u"矩形 (R)")), "create_rectangle");
    m_rectangleTool->setShortcut(QKeySequence(QStringLiteral("R")));
    m_polygonTool = named(tools->addAction(QStringLiteral(u"多边形 (P)")), "create_polygon");
    m_polygonTool->setShortcut(QKeySequence(QStringLiteral("P")));
    for (QAction* action : {m_selectTool, m_rectangleTool, m_polygonTool}) {
        action->setCheckable(true);
        toolGroup->addAction(action);
    }
    m_selectTool->setChecked(true);
    connect(m_selectTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Select); });
    connect(m_rectangleTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Rectangle); });
    connect(m_polygonTool, &QAction::triggered, this, [this] { m_canvas->setTool(Canvas::Tool::Polygon); });
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
    const QString root = QFileDialog::getExistingDirectory(this, QStringLiteral(u"打开项目（选择数据根目录）"));
    if (!root.isEmpty())
        openProject(root);
}

bool MainWindow::openProject(const QString& root)
{
    if (!closeProject())
        return false;
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

    const QString name = m_project->config().name.isEmpty() ? m_project->rootPath() : m_project->config().name;
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
            QCoreApplication::processEvents();
        }
    }
    if (m_current < 0 && !m_images.isEmpty())
        goToImage(0);
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

void MainWindow::scanFinished(bool /*complete*/)
{
    if (!m_catalog)
        return;
    reloadImages();
    const dataset::ScanReports reports = m_catalog->reader().reports();
    showScanReports(reports.unsupported, reports.unreadable);
    if (m_current < 0 && !m_images.isEmpty())
        goToImage(0);
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
    setImages(std::move(images));
    showScanReports(scan.unsupported, scan.unreadable);
}

void MainWindow::reloadImages()
{
    if (m_catalog)
        setImages(m_catalog->reader().images());
}

void MainWindow::setImages(QList<dataset::CatalogEntry> images)
{
    const QString currentPath = m_current >= 0 ? m_images[m_current].path : QString();
    m_images = std::move(images);
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
    m_statuses.clear();
    for (const dataset::CatalogEntry& entry : std::as_const(m_images)) {
        if (entry.status && entry.verified)
            m_statuses.insert(entry.path, *entry.status);
    }
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
    refresh();
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
    dropSession();
    m_saver.reset();
    m_catalog.reset();
    m_scanState->clear();
    m_project.reset();
    m_images.clear();
    m_imageIndex.clear();
    m_statuses.clear();
    m_unwritten.clear();
    m_deferredRecords.clear();
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
        if (choice == RecoveryChoice::Later) {
            m_deferredRecords.insert(candidate.record.imagePath);
            m_unwritten.insert(candidate.record.imagePath);
            continue;
        }
        if (result.status == application::RecordStatus::Done) {
            m_unwritten.remove(candidate.record.imagePath);
            m_deferredRecords.remove(candidate.record.imagePath);
        } else {
            m_deferredRecords.insert(candidate.record.imagePath);
            m_unwritten.insert(candidate.record.imagePath);
            m_prompts.message(QStringLiteral(u"未完成"),
                              QStringLiteral(u"图片 %1 的未写入编辑没有处理完：%2")
                                  .arg(candidate.record.imagePath, result.error));
        }
    }
    if (onlyImage.isEmpty() && !scan.unusable.isEmpty()) {
        statusBar()->showMessage(QStringLiteral(u"有 %1 份本机恢复记录无法使用，已保留；保存同一张图片前会把它移到恢复目录下的 unusable 文件夹。")
                                     .arg(scan.unusable.size()), 15000);
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
    if (m_unwritten.contains(path) && !m_deferredRecords.contains(path))
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
    m_session->setBlocked(m_deferredRecords.contains(scanned.path));
    m_canvas->setDocument(&m_session->document(), m_session->isReadOnly());
    if (held != m_heldEdits.cend())
        m_idleTimer.start();
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
    if (m_session && m_session->hasNameCollision() && m_session->needsSave()) {
        formats::LoadedAnnotation copy = m_session->copy();
        m_heldEdits.insert(m_session->imagePath(),
                           HeldEdit{std::make_shared<formats::AnnotationFile>(std::move(copy.file)), copy.stamp,
                                    QStringLiteral(u"同一目录下有同名图片，不能写入共用的标注文件")});
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

void MainWindow::processSaveResults()
{
    if (!m_saver || m_handlingResults)
        return;
    m_handlingResults = true;
    for (application::SaveJobResult& result : m_saver->takeResults()) {
        const SaveStatus status = result.outcome.result.status;
        // Results of an earlier session of the same image (before "重新加载") are not this one's.
        const bool current = m_session && m_session->id() == result.sessionId;
        if (status == SaveStatus::Saved || status == SaveStatus::NotModified) {
            m_unwritten.remove(result.imagePath);
            m_heldEdits.remove(result.imagePath);
            if (status == SaveStatus::Saved && m_catalog && result.saved) {
                // Format spec 5.4: what this program wrote counts without reading it again.
                const int index = m_imageIndex.value(result.imagePath, -1);
                const bool nameCollision = index >= 0 && m_images[index].nameCollision;
                m_catalog->recordSaved(result.imagePath,
                                       application::statusOfFile(*result.saved, m_project->config(), nameCollision, true),
                                       result.stamp, nameCollision);
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
        if (result.outcome.recoveryRecordKept) {
            m_heldEdits.remove(result.imagePath);
        } else if (result.saved) {
            m_heldEdits.insert(result.imagePath,
                               HeldEdit{result.saved, result.stamp,
                                        status == SaveStatus::ExternallyModified
                                            ? QStringLiteral(u"文件被其他程序修改过")
                                            : result.outcome.result.error});
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
    if (index < 0 || index >= m_imageList->count())
        return;
    m_imageList->item(index)->setText(imageItemText(m_images[index].path));
}

QString MainWindow::imageItemText(const QString& path) const
{
    QString text = path;
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
    if (closeProject())
        event->accept();
    else
        event->ignore();
}

} // namespace liteanno::ui
