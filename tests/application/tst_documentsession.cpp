// Editing one image: undo and redo restore the whole content including the review state
// (format spec 5.2), and background saves (architecture 8.1, 8.2) keep track of what is on
// disk while editing goes on.

#include "application/BackgroundSaver.h"
#include "application/DocumentSession.h"
#include "application/ProjectSession.h"
#include "common/ReliableSaveTestData.h"
#include "common/TestData.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace liteanno;
using namespace liteanno::application;
using namespace liteanno::formats;

namespace {

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

Shape box(double x, const QString& label = QStringLiteral("scratch"))
{
    Shape shape;
    shape.type = ShapeType::Rectangle;
    shape.label = label;
    shape.points = {QPointF(x, 10), QPointF(x + 20, 30)};
    return shape;
}

ProjectConfig projectWithClasses()
{
    ProjectConfig project;
    project.classes = {{0, QStringLiteral("scratch"), QString()}, {1, QStringLiteral("dent"), QString()}};
    return project;
}

struct Setup {
    QTemporaryDir local;
    QTemporaryDir data;
    SaveCoordinator coordinator() const
    {
        return SaveCoordinator(LocalDataPaths(local.path()), test::reliableSaveLocation(data.path()));
    }
    QString annotationPath() const { return data.filePath(QStringLiteral("line1/0002.json")); }
    DocumentSession open(const ProjectConfig& project = projectWithClasses()) const
    {
        QDir(data.path()).mkpath(QStringLiteral("line1"));
        return DocumentSession(test::kReliableSaveImage,
                               loadAnnotationFile(annotationPath(), test::reliableSaveImageInfo()), project);
    }
};

void saveNow(BackgroundSaver& saver, DocumentSession& session)
{
    saver.submit(session.snapshot());
    QVERIFY(saver.waitUntilAllDone(30000));
    for (const SaveJobResult& result : saver.takeResults()) {
        QCOMPARE(saveStatusName(result.outcome.result.status), saveStatusName(SaveStatus::Saved));
        session.savedSnapshot(result.entryId, *result.saved, result.stamp);
    }
}

} // namespace

class TestDocumentSession : public QObject
{
    Q_OBJECT

private slots:
    void undoAndRedoRestoreReviewState()
    {
        Setup setup;
        DocumentSession session = setup.open();
        QVERIFY(!session.needsSave());
        QVERIFY(session.addShape(box(1)) >= 0);
        QVERIFY(session.confirm());
        QVERIFY(session.document().review().checked);

        // Undoing the confirmation, then the shape.
        QVERIFY(session.undo());
        QVERIFY(!session.document().review().checked);
        QCOMPARE(session.document().shapes().size(), 1);
        QVERIFY(session.undo());
        QVERIFY(session.document().shapes().isEmpty());
        QVERIFY(!session.canUndo());
        // Back at the content as read: nothing to write.
        QVERIFY(!session.needsSave());

        QVERIFY(session.redo());
        QVERIFY(session.redo());
        QVERIFY(session.document().review().checked);
        QVERIFY(!session.canRedo());

        // A skip, undone, gives the confirmation back.
        QVERIFY(session.skip(QStringLiteral("blurred")));
        QCOMPARE(session.document().review().skip, SkipState::Skipped);
        QVERIFY(!session.document().review().checked);
        QVERIFY(session.undo());
        QVERIFY(session.document().review().checked);
        QCOMPARE(session.document().review().skip, SkipState::None);

        // A new edit drops the redo steps.
        QVERIFY(session.addShape(box(50)) >= 0);
        QVERIFY(!session.canRedo());
    }

    void confirmFollowsTheConditions()
    {
        Setup setup;
        DocumentSession session = setup.open();
        QVERIFY(session.addShape(box(1, QStringLiteral("crack"))) >= 0);
        QCOMPARE(session.confirmBlockers().size(), 1);
        QVERIFY(!session.confirm());
        QCOMPARE(annotationStatusName(session.status()), annotationStatusName(AnnotationStatus::PendingConfirmation));
        QVERIFY(session.setShapeLabel(0, QStringLiteral("dent")));
        QVERIFY(session.confirm());
        QCOMPARE(annotationStatusName(session.status()), annotationStatusName(AnnotationStatus::Confirmed));
    }

