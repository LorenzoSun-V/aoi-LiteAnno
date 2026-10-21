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
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QWheelEvent>

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
    // Runs while the recovery question is shown, like a repaint behind a dialog.
    std::function<void()> duringRecovery;

    int externalChangeAsked = 0;
    QList<application::RecoveryState> recoveryAsked;
    QStringList messages;
    int discardAsked = 0;

    Prompts prompts()
    {
        Prompts p;
        p.chooseLabel = [this](const QStringList&, const QString&) -> std::optional<QString> {
            if (label.isEmpty())
                return std::nullopt;
            return label;
        };
        p.skipReason = [this]() -> std::optional<QString> { return skipReason; };
        p.externalChange = [this](const QString&) {
            ++externalChangeAsked;
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
        p.message = [this](const QString& title, const QString& text) { messages.append(title + QLatin1Char('\n') + text); };
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

    // Without any catalog (no cache folder, no temporary folder) the folder is still listed
    // and the images can be labelled.
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
