// The editing window driven like a user: drawing with the mouse, undo and redo, the review
// buttons, background saves and the prompts for external changes and recovery records.
// Runs offscreen; the prompts are replaced so nothing modal blocks.

#include "application/LocalDataPaths.h"
#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

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

    int externalChangeAsked = 0;
    QList<application::RecoveryState> recoveryAsked;
    QStringList messages;

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
            return recovery;
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
};

QTEST_MAIN(TestMainWindow)
#include "tst_mainwindow.moc"
