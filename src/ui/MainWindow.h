#pragma once

#include "application/BackgroundSaver.h"
#include "application/DocumentSession.h"
#include "application/LocalDataPaths.h"
#include "application/ProjectCatalog.h"
#include "application/ProjectSession.h"
#include "application/WorksetStore.h"
#include "dataset/Catalog.h"
#include "ui/Canvas.h"
#include "ui/Prompts.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QTimer>

#include <memory>

class QAbstractButton;
class QAction;
class QGroupBox;
class QLabel;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace liteanno::ui {

struct MainWindowOptions {
    application::LocalDataPaths localData = application::LocalDataPaths::standard();
    application::BackgroundSaver::Limits saveLimits;
    int idleSaveMs = 800;  // save this long after the last edit
    // Tests slow saves down with it.
    std::function<void(application::SaveStage)> saveStageHook;
    // Opening a project waits until its folder is listed (tests).
    bool waitForScan = false;
};

// The editing window: one project, one image at a time (milestone 2).
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    using Options = MainWindowOptions;

    explicit MainWindow(Options options = Options(), QWidget* parent = nullptr);
    ~MainWindow() override;

    // Every question holds the image list while it is shown; see ListHold.
    void setPrompts(Prompts prompts);

    // Opens the project whose data root is `root`. Reports problems through the prompts.
    bool openProject(const QString& root);
    // Writes every edit and releases the project. Refuses, unless `force`, when the user keeps
    // edits that could not be saved anywhere.
    bool closeProject(bool force = false);

    bool goToImage(int index);
    bool nextImage();
    bool previousImage();
    // Hands the current edits to the background saver now.
    void saveNow();
    // Waits until all handed-over saves are written and their results are handled.
    void flushSaves();
    // "处理未写入的编辑…": asks again about every recovery record, then reopens the image.
    void resolveRecoveryRecords();
    // "重新扫描": lists the folder again and checks what changed (architecture 6.1).
    void rescan();
    // "导出…" (architecture 11): writes the edits first, then exports the files.
    bool exportAnnotations();
    application::ProjectCatalog* catalog() const { return m_catalog.get(); }
    application::BackgroundSaver* saver() const { return m_saver.get(); }

    // Batches (format spec 7, architecture 7). The list shows the images of one batch, or
    // all images when no batch is chosen.
    // "开始下一批": gives out the next images not in any batch yet and shows them.
    bool startNextBatch();
    // "继续上次批次": the batch worked on last, at the image left last.
    bool continueLastBatch();
    // "选择批次…".
    bool chooseBatch();
    // "显示全部图片".
    void showAllImages();
    // "查看未完成项": goes to the first image that is not done; returns how many are not.
    int showUnfinished();
    // Ctrl+Shift+D / Ctrl+Shift+A: the next or previous image in the list that is not done.
    bool nextUnfinished();
    bool previousUnfinished();
    // The list file's stem ("b000003") of the batch shown; empty when all images are.
    QString currentBatch() const { return m_batchStem; }
    QString batchStateText() const;
    application::WorksetStore* worksets() const { return m_worksets.get(); }

    // For tests and status displays.
    int imageCount() const { return int(m_images.size()); }
    int currentIndex() const { return m_current; }
    application::DocumentSession* session() const { return m_session.get(); }
    Canvas* canvas() const { return m_canvas; }
    QString saveStateText() const;
    int savingWaits() const { return m_savingWaits; }
    bool hasProject() const { return m_project != nullptr; }

    // The actions of the panel, as buttons and menu entries do them.
    bool confirmCurrent();
    bool skipCurrent();
    bool unskipCurrent();
    bool undo();
    bool redo();
    bool deleteSelectedShape();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void buildImageLabelPanel();
    void chooseProject();
    void handleRecoveryRecords(const QString& onlyImage = QString());
    void loadCurrentImage();
    // Hands the current edits to the saver unless that version is already handed over.
    void commitCurrent(const formats::SaveOptions& options = {}, bool force = false);
    bool waitForSaver(const std::function<bool(int)>& wait);
    void processSaveResults();
    void reloadCurrent();
    // The canvas must not keep pointing at a session that is gone, also not while a prompt
    // runs before the next one exists.
    void dropSession();
    void edited();
    void refresh();
    // The batch line of the status bar, and the completion event when the batch is done.
    void refreshBatchState();
    void refreshImageItem(int index);
    QString imageItemText(const QString& path) const;
    // The image list from the catalog; the current image stays current.
    void reloadImages();
    void setImages(QList<dataset::CatalogEntry> images);
    void reloadStatuses();
    void scanFinished(bool complete);
    // Without a catalog: lists the folder here, on this thread, and shows no statuses.
    void listWithoutCatalog();
    void showScanReports(const QStringList& unsupported, const QStringList& unreadable);
    void addDrawnShape(const Shape& shape);
    void editLabel(qsizetype index);
    QStringList classNames() const;
    // Shows the batch `stem` (all images when empty), at `preferred` when it is listed.
    void switchBatch(const QString& stem, const QString& preferred);
    // The list for the batch shown, from all images the catalog has.
    QList<dataset::CatalogEntry> batchEntries() const;
    // The image the list starts at: where the batch was left, else the first.
    int startIndex() const;
    // Format spec 5.4: saved, verified and counted as done; not while edits wait.
    bool isDone(const QString& imagePath) const;
    bool isDoneAt(int index) const;
    // The progress of the batch shown, started when its file is missing or damaged.
    formats::WorksetProgress* batchProgress();
    // Remembers the image shown as the batch's last position (written shortly after).
    void rememberPosition();
    // Format spec 7.2: notes once that every image of the batch is done.
    void recordCompletion(int done);
    void writeProgress();
    void loadWorksets();