    void singleChoiceImageLabel()
    {
        Setup setup;
        ProjectConfig project;
        project.imageLabels = {ImageLabelMode::Single, {QStringLiteral("OK"), QStringLiteral("NG")}};
        DocumentSession session = setup.open(project);
        QVERIFY(session.selectSingleImageLabel(QStringLiteral("NG")));
        QVERIFY(session.selectSingleImageLabel(QStringLiteral("OK")));
        QCOMPARE(session.document().imageLabel(QStringLiteral("OK")), std::optional<bool>(true));
        QCOMPARE(session.document().imageLabel(QStringLiteral("NG")), std::optional<bool>(false));
        QVERIFY(session.confirmBlockers().isEmpty());
        // One step to undo.
        QVERIFY(session.undo());
        QCOMPARE(session.document().imageLabel(QStringLiteral("NG")), std::optional<bool>(true));
    }

    void readOnlyRefusesEdits()
    {
        Setup setup;
        writeFile(setup.annotationPath(), "{");
        DocumentSession session = setup.open();
        QVERIFY(session.isReadOnly());
        QCOMPARE(session.addShape(box(1)), qsizetype(-1));
        QVERIFY(!session.skip(QStringLiteral("x")));
        QCOMPARE(annotationStatusName(session.status()), annotationStatusName(AnnotationStatus::Corrupt));
    }

