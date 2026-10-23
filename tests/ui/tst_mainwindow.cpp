// The editing window driven like a user: drawing with the mouse, undo and redo, the review
// buttons, background saves and the prompts for external changes and recovery records.
// Runs offscreen; the prompts are replaced so nothing modal blocks.

#include "application/LocalDataPaths.h"
#include "application/ProjectSession.h"
#include "application/RecoveryService.h"
#include "platform/VolumeBinding.h"
#include "ui/MainWindow.h"
#include "ui/Prompts.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <QThread>
#include <QToolButton>
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

    QStringList unusableTexts;
    bool setAsideUnusable = false;
    QStringList labelsOffered;
    bool labelFreeText = false;
    ExportQuestion exportQuestion;
    int externalChangeAsked = 0;
    QList<application::RecoveryState> recoveryAsked;
    QStringList messages;
    int discardAsked = 0;

    Prompts prompts()
    {
        Prompts p;
        p.chooseLabel = [this](const QStringList& classes, const QString&, bool freeText) -> std::optional<QString> {
            labelsOffered = classes;
            labelFreeText = freeText;
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
        p.exportOptions = [this](const ExportQuestion& question) {
            exportBatchOffered = question.batchNumber;
            exportQuestion = question;
            return exportChoice;
        };
        p.skipUnconvertible = [this](const QString& text) {
            unconvertibleTexts.append(text);
            return skipUnconvertible;
        };
        p.setAsideUnusable = [this](const QString& text) {
            unusableTexts.append(text);
            return setAsideUnusable;
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

    void writeProjectFile(const QByteArray& extra, const QByteArray& shapeTypes = R"("rectangle", "polygon")")
    {
        QByteArray text = QByteArray("{\"formatVersion\": \"1.0\", \"projectId\": \"") + kProjectId
                          + "\", \"shapeTypes\": [" + shapeTypes + "], "
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
    }

    // "帮助 > 关于": the source and the Qt licence notice (LGPL 3.0, section 4).
    void aboutNamesTheSourceAndTheLicences()
    {
        Window w(Project().options());
        QStringList shown;
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            for (const QLabel* label : dialog->findChildren<QLabel*>())
                shown.append(label->text());
            dialog->reject();
        });
        answer.start(20);
        w.window.showAbout();
        const QString text = shown.join(QLatin1Char('\n'));
        QVERIFY2(text.contains(QStringLiteral("https://github.com/LorenzoSun-V/aoi-LiteAnno")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("LGPL 3.0")), qPrintable(text));
    }

    // Review of #11, round 2: "处理未写入的编辑" also deals with records the program cannot
    // use, which liteanno-cli init otherwise waits for forever.
    void unusableRecordsCanBeMovedAside()
    {
        Project project;
        QDir(project.data.filePath(QStringLiteral("liteanno"))).removeRecursively();
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        const application::LocalDataPaths paths(project.local.path());
        const QString root = QDir::cleanPath(QFileInfo(project.data.path()).absoluteFilePath());
        const QString bindingId = platform::bindingIdForDirectory(root);
        const application::ProjectLocation location{application::plainFolderConfig(bindingId).projectId, bindingId, root};
        const QString dir = paths.recoveryDir(location.projectId, location.bindingId);
        QVERIFY(QDir().mkpath(dir));
        writeFile(dir + QStringLiteral("/0001.json"), "{\"formatVersion\": \"1.0\", \"proj");

        w.answers.setAsideUnusable = false;
        w.window.resolveRecoveryRecords();
        QCOMPARE(w.answers.unusableTexts.size(), 1);
        QVERIFY2(w.answers.unusableTexts[0].contains(QStringLiteral("0001.json")), qPrintable(w.answers.unusableTexts[0]));
        QVERIFY(QFile::exists(dir + QStringLiteral("/0001.json")));

        w.answers.setAsideUnusable = true;
        w.window.resolveRecoveryRecords();
        QCOMPARE(w.answers.unusableTexts.size(), 2);
        QVERIFY(!QFile::exists(dir + QStringLiteral("/0001.json")));
        QCOMPARE(QDir(dir + QStringLiteral("/unusable")).entryList(QDir::Files).size(), 1);
        const application::RecoveryScan scan = application::RecoveryService(paths, location).scan();
        QVERIFY(scan.candidates.isEmpty() && scan.unusable.isEmpty());

        // Nothing left: not asked again.
        w.window.resolveRecoveryRecords();
        QCOMPARE(w.answers.unusableTexts.size(), 2);
    }

    // Review of #11, 5: a name chosen in the program's own class dialog stays as the
    // classes.txt has it; only a typed name loses the spaces around it.
    void classChosenInTheDialogKeepsItsSpaces()
    {
        QWidget parent;
        const Prompts prompts = Prompts::dialogs(&parent);
        QString typed;  // empty: take the item shown
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (!typed.isEmpty())
                dialog->setTextValue(typed);
            dialog->accept();
        });
        answer.start(20);
        const QStringList classes{QStringLiteral(" scratch "), QStringLiteral("dent")};
        QCOMPARE(prompts.chooseLabel(classes, QString(), true), std::optional<QString>(QStringLiteral(" scratch ")));
        typed = QStringLiteral("  hole ");
        QCOMPARE(prompts.chooseLabel(classes, QString(), true), std::optional<QString>(QStringLiteral("hole")));
        typed.clear();
        QCOMPARE(prompts.chooseLabel(classes, QStringLiteral(" scratch "), false),
                 std::optional<QString>(QStringLiteral(" scratch ")));
    }

    // A folder of images without a project file opens as in X-AnyLabeling: any label, every
    // shape tool, no batches, and nothing in the folder but the annotation files.
    void plainFolderOpensWithoutAProjectFile()
    {
        Project project;
        QFile::remove(project.data.filePath(QStringLiteral("liteanno/project.json")));
        QDir(project.data.filePath(QStringLiteral("liteanno"))).removeRecursively();
        writeFile(project.data.filePath(QStringLiteral("classes.txt")), "dent\nscratch\n");
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.answers.messages.isEmpty());
        QCOMPARE(w.window.imageCount(), 3);
        QVERIFY(!w.window.startNextBatch());
        for (const char* tool : {"create_rectangle", "create_polygon", "create_rotation"}) {
            const QAction* action = w.window.findChild<QAction*>(QLatin1String(tool));
            QVERIFY2(action && action->isEnabled(), tool);
        }

        // A name the classes.txt does not have is typed in; the names known are offered.
        w.answers.label = QStringLiteral("hole");
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QCOMPARE(w.document().shapes().size(), 1);
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("hole"));
        QVERIFY(w.answers.labelFreeText);
        QCOMPARE(w.answers.labelsOffered, (QStringList{QStringLiteral("dent"), QStringLiteral("scratch")}));
        QVERIFY(w.blockers().isEmpty());
        w.window.flushSaves();
        QVERIFY(QFile::exists(project.json(1)));
        QVERIFY(w.window.nextImage());
        w.answers.label = QStringLiteral("dent");
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QCOMPARE(w.answers.labelsOffered,
                 (QStringList{QStringLiteral("dent"), QStringLiteral("scratch"), QStringLiteral("hole")}));

        // The export offers the folder's classes.txt; unconfirmed images are exported too.
        QTemporaryDir out;
        ExportChoice choice;
        choice.format = formats::ExportFormat::YoloDetection;
        choice.outputDir = out.filePath(QStringLiteral("det"));
        choice.classesFile = project.data.filePath(QStringLiteral("classes.txt"));
        w.answers.exportChoice = choice;
        w.answers.skipUnconvertible = true;  // "hole" is not in classes.txt
        QVERIFY(w.window.exportAnnotations());
        QVERIFY(w.answers.exportQuestion.plainFolder);
        QCOMPARE(w.answers.exportQuestion.batchNumber, 0);
        QCOMPARE(w.answers.exportQuestion.classesFile,
                 QDir::toNativeSeparators(project.data.filePath(QStringLiteral("classes.txt"))));
        QCOMPARE(w.answers.unconvertibleTexts.size(), 1);
        QVERIFY2(w.answers.unconvertibleTexts[0].contains(QStringLiteral("hole")), qPrintable(w.answers.unconvertibleTexts[0]));
        QVERIFY2(w.answers.messages.last().contains(QStringLiteral(u"已导出 1 张图片")), qPrintable(w.answers.messages.last()));
        QVERIFY(readFile(out.filePath(QStringLiteral("det/line1/0002.txt"))).startsWith("0 "));

        w.window.closeProject();
        QVERIFY(!QDir(project.data.filePath(QStringLiteral("liteanno"))).exists());
    }

    // The theme (style sheet and icons) is built in, and the panel shows the status, the
    // size and the shapes of the image.
    void themedWindow()
    {
        const QString sheet = themeStyleSheet();
        QVERIFY(sheet.contains(QStringLiteral("QPushButton#confirmButton")));
        Project project;
        Window w(project.options());
        for (const char* name : {"edit_polygon", "create_rectangle", "create_polygon", "create_rotation", "create_point",
                                 "create_line", "create_linestrip", "create_circle", "open_prev", "open_next", "fit_window"}) {
            const QAction* action = w.window.findChild<QAction*>(QLatin1String(name));
            QVERIFY2(action && !action->icon().pixmap(QSize(20, 20)).isNull(), name);
            QVERIFY2(!action->text().contains(QLatin1Char('(')), name);  // the keys are in the tooltip
            QVERIFY2(action->toolTip().contains(action->shortcut().toString(QKeySequence::NativeText)), name);
        }

        QVERIFY(w.window.openProject(project.data.path()));
        auto* chip = w.window.findChild<QLabel*>(QStringLiteral("statusChip"));
        QCOMPARE(chip->text(), QStringLiteral(u"未处理"));
        QCOMPARE(chip->property("tone").toString(), QStringLiteral("new"));
        QVERIFY2(w.window.findChild<QLabel*>(QStringLiteral("imageInfo"))->text().contains(QStringLiteral(u"第 1 / 3 张")),
                 qPrintable(w.window.findChild<QLabel*>(QStringLiteral("imageInfo"))->text()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QCOMPARE(chip->text(), QStringLiteral(u"待确认"));
        QCOMPARE(chip->property("tone").toString(), QStringLiteral("wait"));
        QVERIFY(w.window.confirmCurrent());
        QCOMPARE(chip->text(), QStringLiteral(u"已确认"));
        QCOMPARE(chip->property("tone").toString(), QStringLiteral("ok"));

        // The rows are drawn by the delegates, with the theme applied.
        qApp->setStyleSheet(sheet);
        QVERIFY(!w.window.grab().isNull());
        qApp->setStyleSheet(QString());
    }

    // Review of #13: the open image is confirmed, then deleted on disk and the folder scanned
    // again. The list and the panel must show "缺失", not the confirmed status of the open
    // document.
    void missingShowsBeforeTheDocumentStatus()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.confirmCurrent());
        w.window.flushSaves();
        QCOMPARE(w.window.imageStatusAt(0), std::optional<AnnotationStatus>(AnnotationStatus::Confirmed));

        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0001.png"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        // The missing image is kept in the list (at its end) and stays the image shown.
        auto* list = w.window.findChild<QListWidget*>(QStringLiteral("imageList"));
        int row = -1;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->text().startsWith(QStringLiteral("line1/0001.png")))
                row = i;
        }
        QVERIFY(row >= 0);
        QVERIFY2(list->item(row)->text().contains(QStringLiteral(u"缺失")), qPrintable(list->item(row)->text()));
        QCOMPARE(w.window.imageStatusAt(row), std::optional<AnnotationStatus>(AnnotationStatus::Missing));
        QCOMPARE(w.window.currentIndex(), row);
        auto* chip = w.window.findChild<QLabel*>(QStringLiteral("statusChip"));
        QCOMPARE(chip->text(), QStringLiteral(u"缺失"));
        QCOMPARE(chip->property("tone").toString(), QStringLiteral("problem"));
    }

    // Review of #13: the window shows the logo and the program's name only, also with a
    // folder open; label tags pick the text colour by contrast.
    void titleAndTagText()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QCOMPARE(w.window.windowTitle(), QStringLiteral("aoi-LiteAnno"));
        QVERIFY(!w.window.windowIcon().pixmap(QSize(32, 32)).isNull());
        w.window.closeProject();
        QCOMPARE(w.window.windowTitle(), QStringLiteral("aoi-LiteAnno"));

        const QColor dark(0x11, 0x11, 0x11);
        QCOMPARE(readableTextOn(QColor(0xFF, 0xFF, 0x00)), dark);  // pure yellow (HSL lightness 0.5)
        QCOMPARE(readableTextOn(QColor(0xFF, 0xB0, 0x20)), dark);
        QCOMPARE(readableTextOn(QColor(0x00, 0xFF, 0x00)), dark);
        QCOMPARE(readableTextOn(QColor(0x1F, 0x6F, 0xEB)), QColor(Qt::white));
        QCOMPARE(readableTextOn(QColor(0x80, 0x00, 0x00)), QColor(Qt::white));
    }

    // The bar under the canvas: the image's place in the list, the zoom and the mouse.
    void navigationBar()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        auto* position = w.window.findChild<QLabel*>(QStringLiteral("positionLabel"));
        auto* zoom = w.window.findChild<QLabel*>(QStringLiteral("zoomLabel"));
        auto* cursor = w.window.findChild<QLabel*>(QStringLiteral("cursorLabel"));
        QCOMPARE(position->text(), QStringLiteral("1 / 3"));
        QVERIFY(w.window.nextImage());
        QCOMPARE(position->text(), QStringLiteral("2 / 3"));

        QCOMPARE(zoom->text(), QStringLiteral("%1%").arg(qRound(w.window.canvas()->zoom() * 100)));
        w.window.canvas()->zoomToOriginalSize();
        QCOMPARE(zoom->text(), QStringLiteral("100%"));
        w.window.canvas()->zoomBy(2);
        QCOMPARE(zoom->text(), QStringLiteral("200%"));

        // The buttons are the menu actions: "上一张" goes back.
        auto* bar = w.window.findChild<QWidget*>(QStringLiteral("navBar"));
        for (QToolButton* button : bar->findChildren<QToolButton*>()) {
            if (button->defaultAction() && button->defaultAction()->objectName() == QLatin1String("open_prev"))
                button->click();
        }
        QCOMPARE(w.window.currentIndex(), 0);
        QCOMPARE(position->text(), QStringLiteral("1 / 3"));

        Canvas* canvas = w.window.canvas();
        canvas->fitToWindow();
        const QPoint centre = canvas->rect().center();
        QTest::mouseMove(canvas, centre);
        QTRY_VERIFY2(cursor->text().startsWith(QStringLiteral("x ")), qPrintable(cursor->text()));

        // Review of #14: zooming or panning under a still mouse updates the position shown.
        QTest::mouseMove(canvas, centre + QPoint(40, 20));
        QTRY_VERIFY(cursor->text().startsWith(QStringLiteral("x ")));
        const QString before = cursor->text();
        w.window.findChild<QAction*>(QStringLiteral("zoom_in"))->trigger();
        QVERIFY2(cursor->text() != before, qPrintable(before));
        const QString zoomed = cursor->text();
        QWheelEvent scroll(QPointF(centre + QPoint(40, 20)), canvas->mapToGlobal(QPointF(centre + QPoint(40, 20))),
                           QPoint(), QPoint(0, -240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &scroll);  // a plain wheel scrolls the view
        QVERIFY2(cursor->text() != zoomed, qPrintable(zoomed));

        QTest::mouseMove(canvas, QPoint(1, 1));  // the margin around the image
        canvas->fitToWindow();
        QTRY_VERIFY(cursor->text().isEmpty());
    }

    // "选中形状": the class and a rectangle's position and size can be typed; the numbers not
    // typed keep their exact values, the rectangle stays in the image, and undo takes it back.
    void selectedShapeForm()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        auto* form = w.window.findChild<QWidget*>(QStringLiteral("shapeForm"));
        QVERIFY(!form->isVisible());
        w.drawRectangle(QPointF(20.25, 10.5), QPointF(80.75, 60.25));
        QCOMPARE(w.canvas()->selectedShape(), qsizetype(0));
        QVERIFY(form->isVisible());
        QCOMPARE(w.window.findChild<QLabel*>(QStringLiteral("shapeType"))->text(), QStringLiteral(u"矩形"));
        auto* label = w.window.findChild<QComboBox*>(QStringLiteral("shapeLabel"));
        QCOMPARE(label->currentText(), QStringLiteral("scratch"));
        QVERIFY(!label->isEditable());  // the project's classes only
        const PointList drawn = w.document().shapes()[0].points;
        const double left = drawn[0].x(), top = drawn[0].y();
        const double right = drawn[2].x(), bottom = drawn[2].y();

        const auto type = [&](const char* name, const QString& text) {
            auto* field = w.window.findChild<QDoubleSpinBox*>(QLatin1String(name));
            QVERIFY2(field && field->isEnabled(), name);
            field->setFocus();
            field->selectAll();
            QTest::keyClicks(field, text);
            QTest::keyClick(field, Qt::Key_Return);
        };
        type("shapeX", QStringLiteral("30"));
        PointList points = w.document().shapes()[0].points;
        QCOMPARE(points[0], QPointF(30, top));
        QCOMPARE(points[2], QPointF(30 + (right - left), bottom));
        type("shapeHeight", QStringLiteral("40"));
        points = w.document().shapes()[0].points;
        QCOMPARE(points[0], QPointF(30, top));
        QCOMPARE(points[2].y(), top + 40);
        QCOMPARE(points[2].x(), 30 + (right - left));

        // Too wide for the image (200 pixels): the field stops at the image's edge.
        type("shapeWidth", QStringLiteral("500"));
        points = w.document().shapes()[0].points;
        QVERIFY2(points[2].x() <= 200, qPrintable(QString::number(points[2].x())));
        QCOMPARE(points[0], QPointF(30, top));

        // The class from the list.
        emit label->activated(int(label->findText(QStringLiteral("dent"))));
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("dent"));

        // Undo, one step at a time.
        QVERIFY(w.window.undo());
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("scratch"));
        while (w.window.undo() && w.document().shapes().size() == 1 && w.document().shapes()[0].points != drawn) {
        }
        QCOMPARE(w.document().shapes()[0].points, drawn);

        // A polygon shows its bounding box, which cannot be typed.
        w.canvas()->setTool(Canvas::Tool::Polygon);
        for (const QPointF& p : {QPointF(120, 20), QPointF(180, 30), QPointF(150, 80)})
            click(w.canvas(), p);
        QTest::keyClick(w.canvas(), Qt::Key_Return);
        w.canvas()->setTool(Canvas::Tool::Select);
        QCOMPARE(w.document().shapes().size(), 2);
        w.canvas()->setSelectedShape(1);
        w.window.findChild<QListWidget*>(QStringLiteral("shapeList"))->setCurrentRow(1);
        QCOMPARE(w.window.findChild<QLabel*>(QStringLiteral("shapeType"))->text(), QStringLiteral(u"多边形"));
        QVERIFY(!w.window.findChild<QDoubleSpinBox*>(QStringLiteral("shapeX"))->isEnabled());
        QVERIFY(label->isEnabled());
    }

    // Review of #15: the fields show one decimal, so their limits are rounded (139.75 shows
    // as 139.8). A value at the shown limit must still keep the rectangle in the image.
    void selectedShapeFormStaysInTheImage()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.session()->setShapePoints(
            0, {QPointF(20.25, 10.5), QPointF(80.5, 10.5), QPointF(80.5, 60.25), QPointF(20.25, 60.25)}));
        auto* shapes = w.window.findChild<QListWidget*>(QStringLiteral("shapeList"));
        shapes->setCurrentRow(-1);
        shapes->setCurrentRow(0);
        const auto type = [&](const char* name, const QString& text) {
            auto* field = w.window.findChild<QDoubleSpinBox*>(QLatin1String(name));
            field->setFocus();
            field->selectAll();
            QTest::keyClicks(field, text);
            QTest::keyClick(field, Qt::Key_Return);
        };
        type("shapeX", QStringLiteral("139.8"));
        PointList points = w.document().shapes()[0].points;
        QCOMPARE(points[2].x(), 200.0);
        QCOMPARE(points[0].x(), 139.75);  // the width stays 60.25
        QVERIFY(w.window.undo());
        shapes->setCurrentRow(-1);
        shapes->setCurrentRow(0);
        type("shapeWidth", QStringLiteral("179.8"));
        points = w.document().shapes()[0].points;
        QCOMPARE(points[0].x(), 20.25);
        QCOMPARE(points[2].x(), 200.0);
        type("shapeHeight", QStringLiteral("89.8"));  // 100 - 10.5 = 89.5 at most
        QVERIFY(w.document().shapes()[0].points[2].y() <= 100.0);
    }

    // Review of #15: a number typed but not yet entered is taken when the image is changed
    // with a button that does not take the focus (the bar under the canvas).
    void selectedShapeFormInputBeforeNextImage()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        const PointList drawn = w.document().shapes()[0].points;
        auto* field = w.window.findChild<QDoubleSpinBox*>(QStringLiteral("shapeX"));
        field->setFocus();
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("50"));
        QToolButton* next = nullptr;
        for (QToolButton* button : w.window.findChild<QWidget*>(QStringLiteral("navBar"))->findChildren<QToolButton*>()) {
            if (button->defaultAction() && button->defaultAction()->objectName() == QLatin1String("open_next"))
                next = button;
        }
        QVERIFY(next);
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(w.window.currentIndex(), 1);
        QVERIFY(w.window.previousImage());
        const PointList points = w.document().shapes()[0].points;
        QCOMPARE(points[0].x(), 50.0);
        QCOMPARE(points[2].x(), 50 + (drawn[2].x() - drawn[0].x()));  // read back from the saved file
    }

    // Without a class list any name may be typed in the form, as in the label dialog.
    void selectedShapeFormFreeText()
    {
        Project project;
        QDir(project.data.filePath(QStringLiteral("liteanno"))).removeRecursively();
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        w.answers.label = QStringLiteral("dent");
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        auto* label = w.window.findChild<QComboBox*>(QStringLiteral("shapeLabel"));
        QVERIFY(label->isEditable());
        label->setFocus();
        label->lineEdit()->selectAll();
        QTest::keyClicks(label->lineEdit(), QStringLiteral("  hole "));
        QTest::keyClick(label->lineEdit(), Qt::Key_Return);
        QCOMPARE(w.document().shapes()[0].label, QStringLiteral("hole"));
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
        QCOMPARE(w.confirmButton()->text(), QStringLiteral(u"确认无目标  Ctrl+Alt+K"));
        QVERIFY(w.confirmButton()->isEnabled());

        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QCOMPARE(w.document().shapes().size(), 1);
        const Shape& shape = w.document().shapes()[0];
        QCOMPARE(shape.label, QStringLiteral("scratch"));
        QCOMPARE(shape.type, ShapeType::Rectangle);
        QVERIFY(qAbs(shape.points[0].x() - 20) < 1 && qAbs(shape.points[2].y() - 60) < 1);
        QCOMPARE(w.confirmButton()->text(), QStringLiteral(u"确认  Ctrl+Alt+K"));

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
            // Not in X-AnyLabeling: keys no upstream action uses.
            {"skip_image", "Ctrl+Alt+S"},     {"show_shortcuts", "F1"},
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

    // Help > 快捷键一览 lists every key once, the canvas's own keys too.
    void shortcutList()
    {
        Project project;
        Window w(project.options());
        const auto rows = w.window.shortcutList();
        QStringList keys;
        for (const auto& row : rows)
            keys.append(row.second);
        for (const QAction* action : w.window.findChildren<QAction*>()) {
            if (!action->shortcut().isEmpty())
                QVERIFY2(keys.contains(action->shortcut().toString(QKeySequence::NativeText)), qPrintable(action->text()));
        }
        QSet<QString> names;
        for (const auto& row : rows) {
            QVERIFY2(!names.contains(row.first), qPrintable(row.first));
            names.insert(row.first);
        }
        QVERIFY(keys.contains(QStringLiteral("Ctrl+Alt+K")));
        QVERIFY(keys.contains(QStringLiteral("Ctrl+Alt+S")));
        QVERIFY(keys.contains(QStringLiteral(u"Esc")));
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

    // Review of PR #9, item 2: choosing a hidden shape in the list shows and selects it; the list
    // and the canvas agree, so Delete removes that shape and not the one selected before.
    void choosingAHiddenShapeInTheList()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        Canvas* canvas = w.canvas();
        canvas->setFocus();
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.drawRectangle(QPointF(100, 20), QPointF(150, 70));
        const quint64 second = w.document().shapes()[1].id;
        canvas->setSelectedShape(1);
        QTest::keyClick(canvas, Qt::Key_S);
        auto* list = w.window.findChild<QListWidget*>(QStringLiteral("shapeList"));
        list->setCurrentRow(0);
        QCOMPARE(canvas->selectedShape(), qsizetype(0));
        list->setCurrentRow(1);
        QVERIFY(!canvas->isHidden(1));
        QCOMPARE(canvas->selectedShape(), qsizetype(1));
        QCOMPARE(list->currentRow(), 1);
        QTest::keyClick(canvas, Qt::Key_Delete);
        QCOMPARE(w.document().shapes().size(), 1);
        QVERIFY(w.document().shapes()[0].id != second);
    }

    // O draws a rotated box upright; Z/V turn it by 1°, X/C by 0.1°, as upstream; dragging a
    // corner keeps the angle. The file gets upstream's numbers.
    void rotatedBoxes()
    {
        Project project;
        project.writeProjectFile(QByteArray(), R"("rectangle", "polygon", "rotation")");
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

    // Review of PR #9, item 3: a rotated box whose file has no "direction" (or one that does
    // not fit its corners) keeps its angle when a corner is dragged; the angle comes from the
    // corners and is written to the file.
    void rotatedBoxWithoutDirection()
    {
        Project project;
        project.writeProjectFile(QByteArray(), R"("rectangle", "polygon", "rotation")");
        writeFile(project.json(1),
                  R"({"version": "4.2.0", "flags": {}, "shapes": [{"label": "scratch", "points": [[50, 20], [90, 60], [70, 80], [30, 40]], )"
                  R"("group_id": null, "description": "", "difficult": false, "shape_type": "rotation", "flags": {}}], )"
                  R"("imagePath": "0001.png", "imageData": null, "imageHeight": 100, "imageWidth": 200})");
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        Canvas* canvas = w.canvas();
        canvas->setFocus();
        canvas->fitToWindow();
        const Shape original = w.document().shapes()[0];
        QCOMPARE(original.type, ShapeType::Rotation);
        canvas->setSelectedShape(0);
        drag(canvas, QPointF(50, 20), QPointF(45, 15));
        const Shape moved = w.document().shapes()[0];
        const double quarter = 3.141592653589793 / 4;
        QVERIFY(qAbs(moved.direction - quarter) < 1e-12);
        QVERIFY(exactlyEqual(moved.points, moveBoxCorner({original.points, moved.direction}, 0, moved.points[0])));
        // Still a box at 45°: side 0-1 runs along the angle, side 1-2 across it.
        const QPointF side = moved.points[1] - moved.points[0];
        const QPointF across = moved.points[2] - moved.points[1];
        QVERIFY(qAbs(side.y() - side.x()) < 1e-9);
        QVERIFY(qAbs(across.y() + across.x()) < 1e-9);
        QVERIFY(qAbs(side.x()) > 1);

        w.window.saveNow();
        w.window.flushSaves();
        const QJsonObject shape = readJson(project.json(1))[QStringLiteral("shapes")].toArray()[0].toObject();
        QVERIFY(qAbs(shape[QStringLiteral("direction")].toDouble() - quarter) < 1e-12);
    }

    // Review of PR #9, item 14: the rotated box tool is there only when the project has rotated
    // boxes among its shape types, as the other tools.
    void rotationToolFollowsTheProject()
    {
        Project project;
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        Canvas* canvas = w.canvas();
        canvas->setFocus();
        auto* action = w.window.findChild<QAction*>(QStringLiteral("create_rotation"));
        QVERIFY(action);
        QVERIFY(!action->isEnabled());
        QTest::keyClick(canvas, Qt::Key_O);
        QCOMPARE(canvas->tool(), Canvas::Tool::Select);
        // The same for the tools of PR #10.
        for (const char* name : {"create_point", "create_line", "create_linestrip", "create_circle"})
            QVERIFY2(!w.window.findChild<QAction*>(QLatin1String(name))->isEnabled(), name);
        for (const Qt::Key key : {Qt::Key_P, Qt::Key_L, Qt::Key_S, Qt::Key_C}) {
            QTest::keyClick(canvas, key, Qt::ShiftModifier);
            QCOMPARE(canvas->tool(), Canvas::Tool::Select);
        }
    }

    // Points, lines, line strips and circles, with upstream's keys.
    void otherShapes()
    {
        Project project;
        project.writeProjectFile(QByteArray(), R"("point", "line", "linestrip", "circle")");
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
        // (The point was clicked at a screen pixel, so it need not be on a whole image pixel:
        // it stops at less than one pixel from the edge.)
        for (int i = 0; i < 20; ++i)
            QTest::keyClick(canvas, Qt::Key_Up);
        const double start = drawn[0].y() + 1;
        QCOMPARE(w.document().shapes()[0].points[0].y(), start - std::floor(start));

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
        // Ctrl+Alt+S skips it, with the reason asked for.
        w.answers.skipReason = QStringLiteral(u"图片模糊");
        QTest::keyClick(canvas, Qt::Key_S, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(w.document().review().skip, SkipState::Skipped);
        QCOMPARE(w.document().review().skipReason, QStringLiteral(u"图片模糊"));
        QVERIFY(w.window.unskipCurrent());

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

    // Review of PR #7: images confirmed outside the window count as soon as the background
    // check has read them, and the batch is recorded as complete then.
    void batchStateFollowsTheBackgroundCheck()
    {
        Project project("\"batchSize\": 3", 3);
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QVERIFY(w.window.startNextBatch());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.confirmCurrent());
        w.window.flushSaves();
        QCOMPARE(w.window.batchStateText(), QStringLiteral(u"第 1 批：已完成 1 / 3 张"));
        for (int i : {2, 3}) {
            writeFile(project.json(i), QStringLiteral(R"({"version": "2.4.0", "flags": {}, "checked": true, "shapes": [], )"
                                                      R"("imagePath": "%1.png", "imageData": null, "imageWidth": 200, "imageHeight": 100})")
                                           .arg(i, 4, 10, QLatin1Char('0'))
                                           .toUtf8());
        }
        // The listing comes first and the check of the changed files after it.
        QSemaphore listed, checkMayStart;
        const QMetaObject::Connection connection = QObject::connect(
            w.window.catalog(), &application::ProjectCatalog::scanFinished, w.window.catalog(),
            [&](bool) {
                listed.release();
                checkMayStart.tryAcquire(1, 20000);
            },
            Qt::DirectConnection);
        w.window.rescan();
        QVERIFY(listed.tryAcquire(1, 10000));
        QCoreApplication::processEvents();
        QObject::disconnect(connection);
        auto* state = w.window.findChild<QLabel*>(QStringLiteral("batchState"));
        QCOMPARE(state->text(), QStringLiteral(u"第 1 批：已完成 1 / 3 张"));
        checkMayStart.release();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QTRY_COMPARE(state->text(), QStringLiteral(u"第 1 批：已完成 3 / 3 张（本批已完成）"));
        const QJsonArray events =
            readJson(project.data.filePath(QStringLiteral("liteanno/worksets/b000001.progress.json")))
                .value(QStringLiteral("events"))
                .toArray();
        QCOMPARE(events.size(), 2);
        QCOMPARE(events[1].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("completed"));
    }

    // Review of PR #7: an image saved after the window moved on to another batch counts in
    // "选择批次…".
    void savesOfOtherBatchesCount()
    {
        Project project("\"batchSize\": 1", 2);
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QVERIFY(w.window.startNextBatch());
        QVERIFY(w.window.confirmCurrent());
        QVERIFY(w.window.startNextBatch());  // the save of the first image may still run
        w.window.flushSaves();
        w.answers.batch = -1;
        QVERIFY(!w.window.chooseBatch());
        QCOMPARE(w.answers.batchesOffered.size(), 2);
        QVERIFY2(w.answers.batchesOffered[0].contains(QStringLiteral(u"已完成 1 / 1 张")),
                 qPrintable(w.answers.batchesOffered[0]));
    }

    // Review of PR #7: a deleted image is not done in "选择批次…" either.
    void missingImagesAreNotDoneInTheBatchChoice()
    {
        Project project("\"batchSize\": 1", 2);
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QVERIFY(w.window.startNextBatch());
        QVERIFY(w.window.confirmCurrent());
        w.window.flushSaves();
        QCOMPARE(w.window.batchStateText(), QStringLiteral(u"第 1 批：已完成 1 / 1 张（本批已完成）"));
        QVERIFY(QFile::remove(project.data.filePath(QStringLiteral("line1/0001.png"))));
        w.window.rescan();
        QVERIFY(w.window.catalog()->waitUntilIdle(10000));
        QCoreApplication::processEvents();
        QCOMPARE(w.window.batchStateText(), QStringLiteral(u"第 1 批：已完成 0 / 1 张"));
        w.answers.batch = -1;
        QVERIFY(!w.window.chooseBatch());
        QVERIFY2(w.answers.batchesOffered.value(0).contains(QStringLiteral(u"已完成 0 / 1 张")),
                 qPrintable(w.answers.batchesOffered.value(0)));
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

    // A close that comes while the window waits for a save (the wait runs events) happens after
    // the wait; closing in the middle freed what the wait was using (a crash on Windows CI).
    void closingDuringASaveWaitWaitsForIt()
    {
        Project project;
        MainWindow::Options options = project.options();
        options.saveStageHook = [](application::SaveStage stage) {
            if (stage == application::SaveStage::RecoveryRecordWritten)
                QThread::msleep(300);
        };
        Window w(options);
        QVERIFY(w.window.openProject(project.data.path()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.window.saveNow();
        int closes = 0;
        QTimer::singleShot(0, &w.window, [&] {
            ++closes;
            QVERIFY(!w.window.close());
            QVERIFY(w.window.hasProject());
        });
        w.window.flushSaves();
        QCOMPARE(closes, 1);
        QVERIFY(w.window.hasProject());
        QTRY_VERIFY(!w.window.hasProject());
        QVERIFY(QFile::exists(project.json(1)));
    }

    // Review of PR #8 (fourth round): the close asked for during the wait must also wait for
    // what follows it: here the real "changed by another program" question, answered "later",
    // after which "处理未写入的编辑…" goes on with the saver. The steps of the review, through the
    // menu, with the program's own message box.
    void closingWaitsForTheQuestionAfterASaveWait()
    {
        Project project;
        MainWindow::Options options = project.options();
        options.saveStageHook = [](application::SaveStage stage) {
            if (stage == application::SaveStage::RecoveryRecordWritten)
                QThread::msleep(300);
        };
        Window w(options);
        QVERIFY(w.window.openProject(project.data.path()));
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        w.window.setPrompts(Prompts::dialogs(&w.window));  // the program's own questions from here
        // Another program writes the annotation file meanwhile.
        writeFile(project.json(1), R"({"version": "4.2.0", "flags": {}, "shapes": [], "imagePath": "0001.png", )"
                                   R"("imageData": null, "imageHeight": 100, "imageWidth": 200})");
        int closes = 0;
        QTimer::singleShot(0, &w.window, [&] {
            ++closes;
            QVERIFY(!w.window.close());
        });
        int questions = 0;
        QStringList asked;
        QTimer answer;
        connect(&answer, &QTimer::timeout, this, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            asked.append(dialog->windowTitle());
            if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
                for (QAbstractButton* button : box->buttons()) {
                    if (button->text() == QStringLiteral(u"稍后处理")) {
                        ++questions;
                        button->click();
                        return;
                    }
                }
            }
            dialog->reject();  // anything else (the unwritten edit is offered again): later
        });
        answer.start(20);
        QAction* resolve = nullptr;
        for (QAction* action : w.window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral(u"处理未写入的编辑…"))
                resolve = action;
        }
        QVERIFY(resolve);
        resolve->trigger();
        QCOMPARE(closes, 1);
        QCOMPARE(questions, 1);
        QTRY_VERIFY2(!w.window.hasProject(), qPrintable(asked.join(QLatin1Char('|'))));
        answer.stop();
    }

    void closingDuringAnExportWaitsForIt()
    {
        // The review's steps: export, and close the window before the progress dialog shows.
        Project project("\"batchSize\": 2", 3);
        Window w(project.options());
        QVERIFY(w.window.openProject(project.data.path()));
        QVERIFY(w.window.startNextBatch());
        w.drawRectangle(QPointF(20, 10), QPointF(80, 60));
        QVERIFY(w.window.confirmCurrent());

        QTemporaryDir out;
        ExportChoice choice;
        choice.format = formats::ExportFormat::YoloDetection;
        choice.wholeProject = true;
        choice.outputDir = out.filePath(QStringLiteral("det"));
        w.answers.exportChoice = choice;
        int closes = 0;
        QTimer::singleShot(0, &w.window, [&] {
            ++closes;
            QVERIFY(!w.window.close());  // the export still uses the project: it is not closed now
            QVERIFY(w.window.hasProject());
        });
        QVERIFY(!w.window.exportAnnotations());
        QCOMPARE(closes, 1);
        QVERIFY(w.window.hasProject());
        QVERIFY(w.answers.messages.isEmpty());  // cancelled: no report
        QVERIFY(QDir(out.filePath(QStringLiteral("det"))).isEmpty());
        // The close asked for happens once the export has ended.
        QTRY_VERIFY(!w.window.hasProject());
        QVERIFY(!w.window.isVisible());
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
