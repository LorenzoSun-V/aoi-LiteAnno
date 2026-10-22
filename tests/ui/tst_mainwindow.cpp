// The editing window driven like a user: drawing with the mouse, undo and redo, the review
// buttons, background saves and the prompts for external changes and recovery records.
// Runs offscreen; the prompts are replaced so nothing modal blocks.

#include "application/LocalDataPaths.h"
#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QWheelEvent>

#include <atomic>
#include <memory>

using namespace liteanno;
using namespace liteanno::ui;

namespace {

const char* const kProjectId = "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33";

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

// Recovery records of all projects under a local data folder.
int recoveryRecordCount(const QString& localRoot)
{
    int count = 0;
    QDirIterator it(localRoot + QLatin1String("/recovery"), {QStringLiteral("*.json")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

// A file where the recovery folder should be: no record can be written, also not by root.
void blockRecoveryFolder(const QString& localRoot)
{
    QDir(localRoot + QLatin1String("/recovery")).removeRecursively();
    writeFile(localRoot + QLatin1String("/recovery"), "blocked");
}

void unblockRecoveryFolder(const QString& localRoot)
{
    QFile::remove(localRoot + QLatin1String("/recovery"));
}

QJsonObject readJson(const QString& path)
{
    return QJsonDocument::fromJson(readFile(path)).object();
}

void mouse(QWidget* widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button,
           Qt::MouseButtons buttons)
{
    QMouseEvent event(type, position, widget->mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void drag(Canvas* canvas, const QPointF& fromImage, const QPointF& toImage)
{
    const QPointF from = canvas->imageToWidget(fromImage);
    const QPointF to = canvas->imageToWidget(toImage);
    mouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

void click(Canvas* canvas, const QPointF& imagePoint)
{
    const QPointF at = canvas->imageToWidget(imagePoint);
    mouse(canvas, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
}

// What the stand-in prompts answer, and what they were asked.
struct Answers {
    QString label = QStringLiteral("scratch");
    QString skipReason = QStringLiteral("blurred");
    ExternalChangeChoice externalChange = ExternalChangeChoice::Reload;
    RecoveryChoice recovery = RecoveryChoice::Restore;
    bool discardUnsaved = false;
    int batch = 0;  // the batch chosen from the list offered
    QStringList batchesOffered;
    std::optional<ExportChoice> exportChoice;
    bool skipUnconvertible = false;
    int exportBatchOffered = -1;
    QStringList unconvertibleTexts;
    // Run while the recovery question or a message is shown, like the event loop behind a
    // dialog.
    std::function<void()> duringRecovery;
    std::function<void()> duringMessage;
    std::function<void()> duringExternalChange;
    std::function<void()> duringLabel;

    int externalChangeAsked = 0;
    QList<application::RecoveryState> recoveryAsked;
    QStringList messages;
    int discardAsked = 0;

    Prompts prompts()
    {
        Prompts p;
        p.chooseLabel = [this](const QStringList&, const QString&) -> std::optional<QString> {
            if (duringLabel)
                duringLabel();
            if (label.isEmpty())
                return std::nullopt;
            return label;
        };
        p.skipReason = [this]() -> std::optional<QString> { return skipReason; };
        p.externalChange = [this](const QString&) {
            ++externalChangeAsked;
            if (duringExternalChange)
                duringExternalChange();
            return externalChange;
        };
        p.recovery = [this](const application::RecoveryCandidate& candidate, const QByteArray&) {
            recoveryAsked.append(candidate.state);
            if (duringRecovery)
                duringRecovery();
            return recovery;
        };
        p.confirmDiscard = [this](const QString&) {
            ++discardAsked;
            return discardUnsaved;
        };
        p.chooseBatch = [this](const QStringList& batches, int) -> std::optional<int> {
            batchesOffered = batches;
            if (batch < 0)
                return std::nullopt;
            return batch;
        };
        p.exportOptions = [this](int batchNumber) {
            exportBatchOffered = batchNumber;
            return exportChoice;
        };
        p.skipUnconvertible = [this](const QString& text) {
            unconvertibleTexts.append(text);
            return skipUnconvertible;
        };
        p.message = [this](const QString& title, const QString& text) {
            messages.append(title + QLatin1Char('\n') + text);
            if (duringMessage)
                duringMessage();
        };
        return p;
    }
};

struct Project {
    QTemporaryDir local;
    QTemporaryDir data;

    explicit Project(const QByteArray& extra = QByteArray(), int images = 3)
    {
        writeProjectFile(extra);
        for (int i = 1; i <= images; ++i) {
            QImage image(200, 100, QImage::Format_RGB32);
            image.fill(QColor(40 * i, 90, 120));
            const QString path = data.filePath(QStringLiteral("line1/%1.png").arg(i, 4, 10, QLatin1Char('0')));
            QDir().mkpath(QFileInfo(path).absolutePath());
            if (!image.save(path))
                qFatal("cannot write %s", qPrintable(path));
        }
    }

    void writeProjectFile(const QByteArray& extra)
    {
        QByteArray text = QByteArray("{\"formatVersion\": \"1.0\", \"projectId\": \"") + kProjectId
                          + "\", \"shapeTypes\": [\"rectangle\", \"polygon\"], "
                            "\"classes\": [{\"id\": 0, \"name\": \"scratch\"}, {\"id\": 1, \"name\": \"dent\"}]";
        if (!extra.isEmpty())
            text += ", " + extra;
        text += "}";
        writeFile(data.filePath(QStringLiteral("liteanno/project.json")), text);
    }

    QString json(int image) const { return data.filePath(QStringLiteral("line1/%1.json").arg(image, 4, 10, QLatin1Char('0'))); }

    MainWindow::Options options() const
    {
        MainWindow::Options o;
        o.localData = application::LocalDataPaths(local.path());
        o.idleSaveMs = 60 * 60 * 1000;  // saves happen when the tests ask for them
        o.waitForScan = true;
        return o;
    }
};

struct Window {
    Answers answers;
    MainWindow window;

    explicit Window(const MainWindow::Options& options)
        : window(options)
    {
        window.setPrompts(answers.prompts());
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
    }

    Canvas* canvas() { return window.canvas(); }
    QPushButton* confirmButton() { return window.findChild<QPushButton*>(QStringLiteral("confirmButton")); }
    QString blockers() { return window.findChild<QLabel*>(QStringLiteral("blockers"))->text(); }
    const AnnotationDocument& document() { return window.session()->document(); }

    void drawRectangle(const QPointF& from, const QPointF& to)
    {
        canvas()->setTool(Canvas::Tool::Rectangle);
        canvas()->fitToWindow();
        drag(canvas(), from, to);
        canvas()->setTool(Canvas::Tool::Select);
    }
};

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void invalidProjectFileNamesTheField()
    {
        Project project;
        writeFile(project.data.filePath(QStringLiteral("liteanno/project.json")),
                  QByteArray("{\"formatVersion\": \"1.0\", \"projectId\": \"") + kProjectId + "\", \"batchSize\": 0}");
        Window w(project.options());
        QVERIFY(!w.window.openProject(project.data.path()));
        QVERIFY(!w.window.hasProject());
        QCOMPARE(w.answers.messages.size(), 1);
        QVERIFY2(w.answers.messages[0].contains(QStringLiteral("/batchSize")), qPrintable(w.answers.messages[0]));

        // A folder without a project file.
        QTemporaryDir empty;
        QVERIFY(!w.window.openProject(empty.path()));
        QVERIFY(w.answers.messages[1].contains(QStringLiteral("project.json")));
    }

    void projectOpenElsewhereIsLocked()
    {
        Project project;
        Window first(project.options());
        QVERIFY(first.window.openProject(project.data.path()));
        Window second(project.options());
        QVERIFY(!second.window.openProject(project.data.path()));
        QCOMPARE(second.answers.messages.size(), 1);
        QVERIFY2(second.answers.messages[0].contains(QStringLiteral(u"另一个窗口")), qPrintable(second.answers.messages[0]));

        first.window.closeProject();
        QVERIFY(second.window.openProject(project.data.path()));
    }

    void drawUndoConfirmAndSave()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QCOMPARE(w.window.imageCount(), 3);
        QCOMPARE(w.window.currentIndex(), 0);
        // No shapes yet: the button confirms an image without objects.
        QCOMPARE(w.confirmButton()->text(), QStringLiteral(u"确认无目标"));
        QVERIFY(w.confirmButton()->isEnabled());

        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QCOMPARE(w.document().shapes().size(), 1);
        const Shape& shape = w.document().shapes()[0];
        QCOMPARE(shape.label, QStringLiteral("scratch"));
        QCOMPARE(shape.type, ShapeType::Rectangle);
        QVERIFY(qAbs(shape.points[0].x() - 20) < 1 && qAbs(shape.points[2].y() - 60) < 1);
        QCOMPARE(w.confirmButton()->text(), QStringLiteral(u"确认"));

        // Moving it with the select tool.
        const PointList before = w.document().shapes()[0].points;
        drag(w.canvas(), QPointF(50, 35), QPointF(60, 40));
        const PointList moved = w.document().shapes()[0].points;
        QVERIFY(qAbs(moved[0].x() - before[0].x() - 10) < 1);
        QVERIFY(qAbs(moved[0].y() - before[0].y() - 5) < 1);

        // Undo and redo, including the confirmation.
        QVERIFY(w.window.confirmCurrent());
        QVERIFY(w.document().review().checked);
        QVERIFY(!w.confirmButton()->isEnabled());
        QVERIFY(w.window.undo());
        QVERIFY(!w.document().review().checked);
        QVERIFY(w.window.undo());
        QCOMPARE(w.document().shapes()[0].points, before);
        QVERIFY(w.window.undo());
        QVERIFY(w.document().shapes().isEmpty());
        QVERIFY(!w.window.undo());
        QVERIFY(w.window.redo());
        QVERIFY(w.window.redo());
        QVERIFY(w.window.redo());
        QVERIFY(w.document().review().checked);
        QCOMPARE(w.document().shapes()[0].points, moved);

        // Going to the next image saves this one in the background.
        QVERIFY(!QFile::exists(project.json(1)));
        QVERIFY(w.window.nextImage());
        QCOMPARE(w.window.currentIndex(), 1);
        w.window.flushSaves();
        QJsonObject saved = readJson(project.json(1));
        QCOMPARE(saved.value(QStringLiteral("shapes")).toArray().size(), 1);
        QCOMPARE(saved.value(QStringLiteral("checked")).toBool(), true);
        QCOMPARE(saved.value(QStringLiteral("imagePath")).toString(), QStringLiteral("0001.png"));

        // A polygon: three clicks and Enter.
        w.canvas()->setTool(Canvas::Tool::Polygon);
        w.canvas()->fitToWindow();
        click(w.canvas(), QPointF(10, 10));
        click(w.canvas(), QPointF(90, 15));
        click(w.canvas(), QPointF(50, 80));
        QTest::keyClick(w.canvas(), Qt::Key_Return);
        QCOMPARE(w.document().shapes().size(), 1);
        QCOMPARE(w.document().shapes()[0].type, ShapeType::Polygon);
        QCOMPARE(w.document().shapes()[0].points.size(), 3);

        // Cancelling the label leaves nothing behind.
        w.answers.label.clear();
        w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
        QCOMPARE(w.document().shapes().size(), 1);

        // Deleting, and the first image again: read back from its file.
        w.canvas()->setSelectedShape(0);
        QVERIFY(w.window.deleteSelectedShape());
        QVERIFY(w.document().shapes().isEmpty());
        QVERIFY(w.window.previousImage());
        QCOMPARE(w.document().shapes().size(), 1);
        QVERIFY(w.document().review().checked);
        QCOMPARE(w.window.saveStateText(), QStringLiteral(u"已保存"));
        // The second image had an edit, so it has a file now.
        w.window.flushSaves();
        QVERIFY(QFile::exists(project.json(2)));
    }

    void confirmBlockedWithReasons()
    {
        Project project;
        writeFile(project.json(1), R"({
  "version": "2.4.0",
  "flags": {},
  "shapes": [
    {
      "label": "crack",
      "points": [[10.0, 10.0], [40.0, 10.0], [40.0, 30.0], [10.0, 30.0]],
      "group_id": null,
      "description": "",
      "shape_type": "rectangle",
      "flags": {}
    }
  ],
  "imagePath": "0001.png",
  "imageData": null,
  "imageHeight": 100,
  "imageWidth": 200
})");
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(!w.confirmButton()->isEnabled());
        QVERIFY(!w.blockers().isEmpty());
        QVERIFY2(w.blockers().contains(QStringLiteral("crack")), qPrintable(w.blockers()));
        QVERIFY(!w.window.confirmCurrent());

        // Choosing a project class removes the reason.
        w.canvas()->setSelectedShape(0);
        emit w.canvas()->labelEditRequested(0);
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("scratch"));
        QVERIFY(w.confirmButton()->isEnabled());
        QVERIFY(w.blockers().isEmpty());
        QVERIFY(w.window.confirmCurrent());
    }

    void skipNeedsAReason()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        w.answers.skipReason = QStringLiteral("   ");
        QVERIFY(!w.window.skipCurrent());
        QCOMPARE(w.document().review().skip, SkipState::None);

        w.answers.skipReason = QStringLiteral(u"图片模糊");
        QVERIFY(w.window.skipCurrent());
        QCOMPARE(w.document().review().skip, SkipState::Skipped);
        QCOMPARE(w.document().review().skipReason, QStringLiteral(u"图片模糊"));
        QVERIFY(w.window.unskipCurrent());
        QCOMPARE(w.document().review().skip, SkipState::None);
        QVERIFY(w.window.undo());
        QCOMPARE(w.document().review().skip, SkipState::Skipped);

        w.window.saveNow();
        w.window.flushSaves();
        const QJsonObject saved = readJson(project.json(1));
        QCOMPARE(saved.value(QStringLiteral("liteanno")).toObject().value(QStringLiteral("skipReason")).toString(),
                 QStringLiteral(u"图片模糊"));
    }

    void singleChoiceImageLabels()
    {
        Project project(R"("imageLabels": {"mode": "single", "options": ["ok", "ng"]})");
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        auto* ok = w.window.findChild<QAbstractButton*>(QStringLiteral("imageLabel:ok"));
        auto* ng = w.window.findChild<QAbstractButton*>(QStringLiteral("imageLabel:ng"));
        QVERIFY(ok && ng);
        ok->click();
        QCOMPARE(w.document().imageLabel(QStringLiteral("ok")), std::optional<bool>(true));
        ng->click();
        QCOMPARE(w.document().imageLabel(QStringLiteral("ok")), std::optional<bool>(false));
        QCOMPARE(w.document().imageLabel(QStringLiteral("ng")), std::optional<bool>(true));
        QVERIFY(ng->isChecked() && !ok->isChecked());
        QVERIFY(w.window.undo());
        QVERIFY(ok->isChecked() && !ng->isChecked());
    }

    void externalChangeReloadOrOverwrite()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(w.window.saveStateText(), QStringLiteral(u"已保存"));

        // Another program rewrites the file; the next save stops and asks.
        QJsonObject external = readJson(project.json(1));
        external.insert(QStringLiteral("shapes"), QJsonArray());
        writeFile(project.json(1), QJsonDocument(external).toJson());
        w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
        QCOMPARE(w.document().shapes().size(), 2);

        w.answers.externalChange = ExternalChangeChoice::Later;
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(w.answers.externalChangeAsked, 1);
        QVERIFY2(w.window.saveStateText().startsWith(QStringLiteral(u"保存失败")), qPrintable(w.window.saveStateText()));
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 0);

        // "重新加载": the file as the other program left it.
        w.answers.externalChange = ExternalChangeChoice::Reload;
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(w.answers.externalChangeAsked, 2);
        QVERIFY(w.document().shapes().isEmpty());
        QCOMPARE(w.window.saveStateText(), QStringLiteral(u"已保存"));

        // "用我的版本覆盖".
        writeFile(project.json(1), QJsonDocument(readJson(project.json(1))).toJson(QJsonDocument::Compact));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.answers.externalChange = ExternalChangeChoice::Overwrite;
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(w.answers.externalChangeAsked, 3);
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 1);
        QCOMPARE(w.window.saveStateText(), QStringLiteral(u"已保存"));
    }