    void backgroundSaveWritesTheFile()
    {
        Setup setup;
        DocumentSession session = setup.open();
        BackgroundSaver saver(setup.coordinator());
        QVERIFY(session.addShape(box(1)) >= 0);
        saveNow(saver, session);
        QVERIFY(!session.needsSave());
        QCOMPARE(readFile(setup.annotationPath()), session.file().toBytes());

        // Undo after the save: the file must be written again.
        QVERIFY(session.undo());
        QVERIFY(session.needsSave());
        saveNow(saver, session);
        QVERIFY(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].empty());
        // Redo to the content saved before: needs writing too, the disk has the undone state.
        QVERIFY(session.redo());
        QVERIFY(session.needsSave());
    }

    // Edits made while a save runs stay to be saved, and the next save does not take the
    // file it just wrote for another program's change.
    void editsDuringASaveStay()
    {
        Setup setup;
        DocumentSession session = setup.open();
        BackgroundSaver saver(setup.coordinator());
        QVERIFY(session.addShape(box(1)) >= 0);
        saver.submit(session.snapshot());
        QVERIFY(session.addShape(box(50)) >= 0);
        QVERIFY(saver.waitUntilAllDone(30000));
        for (const SaveJobResult& result : saver.takeResults())
            session.savedSnapshot(result.entryId, *result.saved, result.stamp);
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(1));
        QVERIFY(session.needsSave());

        saveNow(saver, session);
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(2));
        // Undo to the first saved content.
        QVERIFY(session.undo());
        QVERIFY(session.needsSave());
    }

    void newerSnapshotReplacesAWaitingOne()
    {
        Setup setup;
        DocumentSession session = setup.open();
        // Many documents could be waiting; one per image.
        BackgroundSaver saver(setup.coordinator());
        for (int i = 0; i < 20; ++i) {
            QVERIFY(session.addShape(box(i * 30)) >= 0);
            saver.submit(session.snapshot());
            QVERIFY(saver.pendingCount() <= 2);
        }
        QVERIFY(saver.waitUntilAllDone(30000));
        std::vector<SaveJobResult> results = saver.takeResults();
        QVERIFY(!results.empty());
        for (const SaveJobResult& result : results)
            session.savedSnapshot(result.entryId, *result.saved, result.stamp);
        QVERIFY(!session.needsSave());
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(20));
    }

    // Snapshots taken before the window learned of earlier saves still save: those saves
    // are this program's, not another program's change.
    void snapshotsAcrossUnacknowledgedSaves()
    {
        Setup setup;
        DocumentSession session = setup.open();
        BackgroundSaver saver(setup.coordinator());
        QVERIFY(session.addShape(box(1)) >= 0);
        SaveSnapshot a = session.snapshot();
        QVERIFY(session.addShape(box(30)) >= 0);
        SaveSnapshot b = session.snapshot();

        saver.submit(std::move(a));
        QVERIFY(saver.waitUntilAllDone(30000));
        std::vector<SaveJobResult> results = saver.takeResults();
        QCOMPARE(results[0].outcome.result.status, SaveStatus::Saved);
        session.savedSnapshot(results[0].entryId, *results[0].saved, results[0].stamp);

        // b expects the file before a; c expects the file after a, but b writes in between.
        QVERIFY(session.addShape(box(60)) >= 0);
        SaveSnapshot c = session.snapshot();
        saver.submit(std::move(b));
        QVERIFY(saver.waitUntilAllDone(30000));
        saver.submit(std::move(c));
        QVERIFY(saver.waitUntilAllDone(30000));
        results = saver.takeResults();
        QCOMPARE(results.size(), std::size_t(2));
        for (const SaveJobResult& result : results) {
            QCOMPARE(saveStatusName(result.outcome.result.status), saveStatusName(SaveStatus::Saved));
            session.savedSnapshot(result.entryId, *result.saved, result.stamp);
        }
        QVERIFY(!session.needsSave());
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(3));

        // Another program's change is still caught.
        writeFile(setup.annotationPath(), "{}");
        QVERIFY(session.addShape(box(90)) >= 0);
        saver.submit(session.snapshot());
        QVERIFY(saver.waitUntilAllDone(30000));
        QCOMPARE(saver.takeResults()[0].outcome.result.status, SaveStatus::ExternallyModified);
    }

    // Review of 168a9cd, item 2: undone before the save was acknowledged, the undone content
    // is still written.
    void undoBeforeTheSaveIsAcknowledged()
    {
        Setup setup;
        DocumentSession session = setup.open();
        BackgroundSaver saver(setup.coordinator());
        QVERIFY(session.addShape(box(1)) >= 0);
        saver.submit(session.snapshot());
        QVERIFY(session.undo());
        // The file may already hold the rectangle.
        QVERIFY(session.needsSave());
        QVERIFY(saver.waitUntilAllDone(30000));
        for (const SaveJobResult& result : saver.takeResults())
            session.savedSnapshot(result.entryId, *result.saved, result.stamp);
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(1));
        QVERIFY(session.needsSave());
        saveNow(saver, session);
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(0));
        QVERIFY(!session.needsSave());
    }

    // The same when the undo comes after the save finished but before its result is handled.
    void undoBetweenSaveAndAcknowledgement()
    {
        Setup setup;
        DocumentSession session = setup.open();
        BackgroundSaver saver(setup.coordinator());
        QVERIFY(session.addShape(box(1)) >= 0);
        saver.submit(session.snapshot());
        QVERIFY(saver.waitUntilAllDone(30000));
        QVERIFY(session.undo());
        for (const SaveJobResult& result : saver.takeResults())
            session.savedSnapshot(result.entryId, *result.saved, result.stamp);
        QVERIFY(session.needsSave());
        saveNow(saver, session);
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(0));
    }

    // Item 3: undoing a saved removal brings back the shape's JSON object as it was.
    void undoSavedRemovalKeepsTheShapeObject()
    {
        Setup setup;
        writeFile(setup.annotationPath(), R"({
  "version": "2.4.0",
  "flags": {},
  "shapes": [
    {
      "label": "scratch",
      "points": [[10.0, 10.0], [40.0, 10.0], [40.0, 30.0], [10.0, 30.0]],
      "group_id": 7,
      "description": "kept",
      "shape_type": "rectangle",
      "flags": {"reviewed": true},
      "attributes": {"severity": "high"},
      "score": 0.9,
      "custom": {"k": 1}
    }
  ],
  "imagePath": "0002.jpg",
  "imageData": null,
  "imageHeight": 480,
  "imageWidth": 640
})");
        const auto original = test::parseOrFail(readFile(setup.annotationPath()))["shapes"][0];
        DocumentSession session = setup.open();
        BackgroundSaver saver(setup.coordinator());
        QVERIFY(session.removeShape(0));
        saveNow(saver, session);
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(0));
        QVERIFY(session.undo());
        saveNow(saver, session);
        const auto restored = test::parseOrFail(readFile(setup.annotationPath()))["shapes"][0];
        QVERIFY2(restored == original, restored.dump().c_str());
        // And through a second round, with a shape added in between.
        QVERIFY(session.redo());
        QVERIFY(session.addShape(box(100)) >= 0);
        saveNow(saver, session);
        QVERIFY(session.undo());
        QVERIFY(session.undo());
        saveNow(saver, session);
        const auto again = test::parseOrFail(readFile(setup.annotationPath()))["shapes"];
        QCOMPARE(again.size(), std::size_t(1));
        QVERIFY2(again[0] == original, again[0].dump().c_str());
    }

    // Item 7: after "重新加载" the new session's file is the base, even when it holds content
    // this program wrote over before.
    void reloadedSessionIsNotTakenForAnOlderSave()
    {
        Setup setup;
        const QByteArray before = test::readTestData(QStringLiteral("annotations/upstream_v2_legacy.json"));
        writeFile(setup.annotationPath(), before);
        BackgroundSaver saver(setup.coordinator());
        {
            DocumentSession first = setup.open();
            QVERIFY(first.addShape(box(1)) >= 0);
            saveNow(saver, first);
        }
        // Another program puts the old file back; the window reloads.
        writeFile(setup.annotationPath(), before);
        DocumentSession second = setup.open();
        QVERIFY(second.addShape(box(50)) >= 0);
        saveNow(saver, second);

        // The same after the file was deleted.
        QFile::remove(setup.annotationPath());
        DocumentSession third = setup.open();
        QVERIFY(third.addShape(box(80)) >= 0);
        saveNow(saver, third);
        QCOMPARE(test::parseOrFail(readFile(setup.annotationPath()))["shapes"].size(), std::size_t(1));
    }

    // Item 8: an annotation named with ".JSON" belongs to the image (format spec 3).
    void annotationExtensionInAnyCase()
    {
        Setup setup;
        QDir(setup.data.path()).mkpath(QStringLiteral("line1"));
        const QString upper = setup.data.filePath(QStringLiteral("line1/0002.JSON"));
        writeFile(upper, test::readTestData(QStringLiteral("annotations/upstream_v2_legacy.json")));
        const QString path = existingAnnotationPath(setup.annotationPath());
        QVERIFY(QFileInfo(path).exists());
        QCOMPARE(setup.coordinator().annotationFilePath(test::kReliableSaveImage), path);

        DocumentSession session(test::kReliableSaveImage, loadAnnotationFile(path, test::reliableSaveImageInfo()),
                                projectWithClasses());
        QVERIFY(!session.file().isNew());
        const qsizetype shapes = session.document().shapes().size();
        QVERIFY(shapes > 0);
        BackgroundSaver saver(setup.coordinator());
        QVERIFY(session.addShape(box(1)) >= 0);
        saveNow(saver, session);
        // Written to that file, not to a new lower-case one.
        const QStringList files = QDir(setup.data.filePath(QStringLiteral("line1"))).entryList(QDir::Files);
        QCOMPARE(files, QStringList{QStringLiteral("0002.JSON")});
        QCOMPARE(test::parseOrFail(readFile(upper))["shapes"].size(), std::size_t(shapes + 1));
    }

    void limitCountsWaitingDocuments()
    {
        Setup setup;
        BackgroundSaver::Limits limits;
        limits.maxDocuments = 2;
        BackgroundSaver saver(setup.coordinator(), limits);
        QVERIFY(!saver.atLimit());
        for (int i = 0; i < 5; ++i) {
            QDir(setup.data.path()).mkpath(QStringLiteral("line%1").arg(i));
            const QString image = QStringLiteral("line%1/0002.jpg").arg(i);
            DocumentSession session(image,
                                    loadAnnotationFile(setup.data.filePath(QStringLiteral("line%1/0002.json").arg(i)),
                                                       test::reliableSaveImageInfo()),
                                    projectWithClasses());
            QVERIFY(session.addShape(box(1)) >= 0);
            QVERIFY(saver.waitUntilBelowLimit(30000));
            saver.submit(session.snapshot());
            QVERIFY(saver.pendingCount() <= 2);
        }
        QVERIFY(saver.waitUntilAllDone(30000));
        QCOMPARE(saver.takeResults().size(), std::size_t(5));
        for (int i = 0; i < 5; ++i)
            QVERIFY(QFile::exists(setup.data.filePath(QStringLiteral("line%1/0002.json").arg(i))));
    }

    void externalChangeIsReported()
    {
        Setup setup;
        writeFile(setup.annotationPath(), test::readTestData(QStringLiteral("annotations/upstream_v2_legacy.json")));
        DocumentSession session = setup.open();
        BackgroundSaver saver(setup.coordinator());
        QVERIFY(session.addShape(box(1)) >= 0);
        writeFile(setup.annotationPath(), "{}");
        saver.submit(session.snapshot());
        QVERIFY(saver.waitUntilIdle(test::kReliableSaveImage, 30000));
        std::vector<SaveJobResult> results = saver.takeResults();
        QCOMPARE(results.size(), std::size_t(1));
        QCOMPARE(results[0].outcome.result.status, SaveStatus::ExternallyModified);
        QVERIFY(results[0].outcome.recoveryRecordKept);
        QCOMPARE(readFile(setup.annotationPath()), QByteArray("{}"));

        // "用我的版本覆盖".
        SaveOptions overwrite;
        overwrite.overwriteExternalChanges = true;
        saver.submit(session.snapshot(), overwrite);
        QVERIFY(saver.waitUntilAllDone(30000));
        results = saver.takeResults();
        QCOMPARE(results[0].outcome.result.status, SaveStatus::Saved);
        session.savedSnapshot(results[0].entryId, *results[0].saved, results[0].stamp);
        QCOMPARE(readFile(setup.annotationPath()), session.file().toBytes());
        QVERIFY(QDir(LocalDataPaths(setup.local.path()).recoveryDir(test::reliableSaveLocation(setup.data.path()).projectId,
                                                                 test::reliableSaveLocation(setup.data.path()).bindingId))
                    .entryList(QDir::Files)
                    .isEmpty());
    }

    void openProject()
    {
        QTemporaryDir local;
        QTemporaryDir data;
        const LocalDataPaths paths(local.path());
        ProjectSession::OpenResult result = ProjectSession::open(data.path(), paths);
        QCOMPARE(openErrorName(result.error), "no-project-file");

        writeFile(data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33", "batchSize": 0})");
        result = ProjectSession::open(data.path(), paths);
        QCOMPARE(openErrorName(result.error), "invalid-project-file");
        QCOMPARE(result.field, QStringLiteral("/batchSize"));

        writeFile(data.filePath(QStringLiteral("liteanno/project.json")),
                  R"({"formatVersion": "1.0", "projectId": "3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"})");
        result = ProjectSession::open(data.path(), paths);
        QVERIFY2(result.session, qPrintable(result.message));
        QCOMPARE(result.session->location().projectId, QStringLiteral("3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33"));
        QCOMPARE(result.session->location().bindingId.size(), 32);

        // A second window on the same project.
        ProjectSession::OpenResult second = ProjectSession::open(data.path() + QLatin1String("/."), paths);
        QCOMPARE(openErrorName(second.error), "locked");
        QCOMPARE(second.holder.pid, QCoreApplication::applicationPid());

        result.session.reset();
        QVERIFY(ProjectSession::open(data.path(), paths).session);
    }
};

QTEST_GUILESS_MAIN(TestDocumentSession)
#include "tst_documentsession.moc"