    Options m_options;
    Prompts m_prompts;
    std::unique_ptr<application::ProjectSession> m_project;
    std::unique_ptr<application::BackgroundSaver> m_saver;
    std::unique_ptr<application::ProjectCatalog> m_catalog;
    QList<dataset::CatalogEntry> m_images;  // the list shown
    QList<dataset::CatalogEntry> m_allImages;  // all images, also when a batch is shown
    std::unique_ptr<application::WorksetStore> m_worksets;
    QString m_batchStem;
    QString m_lastBatchStem;  // the batch shown last in this session, for "继续上次批次"
    std::optional<formats::WorksetProgress> m_batchProgress;
    bool m_progressChanged = false;
    QTimer m_progressTimer;
    // The last scan of this session listed every folder: only its images may be given out.
    bool m_scanComplete = false;
    QHash<QString, int> m_imageIndex;
    QHash<QString, AnnotationStatus> m_statuses;
    int m_current = -1;
    std::unique_ptr<application::DocumentSession> m_session;
    QString m_saveError;  // the last save failure of the current image
    qint64 m_submittedVersion = -1;  // the version of the current image last handed to the saver
    // The user answered "以后再说" to an external change: later saves keep the edits in the
    // recovery record without asking again, until the user saves explicitly.
    bool m_externalChangeDeferred = false;
    QSet<QString> m_unwritten;  // other images whose edits wait in recovery records
    // Recovery records the user answered "以后再说" for; their images stay read-only so that
    // a new save does not replace the record.
    QSet<QString> m_deferredRecords;
    // Recovery records not offered because the image has a name collision: writing them
    // would go into the annotation file the images share. Offered once the collision is gone.
    QSet<QString> m_collisionRecords;
    // Edits whose save failed and left no recovery record (the local folder was not writable
    // either). They live only here; opening the image again continues from them.
    struct HeldEdit {
        std::shared_ptr<formats::AnnotationFile> file;
        formats::FileStamp stamp;
        QString error;
        // The session and its document version the edits come from. Sessions are numbered
        // in the order they were opened, so a save result only concerns held edits that are
        // not newer than what it saved.
        quint64 sessionId = 0;
        qint64 version = 0;
    };
    QHash<QString, HeldEdit> m_heldEdits;
    // Whether edits held for the image are newer than the save of (sessionId, version).
    // While a question is shown or saves are waited for, an event loop runs and a scan may
    // finish. Code around those calls keeps rows of the image list; so a new list is kept
    // aside while any hold exists and applied when the last one ends.
    class ListHold
    {
    public:
        explicit ListHold(MainWindow* window);
        ~ListHold();
        ListHold(const ListHold&) = delete;
        ListHold& operator=(const ListHold&) = delete;

    private:
        MainWindow* m_window;
    };
    int m_listHolds = 0;
    // An export holds a reference to the project while it runs: a close asked for meanwhile
    // cancels it and closes the window once it has ended.
    bool m_exporting = false;
    bool m_closeAfterExport = false;
    std::optional<QList<dataset::CatalogEntry>> m_pendingImages;
    // A recovery record of the image waits: put off by the user, or held back for a name
    // collision. The image is read-only until the record is restored or given up.
    bool recordHeldBack(const QString& imagePath) const;
    // A same-name image on disk now, or in what the window knows.
    bool collidesNow(const QString& imagePath) const;
    // Queues offerUnblockedRecords() when the image shown waits only for a collision that
    // is gone.
    void queueOfferIfFree();
    // Offers the recovery records of the image shown once nothing holds them back.
    void offerUnblockedRecords();
    // Whether the image has a name collision as far as the window knows now: the catalog,
    // a list waiting to be applied, or the list shown.
    bool latestNameCollision(const QString& imagePath) const;
    bool heldNewerThan(const QString& imagePath, quint64 sessionId, qint64 version) const;
    QString m_lastLabel;
    QTimer m_idleTimer;
    int m_savingWaits = 0;
    bool m_handlingResults = false;

    Canvas* m_canvas = nullptr;
    QListWidget* m_imageList = nullptr;
    QListWidget* m_shapeList = nullptr;
    QLabel* m_imageStatus = nullptr;
    QLabel* m_readOnlyNote = nullptr;
    QLabel* m_blockers = nullptr;
    QLabel* m_saveState = nullptr;
    QLabel* m_scanState = nullptr;
    QLabel* m_batchState = nullptr;
    QPushButton* m_confirmButton = nullptr;
    QPushButton* m_skipButton = nullptr;
    QPushButton* m_unskipButton = nullptr;
    QPushButton* m_correctSizeButton = nullptr;
    QPushButton* m_deleteButton = nullptr;
    QGroupBox* m_imageLabelPanel = nullptr;
    QVBoxLayout* m_imageLabelLayout = nullptr;
    QList<QAbstractButton*> m_imageLabelButtons;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_selectTool = nullptr;
    QAction* m_rectangleTool = nullptr;
    QAction* m_polygonTool = nullptr;
};

} // namespace liteanno::ui