    void recoveryRecordsAtOpen()
    {
        Project project;
        QByteArray savedBytes;
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
            w.window.saveNow();
            w.window.flushSaves();
            savedBytes = readFile(project.json(1));

            // Kept aside after an external change, and still there when the window closes.
            writeFile(project.json(1), QByteArray("{\"broken\": "));
            w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
            w.answers.externalChange = ExternalChangeChoice::Later;
            w.window.closeProject();
            QVERIFY(w.answers.externalChangeAsked >= 1);
        }

        // The file differs from the one the edit was based on: both are shown, the user restores.
        {
            Window w(project.options());
            w.answers.recovery = RecoveryChoice::Later;
            QVERIFY(w.window.openProject(project.data.path()));
            QCOMPARE(w.answers.recoveryAsked, QList<application::RecoveryState>{application::RecoveryState::Conflict});
            w.window.closeProject();
        }

        // The file is back as it was: the record can be written on top of it.
        writeFile(project.json(1), savedBytes);
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            QCOMPARE(w.answers.recoveryAsked, QList<application::RecoveryState>{application::RecoveryState::Restorable});
            QCOMPARE(w.document().shapes().size(), 2);
            QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 2);
            w.window.closeProject();
        }

        // Nothing left to ask about.
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.answers.recoveryAsked.isEmpty());
    }

    void switchingImagesWaitsAtTheLimit()
    {
        Project project;
        MainWindow::Options options = project.options();
        options.saveLimits.maxDocuments = 1;
        options.saveStageHook = [](application::SaveStage stage) {
            if (stage == application::SaveStage::RecoveryRecordWritten)
                QThread::msleep(300);
        };
        Window w(options);
        QVERIFY(w.window.openProject(project.data.path()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QCOMPARE(w.window.savingWaits(), 0);
        QVERIFY(w.window.nextImage());
        // The save of the first image was still running: the switch waited for it.
        QCOMPARE(w.window.savingWaits(), 1);
        QVERIFY(QFile::exists(project.json(1)));
        QCOMPARE(w.window.currentIndex(), 1);

        // Below the limit no wait.
        QVERIFY(w.window.nextImage());
        QCOMPARE(w.window.savingWaits(), 1);
    }

    // Review of 168a9cd, item 1: a save that fails without a recovery record keeps the edits
    // in the program, and closing asks before losing them.
    void failedSaveWithoutRecordKeepsEdits()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        blockRecoveryFolder(project.local.path());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.nextImage());
        w.window.flushSaves();
        QVERIFY(!QFile::exists(project.json(1)));
        QCOMPARE(w.answers.messages.size(), 1);

        // Back on the image: the rectangle is still there, and still not saved.
        QVERIFY(w.window.previousImage());
        QCOMPARE(w.document().shapes().size(), 1);
        QVERIFY(w.window.session()->needsSave());
        QVERIFY2(w.window.saveStateText().startsWith(QStringLiteral(u"保存失败")), qPrintable(w.window.saveStateText()));

        // Closing asks; the default answer keeps the project open.
        QVERIFY(!w.window.closeProject());
        QCOMPARE(w.answers.discardAsked, 1);
        QVERIFY(w.window.hasProject());
        QCOMPARE(w.document().shapes().size(), 1);

        // Once the folder is writable again the edits are saved, and closing does not ask.
        unblockRecoveryFolder(project.local.path());
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 1);
        QVERIFY(w.window.closeProject());
        QCOMPARE(w.answers.discardAsked, 1);
    }

    // CI on d154c9e: a failed save that finished before the switch to the next image was
    // taken as the shown image's state, so leaving it gave no message.
    void failedSaveFinishedBeforeSwitchingIsReported()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        blockRecoveryFolder(project.local.path());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.window.saveNow();
        QThread::msleep(300);  // the save fails meanwhile; its result waits for the event loop
        QVERIFY(w.window.nextImage());
        QCOMPARE(w.answers.messages.size(), 1);
        QVERIFY(w.answers.messages[0].contains(QStringLiteral(u"修改还在程序里")));
        QVERIFY(w.window.previousImage());
        QCOMPARE(w.document().shapes().size(), 1);
        QVERIFY(w.window.session()->needsSave());
    }

    void closingCanGiveUpUnsavedEdits()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        blockRecoveryFolder(project.local.path());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.answers.discardUnsaved = true;
        QVERIFY(w.window.closeProject());
        QCOMPARE(w.answers.discardAsked, 1);
        QVERIFY(!w.window.hasProject());
        QVERIFY(!QFile::exists(project.json(1)));
    }

    // Item 4: a record put off with "以后再说" is not replaced by new saves.
    void deferredRecordBlocksEditing()
    {
        Project project;
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
            w.window.saveNow();
            w.window.flushSaves();
            // Another program's valid change, so only the record keeps the image read-only.
            QJsonObject external = readJson(project.json(1));
            external.insert(QStringLiteral("shapes"), QJsonArray());
            writeFile(project.json(1), QJsonDocument(external).toJson());
            w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
            w.answers.externalChange = ExternalChangeChoice::Later;
            QVERIFY(w.window.closeProject());
        }
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);

        Window w(project.options());
        w.answers.recovery = RecoveryChoice::Later;
        QVERIFY(w.window.openProject(project.data.path()));
        QCOMPARE(w.answers.recoveryAsked.size(), 1);
        QVERIFY(w.window.session()->isReadOnly());
        QVERIFY(w.document().shapes().isEmpty());
        w.drawRectangle(QPointF(20, 70), QPointF(60, 95));
        QVERIFY(w.document().shapes().isEmpty());
        QVERIFY(!w.window.confirmCurrent());
        // Away and back: not asked again, still read-only, the record untouched.
        QVERIFY(w.window.nextImage());
        QVERIFY(w.window.previousImage());
        QCOMPARE(w.answers.recoveryAsked.size(), 1);
        QVERIFY(w.window.session()->isReadOnly());
        w.window.flushSaves();
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);

        // Deciding lifts it.
        w.answers.recovery = RecoveryChoice::Restore;
        w.window.resolveRecoveryRecords();
        QCOMPARE(w.answers.recoveryAsked.size(), 2);
        QCOMPARE(recoveryRecordCount(project.local.path()), 0);
        QVERIFY(!w.window.session()->isReadOnly());
        QCOMPARE(w.document().shapes().size(), 2);
    }

    // Item 5: while the recovery question for the next image is shown, the canvas no longer
    // refers to the previous image's document.
    void recoveryQuestionWhileSwitching()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.goToImage(1));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.window.saveNow();
        w.window.flushSaves();
        // Changed by another program, then edited and left: the edit waits in a record.
        writeFile(project.json(2), QByteArray("{}"));
        w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
        QVERIFY(w.window.goToImage(0));
        w.window.flushSaves();
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);

        bool checked = false;
        w.answers.recovery = RecoveryChoice::Later;
        w.answers.duringRecovery = [&] {
            QVERIFY(w.canvas()->document() == nullptr);
            QVERIFY(w.window.session() == nullptr);
            w.canvas()->repaint();
            checked = true;
        };
        QVERIFY(w.window.goToImage(1));
        QVERIFY(checked);
        QCOMPARE(w.canvas()->document(), &w.document());
    }

    // Item 6: deleting the shape being dragged ends the drag.
    void deleteWhileDragging()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        Canvas* canvas = w.canvas();
        const QPointF inside = canvas->imageToWidget(QPointF(50, 35));
        mouse(canvas, QEvent::MouseButtonPress, inside, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, inside + QPointF(5, 5), Qt::NoButton, Qt::LeftButton);
        QTest::keyClick(canvas, Qt::Key_Delete);
        QVERIFY(w.document().shapes().isEmpty());
        mouse(canvas, QEvent::MouseMove, inside + QPointF(15, 10), Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, inside + QPointF(15, 10), Qt::LeftButton, Qt::NoButton);
        QVERIFY(w.document().shapes().isEmpty());
        QCOMPARE(canvas->selectedShape(), qsizetype(-1));
    }

    // The shortcuts are X-AnyLabeling 4.2.0's defaults (anylabeling/configs/xanylabeling_config.yaml).
    void shortcutsMatchXAnyLabeling_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("keys");
        const QList<std::pair<const char*, const char*>> upstream = {
            {"open_dir", "Ctrl+U"},           {"save", "Ctrl+S"},
            {"quit", "Ctrl+Q"},               {"open_next", "D"},
            {"open_prev", "A"},               {"toggle_annotation_checked", "Ctrl+Alt+K"},
            {"zoom_in", "Ctrl++"},            {"zoom_out", "Ctrl+-"},
            {"zoom_to_original", "Ctrl+="},   {"fit_window", "Ctrl+F"},
            {"create_polygon", "P"},          {"create_rectangle", "R"},
            {"edit_polygon", "Ctrl+J"},       {"delete_polygon", "Delete"},
            {"undo", "Ctrl+Z"},               {"edit_label", "Ctrl+E"},
            {"open_next_unchecked", "Ctrl+Shift+D"}, {"open_prev_unchecked", "Ctrl+Shift+A"},
            {"copy_polygon", "Ctrl+C"},       {"paste_polygon", "Ctrl+V"},
            {"hide_selected_polygons", "S"},  {"show_hidden_polygons", "W"},
            {"toggle_visibility_shapes", "Ctrl+H"}, {"show_labels", "Ctrl+L"},
            {"create_rotation", "O"},         {"create_point", "Shift+P"},
            {"create_line", "Shift+L"},       {"create_linestrip", "Shift+S"},
            {"create_circle", "Shift+C"},
        };
        for (const auto& [action, keys] : upstream)
            QTest::newRow(action) << QString::fromLatin1(action) << QString::fromLatin1(keys);
    }

    void shortcutsMatchXAnyLabeling()
    {
        QFETCH(QString, action);
        QFETCH(QString, keys);
        Project project;
        Window w(project.options());
        const QAction* found = w.window.findChild<QAction*>(action);
        QVERIFY2(found, qPrintable(action));
        QCOMPARE(found->shortcuts(), QList<QKeySequence>{QKeySequence(keys)});
    }

    // No shortcut of this program may take a key X-AnyLabeling uses for something else.
    void noShortcutTakesAnotherUpstreamKey()
    {
        Project project;
        Window w(project.options());
        const QSet<QString> upstreamElsewhere = {QStringLiteral("Ctrl+Y"), QStringLiteral("Ctrl+O"),
                                                 QStringLiteral("Ctrl+I"), QStringLiteral("V")};
        for (const QAction* action : w.window.findChildren<QAction*>()) {
            for (const QKeySequence& keys : action->shortcuts())
                QVERIFY2(!upstreamElsewhere.contains(keys.toString()), qPrintable(action->text() + QLatin1Char(' ') + keys.toString()));
        }
    }

    // Ctrl+C / Ctrl+V copy the selected shape, also into the next image; S hides it, W shows
    // it again, Ctrl+H switches all shapes off and on; Ctrl+L the class names.
    void copyPasteAndHide()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        Canvas* canvas = w.canvas();
        canvas->setFocus();
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.drawRectangle(QPointF(100, 20), QPointF(150, 70));
        QCOMPARE(w.document().shapes().size(), 2);
        canvas->setSelectedShape(0);
        QTest::keyClick(canvas, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(canvas, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(w.document().shapes().size(), 3);
        QVERIFY(exactlyEqual(w.document().shapes()[2].points, w.document().shapes()[0].points));
        QCOMPARE(canvas->selectedShape(), qsizetype(2));
        QVERIFY(w.window.nextImage());
        QTest::keyClick(canvas, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(w.document().shapes().size(), 1);
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("scratch"));
        QVERIFY(w.window.previousImage());

        // S hides the selected shape: not drawn, not clickable, marked in the list.
        auto* list = w.window.findChild<QListWidget*>(QStringLiteral("shapeList"));
        canvas->setSelectedShape(1);
        QTest::keyClick(canvas, Qt::Key_S);
        QVERIFY(canvas->isHidden(1));
        QCOMPARE(canvas->selectedShape(), qsizetype(-1));
        QVERIFY(list->item(1)->text().contains(QStringLiteral(u"已隐藏")));
        click(canvas, QPointF(125, 45));
        QCOMPARE(canvas->selectedShape(), qsizetype(-1));
        canvas->setSelectedShape(0);
        QTest::keyClick(canvas, Qt::Key_S);
        // W shows the one hidden last.
        QTest::keyClick(canvas, Qt::Key_W);
        QVERIFY(!canvas->isHidden(0));
        QVERIFY(canvas->isHidden(1));
        QTest::keyClick(canvas, Qt::Key_W);
        QVERIFY(!canvas->isHidden(1));
        QVERIFY(!w.window.findChild<QListWidget*>(QStringLiteral("shapeList"))->item(1)->text().contains(QStringLiteral(u"已隐藏")));

        // Ctrl+H: all off, also on the next image, and on again.
        QTest::keyClick(canvas, Qt::Key_H, Qt::ControlModifier);
        QVERIFY(!canvas->shapesVisible());
        for (int i = 0; i < 3; ++i)
            QVERIFY(canvas->isHidden(i));
        QVERIFY(w.window.nextImage());
        QVERIFY(canvas->isHidden(0));
        // A shape drawn meanwhile is shown.
        w.drawRectangle(QPointF(30, 30), QPointF(60, 60));
        QVERIFY(!canvas->isHidden(1));
        QTest::keyClick(canvas, Qt::Key_H, Qt::ControlModifier);
        QVERIFY(!canvas->isHidden(0));

        // Ctrl+L: class names off and on; nothing changes in the file.
        QVERIFY(canvas->labelsVisible());
        QTest::keyClick(canvas, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(!canvas->labelsVisible());
        QTest::keyClick(canvas, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(canvas->labelsVisible());
    }

    // O draws a rotated box upright; Z/V turn it by 1°, X/C by 0.1°, as upstream; dragging a
    // corner keeps the angle. The file gets upstream's numbers.
    void rotatedBoxes()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        Canvas* canvas = w.canvas();
        canvas->setFocus();
        canvas->fitToWindow();
        QTest::keyClick(canvas, Qt::Key_O);
        QCOMPARE(canvas->tool(), Canvas::Tool::Rotation);
        drag(canvas, QPointF(40, 20), QPointF(120, 60));
        QCOMPARE(w.document().shapes().size(), 1);
        const Shape drawn = w.document().shapes()[0];
        QCOMPARE(drawn.type, ShapeType::Rotation);
        QCOMPARE(drawn.direction, 0.0);

        QTest::keyClick(canvas, Qt::Key_J, Qt::ControlModifier);
        canvas->setSelectedShape(0);
        QTest::keyClick(canvas, Qt::Key_Z);
        QTest::keyClick(canvas, Qt::Key_X);
        QTest::keyClick(canvas, Qt::Key_C);
        QTest::keyClick(canvas, Qt::Key_C);
        QTest::keyClick(canvas, Qt::Key_V);
        RotatedBox expected{drawn.points, 0};
        for (double degrees : {1.0, 0.1, -0.1, -0.1, -1.0})
            expected = rotateBox(expected, radians(degrees));
        const Shape turned = w.document().shapes()[0];
        QVERIFY(exactlyEqual(turned.points, expected.points));
        QCOMPARE(turned.direction, expected.direction);
        // Turned back by 0.1° in all: upstream's angle is then 0.1° in radians.
        QVERIFY(qAbs(turned.direction - radians(0.1)) < 1e-12);

        // Each turn is one undo step: Ctrl+Z takes back the last V.
        QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(qAbs(w.document().shapes()[0].direction - (2 * 3.141592653589793 - radians(0.9))) < 1e-12);

        // A corner moved with the mouse keeps the angle.
        canvas->setSelectedShape(0);
        const RotatedBox before{w.document().shapes()[0].points, w.document().shapes()[0].direction};
        drag(canvas, before.points[2], before.points[2] + QPointF(10, 5));
        const Shape moved = w.document().shapes()[0];
        QCOMPARE(moved.direction, before.direction);
        QVERIFY(!exactlyEqual(moved.points, before.points));
        QVERIFY(exactlyEqual(moved.points, moveBoxCorner(before, 2, moved.points[2])));

        w.window.saveNow();
        w.window.flushSaves();
        const QJsonObject shape = readJson(project.json(1))[QStringLiteral("shapes")].toArray()[0].toObject();
        QCOMPARE(shape[QStringLiteral("shape_type")].toString(), QStringLiteral("rotation"));
        QCOMPARE(shape[QStringLiteral("direction")].toDouble(), moved.direction);
    }

    // Points, lines, line strips and circles, with upstream's keys.
    void otherShapes()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        Canvas* canvas = w.canvas();
        canvas->setFocus();
        canvas->fitToWindow();

        QTest::keyClick(canvas, Qt::Key_P, Qt::ShiftModifier);
        QCOMPARE(canvas->tool(), Canvas::Tool::Point);
        click(canvas, QPointF(30, 40));
        QTest::keyClick(canvas, Qt::Key_L, Qt::ShiftModifier);
        QCOMPARE(canvas->tool(), Canvas::Tool::Line);
        drag(canvas, QPointF(10, 10), QPointF(90, 50));
        QTest::keyClick(canvas, Qt::Key_C, Qt::ShiftModifier);
        QCOMPARE(canvas->tool(), Canvas::Tool::Circle);
        drag(canvas, QPointF(150, 50), QPointF(170, 50));
        QTest::keyClick(canvas, Qt::Key_S, Qt::ShiftModifier);
        QCOMPARE(canvas->tool(), Canvas::Tool::LineStrip);
        click(canvas, QPointF(20, 80));
        click(canvas, QPointF(60, 90));
        QTest::keyClick(canvas, Qt::Key_Return);

        const QList<Shape>& shapes = w.document().shapes();
        QCOMPARE(shapes.size(), 4);
        QCOMPARE(shapes[0].type, ShapeType::Point);
        QCOMPARE(shapes[0].points.size(), 1);
        QVERIFY(qAbs(shapes[0].points[0].x() - 30) < 1);
        QCOMPARE(shapes[1].type, ShapeType::Line);
        QCOMPARE(shapes[1].points.size(), 2);
        QVERIFY(qAbs(shapes[1].points[1].y() - 50) < 1);
        QCOMPARE(shapes[2].type, ShapeType::Circle);
        QVERIFY(qAbs(shapes[2].points[0].x() - 150) < 1);
        QVERIFY(qAbs(shapes[2].points[1].x() - 170) < 1);
        QCOMPARE(shapes[3].type, ShapeType::LineStrip);
        QCOMPARE(shapes[3].points.size(), 2);
        // All can be confirmed.
        QVERIFY(w.window.confirmCurrent());

        // A point is selected and moved like any shape.
        QTest::keyClick(canvas, Qt::Key_J, Qt::ControlModifier);
        click(canvas, shapes[0].points[0]);
        QCOMPARE(canvas->selectedShape(), qsizetype(0));
        const QPointF before = shapes[0].points[0];
        QTest::keyClick(canvas, Qt::Key_Right);
        QCOMPARE(w.document().shapes()[0].points[0], before + QPointF(1, 0));
    }

    void keyboardAndMouseLikeXAnyLabeling()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        Canvas* canvas = w.canvas();
        canvas->setFocus();
        canvas->fitToWindow();

        // P draws polygons; Ctrl+Z while drawing takes back the last point.
        QTest::keyClick(canvas, Qt::Key_P);
        QCOMPARE(canvas->tool(), Canvas::Tool::Polygon);
        click(canvas, QPointF(10, 10));
        click(canvas, QPointF(90, 15));
        click(canvas, QPointF(150, 90));
        QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
        click(canvas, QPointF(50, 80));
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE(w.document().shapes().size(), 1);
        const PointList drawn = w.document().shapes()[0].points;
        QCOMPARE(drawn.size(), 3);
        QVERIFY(qAbs(drawn[2].x() - 50) < 1);

        // Ctrl+J selects; arrow keys move the selected shape one pixel.
        QTest::keyClick(canvas, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(canvas->tool(), Canvas::Tool::Select);
        canvas->setSelectedShape(0);
        QTest::keyClick(canvas, Qt::Key_Right);
        QTest::keyClick(canvas, Qt::Key_Down);
        QCOMPARE(w.document().shapes()[0].points[0], drawn[0] + QPointF(1, 1));
        // Not out of the image: the shape stays where it is.
        for (int i = 0; i < 20; ++i)
            QTest::keyClick(canvas, Qt::Key_Up);
        QCOMPARE(w.document().shapes()[0].points[0].y(), 0.0);

        // Ctrl+E changes the class.
        w.answers.label = QStringLiteral("dent");
        QTest::keyClick(canvas, Qt::Key_E, Qt::ControlModifier);
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("dent"));
        // Ctrl+Z undoes it, Ctrl+Shift+Z redoes it, Ctrl+Y does nothing.
        QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("scratch"));
        QTest::keyClick(canvas, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("scratch"));
        QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("dent"));

        // Ctrl+Alt+K confirms the image.
        QTest::keyClick(canvas, Qt::Key_K, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(w.document().review().checked);

        // The wheel scrolls, Ctrl + wheel zooms; Ctrl+= is the original size, Ctrl+F fits.
        const double fitted = canvas->zoom();
        const QPointF origin = canvas->imageToWidget(QPointF(0, 0));
        const QPointF middle(canvas->width() / 2.0, canvas->height() / 2.0);
        QWheelEvent scroll(middle, canvas->mapToGlobal(middle), QPoint(), QPoint(0, -120), Qt::NoButton,
                           Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &scroll);
        QCOMPARE(canvas->zoom(), fitted);
        QVERIFY(canvas->imageToWidget(QPointF(0, 0)).y() < origin.y());
        QWheelEvent zoom(middle, canvas->mapToGlobal(middle), QPoint(), QPoint(0, 120), Qt::NoButton,
                         Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &zoom);
        QVERIFY(qAbs(canvas->zoom() - fitted * 1.1) < 1e-9);
        QTest::keyClick(canvas, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(canvas->zoom(), 1.0);
        QTest::keyClick(canvas, Qt::Key_F, Qt::ControlModifier);
        QCOMPARE(canvas->zoom(), fitted);

        // Space + left drag pans and draws nothing, also with the rectangle tool.
        QTest::keyClick(canvas, Qt::Key_R);
        QCOMPARE(canvas->tool(), Canvas::Tool::Rectangle);
        const QPointF before = canvas->imageToWidget(QPointF(0, 0));
        QTest::keyPress(canvas, Qt::Key_Space);
        const QPointF from = canvas->imageToWidget(QPointF(100, 50));
        mouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, from + QPointF(30, 20), Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, from + QPointF(30, 20), Qt::LeftButton, Qt::NoButton);
        QTest::keyRelease(canvas, Qt::Key_Space);
        QCOMPARE(canvas->imageToWidget(QPointF(0, 0)), before + QPointF(30, 20));
        QCOMPARE(w.document().shapes().size(), 1);
    }

    // Review of 6f5f547: fitting can go beyond the zoom limits; zooming from there must not
    // turn the other way (a very wide image grew on Ctrl+-, a 1x1 image shrank on Ctrl++).
    void zoomNeverTurnsTheOtherWay_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("40000x100") << QSize(40000, 100);
        QTest::newRow("1x1") << QSize(1, 1);
        QTest::newRow("200x100") << QSize(200, 100);
    }

    // Architecture 6: the list and its statuses come from the catalog; saves update it, and
    // "重新扫描" finds files added or removed by other programs without losing the image shown.
    void catalogListAndRescan()
    {
        Project project;
        writeFile(project.json(2),
                  R"({"version": "2.4.0", "flags": {}, "shapes": [], "imagePath": "0002.png", "imageData": null, )"
                  R"("imageHeight": 100, "imageWidth": 200, "checked": true})");
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        auto* list = w.window.findChild<QListWidget*>(QStringLiteral("imageList"));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY2(list->item(1)->text().contains(QStringLiteral(u"[已确认]")), qPrintable(list->item(1)->text()));
        QVERIFY(list->item(2)->text().contains(QStringLiteral(u"[未处理]")));

        // A save of the first image goes into the catalog as written.
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.confirmCurrent());
        QVERIFY(w.window.goToImage(2));
        w.window.flushSaves();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        dataset::CatalogEntry first = *w.window.catalog()->reader().image(QStringLiteral("line1/0001.png"));
        QCOMPARE(first.status, AnnotationStatus::Confirmed);
        QVERIFY(first.verified);

        // Another program adds one image and removes another; the image shown stays shown.
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0000.png"))));
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0002.png"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QCOMPARE(w.window.imageCount(), 3);
        QCOMPARE(list->item(0)->text(), QStringLiteral(u"line1/0000.png  [未处理]"));
        QCOMPARE(w.window.currentIndex(), 2);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0003.png"));
        QVERIFY(list->item(1)->text().contains(QStringLiteral(u"[已确认]")));

        // The image shown is removed: it stays in the list until it is left.
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0003.png"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QCOMPARE(w.window.imageCount(), 3);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0003.png"));
        QVERIFY(w.window.goToImage(0));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0000.png"));
    }

    // An image that gets a name collision while shown becomes read-only: saving would write
    // the annotation file the other image shares (format spec 2).
    void nameCollisionFoundWhileShown()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QVERIFY(w.window.session()->isReadOnly());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.document().shapes().isEmpty());
        w.window.saveNow();
        w.window.flushSaves();
        QVERIFY(!QFile::exists(project.json(1)));

        // The other image goes away again: editable again.
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY(!w.window.session()->isReadOnly());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QCOMPARE(w.document().shapes().size(), 1);
    }

    // Edits not saved yet when the collision is found are neither written to the shared file
    // nor lost: they stay with the image, and closing asks before giving them up.
    void unsavedEditsSurviveANameCollision()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QCOMPARE(w.document().shapes().size(), 1);

        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY(w.window.session()->isReadOnly());

        // Away and back: the rectangle is still there, still not written. The list is now
        // 0001.jpg, 0001.png, 0002.png, 0003.png.
        QCOMPARE(w.window.currentIndex(), 1);
        QVERIFY(w.window.goToImage(3));
        w.window.flushSaves();
        QVERIFY(w.window.goToImage(1));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QCOMPARE(w.document().shapes().size(), 1);
        QVERIFY(w.window.session()->isReadOnly());
        w.window.flushSaves();
        QVERIFY(!QFile::exists(project.json(1)));

        // Closing asks; declined, the project stays open with the edits.
        w.answers.discardUnsaved = false;
        QVERIFY(!w.window.closeProject());
        QCOMPARE(w.answers.discardAsked, 1);
        QCOMPARE(w.document().shapes().size(), 1);

        // The other image is renamed away: the edits are saved after all.
        QVERIFY(QFile::rename(project.data.filePath(QStringLiteral("line1/0001.jpg")),
                              project.data.filePath(QStringLiteral("line1/0009.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY(!w.window.session()->isReadOnly());
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 1);
        QVERIFY(w.window.closeProject());
        QCOMPARE(w.answers.discardAsked, 1);
    }

    // Review of 9a46be8: the receipt of a save started before the collision was found comes
    // back after newer edits were held. It must not take those edits with it.
    void olderReceiptKeepsNewerHeldEdits()
    {
        Project project;
        MainWindow::Options options = project.options();
        auto written = std::make_shared<QSemaphore>();
        auto release = std::make_shared<QSemaphore>();
        auto hold = std::make_shared<std::atomic<bool>>(false);
        options.saveStageHook = [written, release, hold](application::SaveStage stage) {
            if (stage == application::SaveStage::AnnotationWritten && hold->exchange(false)) {
                written->release();
                release->tryAcquire(1, 20000);
            }
        };
        Window w(options);
        const auto unblock = qScopeGuard([&] { release->release(); });
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));

        // The first rectangle is written; its receipt is held back.
        hold->store(true);
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.window.saveNow();
        QVERIFY(written->tryAcquire(1, 10000));
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 1);

        // A second rectangle, then the same-name image appears: both are held.
        w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
        QCOMPARE(w.document().shapes().size(), 2);
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY(w.window.session()->isReadOnly());

        // The old save finishes; its receipt is handled while switching away.
        release->release();
        QVERIFY(w.window.saver()->waitUntilAllDone(10000));

        // Away and back: both rectangles, still not written; closing asks.
        QVERIFY(w.window.goToImage(3));
        w.window.flushSaves();
        QVERIFY(w.window.goToImage(1));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QCOMPARE(w.document().shapes().size(), 2);
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 1);
        w.answers.discardUnsaved = false;
        QVERIFY(!w.window.closeProject());
        QCOMPARE(w.answers.discardAsked, 1);

        // Once the other image is gone, the second rectangle is written too.
        QVERIFY(QFile::rename(project.data.filePath(QStringLiteral("line1/0001.jpg")),
                              project.data.filePath(QStringLiteral("line1/0009.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 2);
        QVERIFY(w.window.closeProject());
        QCOMPARE(w.answers.discardAsked, 1);
    }

    // A recovery record of an image that now has a same-name image is not restored: that
    // would write the annotation file the two share. It is offered once the collision is gone.
    void recoveryRecordWaitsForANameCollision()
    {
        Project project;
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
            w.window.saveNow();
            w.window.flushSaves();
            QJsonObject external = readJson(project.json(1));
            external.insert(QStringLiteral("shapes"), QJsonArray());
            writeFile(project.json(1), QJsonDocument(external).toJson());
            w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
            w.answers.externalChange = ExternalChangeChoice::Later;
            QVERIFY(w.window.closeProject());
        }
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);
        const QByteArray external = readFile(project.json(1));
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0001.jpg"))));

        Window w(project.options());
        w.answers.recovery = RecoveryChoice::Restore;
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        // The list is now 0001.jpg, 0001.png, 0002.png, 0003.png.
        QVERIFY(w.window.goToImage(1));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QVERIFY(w.answers.recoveryAsked.isEmpty());
        QVERIFY(w.window.session()->isReadOnly());
        w.window.resolveRecoveryRecords();
        QVERIFY(w.answers.recoveryAsked.isEmpty());
        QCOMPARE(readFile(project.json(1)), external);
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);

        // The other image is renamed away while this one is shown: the record is offered now.
        QVERIFY(QFile::rename(project.data.filePath(QStringLiteral("line1/0001.jpg")),
                              project.data.filePath(QStringLiteral("line1/0009.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QTRY_COMPARE(w.answers.recoveryAsked.size(), 1);
        QCOMPARE(recoveryRecordCount(project.local.path()), 0);
        QVERIFY(!w.window.session()->isReadOnly());
        QCOMPARE(w.document().shapes().size(), 2);
    }

    // Review of ed16295, item 1: the other image is deleted, so the list gets shorter. While
    // the recovery question is shown, the status check finishes and the list is refreshed;
    // the list and the images behind it must already agree.
    void recoveryQuestionAfterTheListShrinks()
    {
        Project project;
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
            w.window.saveNow();
            w.window.flushSaves();
            QJsonObject external = readJson(project.json(1));
            external.insert(QStringLiteral("shapes"), QJsonArray());
            writeFile(project.json(1), QJsonDocument(external).toJson());
            w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
            w.answers.externalChange = ExternalChangeChoice::Later;
            QVERIFY(w.window.closeProject());
        }
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0001.jpg"))));

        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY(w.window.goToImage(1));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QVERIFY(w.answers.recoveryAsked.isEmpty());

        auto* list = w.window.findChild<QListWidget*>();
        QVERIFY(list);
        int listed = -1;
        w.answers.recovery = RecoveryChoice::Restore;
        w.answers.duringRecovery = [&] {
            listed = list->count();
            QVERIFY(w.window.catalog()->waitUntilIdle(10000));
            QCoreApplication::processEvents();
        };
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QTRY_COMPARE(w.answers.recoveryAsked.size(), 1);
        QCOMPARE(listed, 3);
        QCOMPARE(w.window.imageCount(), 3);
        QCOMPARE(list->count(), 3);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QCOMPARE(w.document().shapes().size(), 2);
        QVERIFY(!w.window.session()->isReadOnly());
    }

    // Review of ed16295, item 2: an older recovery record is restored while newer edits are
    // held. The file the program just wrote is what the held edits replace, not an external
    // change.
    void heldEditsAfterRestoringAnOlderRecord()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));

        // The first save meets an external change; the edit goes to a recovery record.
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        writeFile(project.json(1), QByteArray(R"({"version": "2.4.0", "flags": {}, "shapes": [], "imagePath": "0001.png", )"
                                              R"("imageData": null, "imageHeight": 100, "imageWidth": 200})"));
        w.answers.externalChange = ExternalChangeChoice::Later;
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(w.answers.externalChangeAsked, 1);
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);

        // A second rectangle, then a same-name image: both rectangles are held.
        w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY(w.window.session()->isReadOnly());
        QVERIFY(w.window.goToImage(3));
        w.window.flushSaves();

        // The collision is gone; the older record is restored from the menu.
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        w.answers.recovery = RecoveryChoice::Restore;
        w.window.resolveRecoveryRecords();
        QCOMPARE(w.answers.recoveryAsked.size(), 1);
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 1);

        // The held rectangles are saved over it without an external-change question.
        QVERIFY(w.window.goToImage(0));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QCOMPARE(w.document().shapes().size(), 2);
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(w.answers.externalChangeAsked, 1);
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 2);
        QVERIFY(w.window.closeProject());
        QCOMPARE(w.answers.discardAsked, 0);
    }

    // Review of 30af45f: switching images shows "修改没有写入" for an earlier failed save. While
    // it is shown, a rescan finds the second image deleted and the list gets shorter. The
    // switch goes to the image that was asked for, found by its path.
    void listShrinksDuringAMessageWhileSwitching()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        blockRecoveryFolder(project.local.path());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.window.saveNow();
        QVERIFY(w.window.saver()->waitUntilAllDone(10000));  // its result is not handled yet

        auto* list = w.window.findChild<QListWidget*>();
        QVERIFY(list);
        w.answers.duringMessage = [&] {
            QVERIFY(w.window.catalog()->waitUntilIdle(10000));
            QCoreApplication::processEvents();
        };
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0002.png"))));
        w.window.rescan();
        QVERIFY(w.window.goToImage(2));
        QCOMPARE(w.answers.messages.size(), 1);
        QCOMPARE(w.window.imageCount(), 2);
        QCOMPARE(list->count(), 2);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0003.png"));
        QCOMPARE(w.window.currentIndex(), 1);
        QCOMPARE(list->currentRow(), 1);
        unblockRecoveryFolder(project.local.path());
    }

    // The image asked for is deleted while the message is shown: it is handled like an image
    // deleted while shown, and the list and the image shown still agree.
    void targetDeletedDuringAMessageWhileSwitching()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        blockRecoveryFolder(project.local.path());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.window.saveNow();
        QVERIFY(w.window.saver()->waitUntilAllDone(10000));

        auto* list = w.window.findChild<QListWidget*>();
        w.answers.duringMessage = [&] {
            QVERIFY(w.window.catalog()->waitUntilIdle(10000));
            QCoreApplication::processEvents();
        };
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0003.png"))));
        w.window.rescan();
        w.window.goToImage(2);
        QCOMPARE(w.answers.messages.size(), 1);
        QCOMPARE(list->count(), w.window.imageCount());
        QVERIFY(w.window.currentIndex() >= 0 && w.window.currentIndex() < w.window.imageCount());
        QCOMPARE(list->currentRow(), w.window.currentIndex());
        QCOMPARE(list->currentItem()->text(), QStringLiteral(u"line1/0003.png  [缺失]"));
        QVERIFY(!w.window.session());
        // The other images are still there to go to.
        QVERIFY(w.window.goToImage(0));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QCOMPARE(w.document().shapes().size(), 1);
        unblockRecoveryFolder(project.local.path());
    }

    // Review of 80932e9, item 1: while the external-change question is shown, a scan finds a
    // same-name image. "用我的版本覆盖" must not write the file the two images share.
    void overwriteAfterACollisionFoundDuringTheQuestion()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        const QByteArray external = R"({"version": "2.4.0", "flags": {}, "shapes": [], "imagePath": "0001.png", )"
                                    R"("imageData": null, "imageHeight": 100, "imageWidth": 200})";
        writeFile(project.json(1), external);
        w.answers.externalChange = ExternalChangeChoice::Overwrite;
        w.answers.duringExternalChange = [&] {
            QImage image(200, 100, QImage::Format_RGB32);
            image.fill(Qt::gray);
            QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
            w.window.rescan();
            QVERIFY(w.window.catalog()->waitUntilIdle(10000));
            QCoreApplication::processEvents();
        };
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(w.answers.externalChangeAsked, 1);
        QCOMPARE(readFile(project.json(1)), external);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QVERIFY(w.window.session()->isReadOnly());
        QCOMPARE(w.document().shapes().size(), 1);
        w.answers.discardUnsaved = false;
        QVERIFY(!w.window.closeProject());
        QCOMPARE(readFile(project.json(1)), external);
    }

    // The same for a recovery record restored at open: the same-name image appears while
    // the question is shown.
    void restoreAfterACollisionAppearsDuringTheQuestion()
    {
        Project project;
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
            w.window.saveNow();
            w.window.flushSaves();
            QJsonObject external = readJson(project.json(1));
            external.insert(QStringLiteral("shapes"), QJsonArray());
            writeFile(project.json(1), QJsonDocument(external).toJson());
            w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
            w.answers.externalChange = ExternalChangeChoice::Later;
            QVERIFY(w.window.closeProject());
        }
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);
        const QByteArray external = readFile(project.json(1));

        Window w(project.options());
        w.answers.recovery = RecoveryChoice::Restore;
        w.answers.duringRecovery = [&] {
            QImage image(200, 100, QImage::Format_RGB32);
            image.fill(Qt::gray);
            QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        };
        QVERIFY(w.window.openProject(project.data.path()));
        QCOMPARE(w.answers.recoveryAsked.size(), 1);
        QCOMPARE(readFile(project.json(1)), external);
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);
    }

    // The last check is in the save itself: a same-name image that appears after the save
    // was handed over keeps it from writing the shared file. The edits are not lost.
    void collisionAppearsWhileSaving()
    {
        Project project;
        MainWindow::Options options = project.options();
        QString jpg;
        options.saveStageHook = [&jpg](application::SaveStage stage) {
            if (stage == application::SaveStage::RecoveryRecordWritten && !jpg.isEmpty()) {
                QImage image(200, 100, QImage::Format_RGB32);
                image.fill(Qt::gray);
                image.save(std::exchange(jpg, QString()));
            }
        };
        Window w(options);
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        jpg = project.data.filePath(QStringLiteral("line1/0001.jpg"));
        w.window.saveNow();
        w.window.flushSaves();
        QVERIFY(!QFile::exists(project.json(1)));
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);
        QCOMPARE(w.document().shapes().size(), 1);

        // The other image goes away: the edits on screen are saved, which replaces the
        // record, and nothing is held back any more.
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0001.jpg"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QVERIFY(!w.window.session()->isReadOnly());
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 1);
        QCOMPARE(recoveryRecordCount(project.local.path()), 0);
        QVERIFY(w.window.goToImage(1));
        QVERIFY(w.window.goToImage(0));
        QCoreApplication::processEvents();
        QVERIFY(!w.window.session()->isReadOnly());
        QCOMPARE(w.answers.recoveryAsked.size(), 0);
    }

    // Review of 80932e9, item 2: the listing is kept aside while a question is shown, and
    // the status check finishes meanwhile. Applying the kept listing must not drop the
    // statuses the check found.
    void statusesFoundDuringAQuestionStay()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        writeFile(project.json(2), R"({"version": "2.4.0", "flags": {}, "checked": true, "shapes": [], )"
                                   R"("imagePath": "0002.png", "imageData": null, "imageWidth": 200, "imageHeight": 100})");
        QSemaphore listed, checkMayStart;
        const QMetaObject::Connection connection = QObject::connect(
            w.window.catalog(), &application::ProjectCatalog::scanFinished, w.window.catalog(),
            [&](bool) {
                listed.release();
                checkMayStart.tryAcquire(1, 20000);
            },
            Qt::DirectConnection);
        auto* list = w.window.findChild<QListWidget*>();
        QString during;
        w.answers.duringLabel = [&] {
            QVERIFY(listed.tryAcquire(1, 10000));
            QCoreApplication::processEvents();  // the listing arrives and is kept aside
            QObject::disconnect(connection);
            checkMayStart.release();
            QVERIFY(w.window.catalog()->waitUntilIdle(10000));
            QCoreApplication::processEvents();  // the check finishes
            during = list->item(1)->text();
        };
        w.window.rescan();
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(during.contains(QStringLiteral("0002.png  [")));
        QCOMPARE(list->item(1)->text(), during);
    }

    // Review of 1f6ee17: "处理未写入的编辑…" meets a same-name image during the question, so the
    // record is not restored; while that message is shown the other image goes away again.
    // The record is still not handled, so the image must stay read-only until it is; then it
    // is asked about again.
    void recordStaysProtectedWhenTheCollisionGoesAway()
    {
        Project project;
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
            w.window.saveNow();
            w.window.flushSaves();
            QJsonObject external = readJson(project.json(1));
            external.insert(QStringLiteral("shapes"), QJsonArray());
            writeFile(project.json(1), QJsonDocument(external).toJson());
            w.drawRectangle(QPointF(100, 10), QPointF(150, 60));
            w.answers.externalChange = ExternalChangeChoice::Later;
            QVERIFY(w.window.closeProject());
        }
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);

        Window w(project.options());
        w.answers.recovery = RecoveryChoice::Later;
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0001.png"));
        QVERIFY(w.window.session()->isReadOnly());

        const QString jpg = project.data.filePath(QStringLiteral("line1/0001.jpg"));
        w.answers.recovery = RecoveryChoice::Restore;
        w.answers.duringRecovery = [&] {
            QImage image(200, 100, QImage::Format_RGB32);
            image.fill(Qt::gray);
            QVERIFY(image.save(jpg));
        };
        w.answers.duringMessage = [&] {
            w.answers.duringMessage = nullptr;
            w.answers.duringRecovery = nullptr;
            QVERIFY(QFile::remove(jpg));
            w.window.rescan();
            QVERIFY(w.window.catalog()->waitUntilIdle(10000));
            QCoreApplication::processEvents();
        };
        w.window.resolveRecoveryRecords();
        QCOMPARE(w.answers.recoveryAsked.size(), 2);
        QCOMPARE(w.answers.messages.size(), 1);
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);

        // Still protected: a new rectangle is refused.
        QVERIFY(w.window.session()->isReadOnly());
        w.drawRectangle(QPointF(20, 70), QPointF(60, 95));
        w.window.saveNow();
        w.window.flushSaves();
        QCOMPARE(recoveryRecordCount(project.local.path()), 1);

        // Asked again now that nothing holds the record back; restored.
        QTRY_COMPARE(w.answers.recoveryAsked.size(), 3);
        QCOMPARE(recoveryRecordCount(project.local.path()), 0);
        QVERIFY(!w.window.session()->isReadOnly());
        QCOMPARE(w.document().shapes().size(), 2);
        QCOMPARE(readJson(project.json(1)).value(QStringLiteral("shapes")).toArray().size(), 2);
    }

    // Without any catalog (no cache folder, no temporary folder) the folder is still listed
    // and the images can be labelled.
    // Architecture 7: "开始下一批" gives out the next images in natural order; the batch and
    // the image left are where the next opening continues.
    void batchesAreGivenOutAndResumed()
    {
        Project project("\"batchSize\": 2", 5);
        auto paths = [](MainWindow& window) {
            QStringList shown;
            auto* list = window.findChild<QListWidget*>(QStringLiteral("imageList"));
            for (int i = 0; i < list->count(); ++i)
                shown.append(list->item(i)->text().section(QLatin1Char(' '), 0, 0));
            return shown;
        };
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            QVERIFY(w.window.currentBatch().isEmpty());
            QCOMPARE(w.window.imageCount(), 5);
            QCOMPARE(w.window.batchStateText(), QStringLiteral(u"全部图片：5 张"));

            QVERIFY(w.window.startNextBatch());
            QCOMPARE(w.window.currentBatch(), QStringLiteral("b000001"));
            QCOMPARE(paths(w.window), (QStringList{QStringLiteral("line1/0001.png"), QStringLiteral("line1/0002.png")}));
            QCOMPARE(w.window.currentIndex(), 0);
            QCOMPARE(w.window.batchStateText(), QStringLiteral(u"第 1 批：已完成 0 / 2 张"));
            // A and D stay inside the batch.
            QVERIFY(w.window.nextImage());
            QVERIFY(!w.window.nextImage());
            QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0002.png"));

            QVERIFY(w.window.startNextBatch());
            QCOMPARE(w.window.currentBatch(), QStringLiteral("b000002"));
            QCOMPARE(paths(w.window), (QStringList{QStringLiteral("line1/0003.png"), QStringLiteral("line1/0004.png")}));
            QVERIFY(w.window.nextImage());
            QVERIFY(w.window.closeProject());
        }
        const QString worksets = project.data.filePath(QStringLiteral("liteanno/worksets"));
        QVERIFY(QFile::exists(worksets + QStringLiteral("/b000001.list.json")));
        const QJsonObject progress = readJson(worksets + QStringLiteral("/b000002.progress.json"));
        QCOMPARE(progress.value(QStringLiteral("lastItem")).toString(), QStringLiteral("line1/0004.png"));
        QCOMPARE(progress.value(QStringLiteral("events")).toArray().first().toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("started"));

        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QCOMPARE(w.window.currentBatch(), QStringLiteral("b000002"));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0004.png"));

        // Back to batch 1 where it was left, all images, and back again.
        w.answers.batch = 0;
        QVERIFY(w.window.chooseBatch());
        QCOMPARE(w.answers.batchesOffered,
                 (QStringList{QStringLiteral(u"第 1 批（已完成 0 / 2 张）"), QStringLiteral(u"第 2 批（已完成 0 / 2 张）")}));
        QCOMPARE(w.window.currentBatch(), QStringLiteral("b000001"));
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0002.png"));
        w.window.showAllImages();
        QVERIFY(w.window.currentBatch().isEmpty());
        QCOMPARE(w.window.imageCount(), 5);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0002.png"));
        QVERIFY(w.window.continueLastBatch());
        QCOMPARE(w.window.currentBatch(), QStringLiteral("b000001"));

        QVERIFY(w.window.startNextBatch());
        QCOMPARE(paths(w.window), QStringList{QStringLiteral("line1/0005.png")});
        QVERIFY(!w.window.startNextBatch());
        QCOMPARE(w.answers.messages.size(), 1);
        QVERIFY(w.answers.messages.first().contains(QStringLiteral(u"所有图片都已经分到批次里")));
        QCOMPARE(w.window.currentBatch(), QStringLiteral("b000003"));
    }

    // Format spec 5.4: an image counts once its done status is written; Ctrl+Shift+D/A go to
    // the ones that are not.
    void unfinishedImagesOfABatch()
    {
        Project project("\"batchSize\": 3", 4);
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QVERIFY(w.window.startNextBatch());
        QCOMPARE(w.window.showUnfinished(), 3);
        QCOMPARE(w.window.currentIndex(), 0);

        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.confirmCurrent());
        // Not written yet: not done.
        QCOMPARE(w.window.batchStateText(), QStringLiteral(u"第 1 批：已完成 0 / 3 张"));
        w.window.flushSaves();
        QCOMPARE(w.window.batchStateText(), QStringLiteral(u"第 1 批：已完成 1 / 3 张"));

        QVERIFY(w.window.goToImage(2));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.confirmCurrent());
        w.window.flushSaves();
        QVERIFY(w.window.previousUnfinished());
        QCOMPARE(w.window.currentIndex(), 1);
        QVERIFY(!w.window.nextUnfinished());
        QVERIFY(!w.window.previousUnfinished());
        QCOMPARE(w.window.currentIndex(), 1);
        // The shortcut does the same.
        QVERIFY(w.window.goToImage(0));
        QTest::keyClick(&w.window, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(w.window.currentIndex(), 1);
        QVERIFY(w.window.goToImage(2));
        QTest::keyClick(&w.window, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(w.window.currentIndex(), 1);

        const QString progressFile = project.data.filePath(QStringLiteral("liteanno/worksets/b000001.progress.json"));
        QCOMPARE(readJson(progressFile).value(QStringLiteral("events")).toArray().size(), 1);
        QVERIFY(w.window.skipCurrent());
        w.window.flushSaves();
        QCOMPARE(w.window.batchStateText(), QStringLiteral(u"第 1 批：已完成 3 / 3 张（本批已完成）"));
        QCOMPARE(w.window.showUnfinished(), 0);
        // The image outside the batch is not counted.
        w.window.showAllImages();
        QCOMPARE(w.window.showUnfinished(), 1);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0004.png"));
    }

    // Format spec 7.2: the batch being complete is recorded once, as history.
    void completedBatchIsRecorded()
    {
        Project project("\"batchSize\": 1", 2);
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.startNextBatch());
        QVERIFY(w.window.skipCurrent());
        w.window.flushSaves();
        QVERIFY(w.window.closeProject());
        const QJsonArray events =
            readJson(project.data.filePath(QStringLiteral("liteanno/worksets/b000001.progress.json")))
                .value(QStringLiteral("events"))
                .toArray();
        QCOMPARE(events.size(), 2);
        QCOMPARE(events[1].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("completed"));
    }

    // A batch keeps its images: one deleted later is shown as missing and is not done; images
    // with a name collision are not given out (format spec 7.1).
    void batchMembersStayFixed()
    {
        Project project("\"batchSize\": 3", 4);
        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0002.jpg"))));
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.startNextBatch());
        auto* list = w.window.findChild<QListWidget*>(QStringLiteral("imageList"));
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->item(0)->text().section(QLatin1Char(' '), 0, 0), QStringLiteral("line1/0001.png"));
        QCOMPARE(list->item(1)->text().section(QLatin1Char(' '), 0, 0), QStringLiteral("line1/0003.png"));
        QCOMPARE(list->item(2)->text().section(QLatin1Char(' '), 0, 0), QStringLiteral("line1/0004.png"));

        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0003.png"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QCOMPARE(list->count(), 3);
        QVERIFY2(list->item(1)->text().contains(QStringLiteral(u"[缺失]")), qPrintable(list->item(1)->text()));
        QCOMPARE(w.window.showUnfinished(), 3);
        QVERIFY(w.window.goToImage(1));
        QVERIFY(!w.window.session());
        // The deleted image is not given out again, nor the colliding ones.
        QVERIFY(!w.window.startNextBatch());
    }

    // A damaged list is not opened or repaired, and its images are not given out again.
    void exportFromTheWindow()
    {
        Project project("\"batchSize\": 2", 3);
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.startNextBatch());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.confirmCurrent());
        QVERIFY(w.window.nextImage());
        QVERIFY(w.window.confirmCurrent());  // no objects: a negative sample
        // The edits are still unwritten; exporting writes them first.

        QTemporaryDir out;
        w.answers.exportChoice.reset();
        QVERIFY(!w.window.exportAnnotations());
        QCOMPARE(w.answers.exportBatchOffered, 1);
        QVERIFY(w.answers.messages.isEmpty());

        ExportChoice choice;
        choice.format = formats::ExportFormat::YoloDetection;
        choice.wholeProject = true;
        choice.outputDir = out.filePath(QStringLiteral("det"));
        w.answers.exportChoice = choice;
        QVERIFY(w.window.exportAnnotations());
        QCOMPARE(w.answers.messages.size(), 1);
        QVERIFY2(w.answers.messages[0].startsWith(QStringLiteral(u"导出完成")), qPrintable(w.answers.messages[0]));
        QVERIFY2(w.answers.messages[0].contains(QStringLiteral(u"已导出 2 张图片（其中无目标 1 张）")),
                 qPrintable(w.answers.messages[0]));
        QVERIFY2(w.answers.messages[0].contains(QStringLiteral(u"未完成 1 张")), qPrintable(w.answers.messages[0]));
        QCOMPARE(readFile(out.filePath(QStringLiteral("det/line1/0001.txt"))).count('\n'), 1);
        QVERIFY(QFile::exists(out.filePath(QStringLiteral("det/line1/0002.txt"))));
        QVERIFY(!QFile::exists(out.filePath(QStringLiteral("det/line1/0003.txt"))));
        QVERIFY(QFile::exists(out.filePath(QStringLiteral("det/liteanno-export.json"))));

        // A rectangle cannot go into a segmentation format: the export stops and asks.
        choice.format = formats::ExportFormat::YoloSegmentation;
        choice.wholeProject = false;
        choice.outputDir = out.filePath(QStringLiteral("seg"));
        w.answers.exportChoice = choice;
        w.answers.skipUnconvertible = false;
        QVERIFY(!w.window.exportAnnotations());
        QCOMPARE(w.answers.unconvertibleTexts.size(), 1);
        QVERIFY2(w.answers.unconvertibleTexts[0].contains(QStringLiteral("line1/0001.png")),
                 qPrintable(w.answers.unconvertibleTexts[0]));
        QVERIFY(QDir(out.filePath(QStringLiteral("seg"))).isEmpty());
        QCOMPARE(w.answers.messages.size(), 1);

        w.answers.skipUnconvertible = true;
        QVERIFY(w.window.exportAnnotations());
        QCOMPARE(w.answers.unconvertibleTexts.size(), 2);
        QVERIFY2(w.answers.messages.last().contains(QStringLiteral(u"无法转换 1 张")), qPrintable(w.answers.messages.last()));
        QVERIFY(!QFile::exists(out.filePath(QStringLiteral("seg/line1/0001.txt"))));
        QVERIFY(QFile::exists(out.filePath(QStringLiteral("seg/line1/0002.txt"))));
        QVERIFY(QFile::exists(out.filePath(QStringLiteral("seg/liteanno-export.json"))));
    }

    void damagedListIsNotUsed()
    {
        Project project("\"batchSize\": 2", 3);
        writeFile(project.data.filePath(QStringLiteral("liteanno/worksets/b000001.list.json")),
                  R"({"formatVersion": "1.0", "projectId": "other", "batchId": "x", "number": 1, )"
                  R"("createdAt": "t", "items": ["line1/0001.png"], "itemsDigest": "sha256:0"})");
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.currentBatch().isEmpty());
        QVERIFY(w.window.statusBar()->currentMessage().contains(QStringLiteral(u"已损坏")));
        QVERIFY(w.window.startNextBatch());
        QCOMPARE(w.window.currentBatch(), QStringLiteral("b000002"));
        QCOMPARE(w.window.imageCount(), 2);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0002.png"));
    }

    void noCatalogAtAll()
    {
        Project project;
        writeFile(project.local.filePath(QStringLiteral("cache")), "not a folder");
        QTemporaryDir other;
        const QString blockedTemp = other.filePath(QStringLiteral("temp-is-a-file"));
        writeFile(blockedTemp, "not a folder");
        const QByteArray names[] = {"TMPDIR", "TMP", "TEMP"};
        QList<QByteArray> saved;
        for (const QByteArray& name : names) {
            saved.append(qgetenv(name.constData()));
            qputenv(name.constData(), QFile::encodeName(blockedTemp));
        }
        const auto restore = qScopeGuard([&] {
            for (int i = 0; i < 3; ++i) {
                if (saved[i].isEmpty())
                    qunsetenv(names[i].constData());
                else
                    qputenv(names[i].constData(), saved[i]);
            }
        });
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(!w.window.catalog());
        QCOMPARE(w.window.imageCount(), 3);
        QVERIFY(w.window.session());
        QVERIFY(w.window.findChild<QLabel*>(QStringLiteral("scanState"))->text().isEmpty());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.nextImage());
        w.window.flushSaves();
        QVERIFY(QFile::exists(project.json(1)));

        QImage image(200, 100, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(project.data.filePath(QStringLiteral("line1/0004.png"))));
        w.window.rescan();
        QCOMPARE(w.window.imageCount(), 4);
        QCOMPARE(w.window.session()->imagePath(), QStringLiteral("line1/0002.png"));

        // A complete listing of the folder is enough to give out a batch.
        QVERIFY(w.window.startNextBatch());
        QCOMPARE(w.window.currentBatch(), QStringLiteral("b000001"));
        QCOMPARE(w.window.imageCount(), 4);
    }

    // Opening again shows what the catalog knows before anything is read again.
    void reopenedProjectUsesTheCatalog()
    {
        Project project;
        {
            Window w(project.options());
            QVERIFY(w.window.openProject(project.data.path()));
            w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
            QVERIFY(w.window.confirmCurrent());
            QVERIFY(w.window.closeProject());
        }
        MainWindow::Options options = project.options();
        options.waitForScan = false;
        Window w(options);
        QVERIFY(w.window.openProject(project.data.path()));
        auto* list = w.window.findChild<QListWidget*>(QStringLiteral("imageList"));
        QCOMPARE(w.window.imageCount(), 3);
        QVERIFY(list->item(0)->text().contains(QStringLiteral(u"[已确认]")));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
    }

    void zoomNeverTurnsTheOtherWay()
    {
        QFETCH(QSize, size);
        Canvas canvas;
        canvas.resize(660, 480);
        QImage image(size, QImage::Format_RGB32);
        image.fill(Qt::gray);
        canvas.setImage(image);
        canvas.fitToWindow();
        const double fitted = canvas.zoom();
        const QPointF middle(canvas.width() / 2.0, canvas.height() / 2.0);
        const auto wheel = [&](int delta) {
            QWheelEvent event(middle, canvas.mapToGlobal(middle), QPoint(), QPoint(0, delta), Qt::NoButton,
                              Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(&canvas, &event);
        };
        for (const auto& [zoom, factor] : QList<std::pair<std::function<void()>, double>>{
                 {[&] { canvas.zoomBy(1.1); }, 1.1},
                 {[&] { canvas.zoomBy(1 / 1.1); }, 1 / 1.1},
                 {[&] { wheel(120); }, 1.1},
                 {[&] { wheel(-120); }, 1 / 1.1}}) {
            canvas.fitToWindow();
            for (int i = 0; i < 3; ++i) {
                const double before = canvas.zoom();
                zoom();
                if (factor > 1)
                    QVERIFY2(canvas.zoom() >= before, qPrintable(QString::number(before) + QLatin1String(" -> ") + QString::number(canvas.zoom())));
                else
                    QVERIFY2(canvas.zoom() <= before, qPrintable(QString::number(before) + QLatin1String(" -> ") + QString::number(canvas.zoom())));
            }
        }
        // Within the limits a step still zooms.
        canvas.fitToWindow();
        canvas.zoomBy(fitted > 1 ? 1 / 1.1 : 1.1);
        QVERIFY(canvas.zoom() != fitted);
    }
};

QTEST_MAIN(TestMainWindow)
#include "tst_mainwindow.moc"
