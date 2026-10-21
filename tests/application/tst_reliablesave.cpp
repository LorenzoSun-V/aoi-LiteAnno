// Reliable saving (architecture section 8, format spec 8): recovery records written before
// the annotation file, recovery after a forced kill, conflicts never overwritten silently,
// and the project lock.

#include "application/ProjectLock.h"
#include "application/RecoveryService.h"
#include "application/SaveCoordinator.h"
#include "common/FileBlocker.h"
#include "common/ReliableSaveTestData.h"
#include "common/TestData.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace liteanno;
using namespace liteanno::application;
using namespace liteanno::formats;

namespace {

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

// A local data root and a data root with line1/0002.json, or without it.
struct Setup {
    QTemporaryDir local;
    QTemporaryDir data;
    QByteArray original;

    explicit Setup(bool withAnnotation = true)
    {
        if (withAnnotation) {
            original = test::readTestData(QStringLiteral("annotations/upstream_v2_legacy.json"));
            writeFile(annotationPath(), original);
        } else {
            QDir(data.path()).mkpath(QStringLiteral("line1"));
        }
    }

    LocalDataPaths paths() const { return LocalDataPaths(local.path()); }
    ProjectLocation location() const { return test::reliableSaveLocation(data.path()); }
    QString annotationPath() const { return data.filePath(QStringLiteral("line1/0002.json")); }
    QString recoveryDir() const { return paths().recoveryDir(location().projectId, location().bindingId); }
    QStringList records() const { return QDir(recoveryDir()).entryList(QDir::Files); }
    SaveCoordinator coordinator() const { return SaveCoordinator(paths(), location()); }
    RecoveryService recovery() const { return RecoveryService(paths(), location()); }

    LoadedAnnotation load() const
    {
        return loadAnnotationFile(annotationPath(), test::reliableSaveImageInfo());
    }

    // Runs the helper, which edits and saves line1/0002 and kills itself at `stage`.
    bool killDuringSave(const char* stage) const
    {
        QProcess helper;
        helper.start(QStringLiteral(LITEANNO_TEST_HELPER),
                     {QStringLiteral("kill-during-save"), local.path(), data.path(), QLatin1String(stage)});
        if (!helper.waitForFinished(30000))
            return false;
        // Killed, not a normal exit.
        return helper.exitStatus() == QProcess::CrashExit || helper.exitCode() == 9;
    }
};

bool hasShape(const LoadedAnnotation& annotation, const Shape& shape)
{
    for (const Shape& s : annotation.file.document().shapes()) {
        // Rectangles are read back with four corners.
        if (s.label == shape.label && s.type == shape.type && s.points.contains(shape.points[0])
            && s.points.contains(shape.points[1]))
            return true;
    }
    return false;
}

} // namespace

class TestReliableSave : public QObject
{
    Q_OBJECT

private slots:
    void saveRemovesTheRecord()
    {
        Setup setup;
        SaveCoordinator coordinator = setup.coordinator();
        QList<SaveStage> stages;
        coordinator.stageHook = [&](SaveStage stage) {
            // The record is on disk before the annotation file changes.
            if (stage == SaveStage::RecoveryRecordWritten) {
                QCOMPARE(setup.records().size(), 1);
                QCOMPARE(readFile(setup.annotationPath()), setup.original);
            }
            stages.append(stage);
        };

        LoadedAnnotation annotation = setup.load();
        QVERIFY(annotation.file.document().addShape(test::reliableSaveShape()) >= 0);
        const SaveOutcome outcome = coordinator.save(test::kReliableSaveImage, annotation, 1);
        QCOMPARE(saveStatusName(outcome.result.status), saveStatusName(SaveStatus::Saved));
        QVERIFY(!outcome.recoveryRecordKept);
        QCOMPARE(stages, (QList<SaveStage>{SaveStage::RecoveryRecordWritten, SaveStage::AnnotationWritten}));
        QCOMPARE(readFile(setup.annotationPath()), annotation.file.toBytes());
        QVERIFY(setup.records().isEmpty());
    }

    void unmodifiedAndReadOnlyWriteNoRecord()
    {
        Setup setup;
        SaveCoordinator coordinator = setup.coordinator();
        LoadedAnnotation annotation = setup.load();
        QCOMPARE(coordinator.save(test::kReliableSaveImage, annotation, 1).result.status, SaveStatus::NotModified);

        // A size mismatch makes the file read-only (format spec 5.3).
        LoadedAnnotation readOnly = loadAnnotationFile(setup.annotationPath(), ImageInfo{QStringLiteral("0002.jpg"), 1, 1, 1});
        QVERIFY(readOnly.file.isReadOnly());
        readOnly.file.document().confirm();
        QCOMPARE(coordinator.save(test::kReliableSaveImage, readOnly, 1).result.status, SaveStatus::ReadOnly);
        QVERIFY(!QDir(setup.recoveryDir()).exists() || setup.records().isEmpty());
        QCOMPARE(readFile(setup.annotationPath()), setup.original);
    }

    void invalidImagePathIsRefused()
    {
        Setup setup;
        LoadedAnnotation annotation = setup.load();
        QVERIFY(annotation.file.document().addShape(test::reliableSaveShape()) >= 0);
        QCOMPARE(setup.coordinator().save(QStringLiteral("../0002.jpg"), annotation, 1).result.status,
                 SaveStatus::Failed);
        QCOMPARE(readFile(setup.annotationPath()), setup.original);
    }

    // An edit that met an external change stays in its record until the user decides.
    void externalChangeKeepsTheRecord()
    {
        Setup setup;
        LoadedAnnotation annotation = setup.load();
        QVERIFY(annotation.file.document().addShape(test::reliableSaveShape()) >= 0);
        const QByteArray external = test::readTestData(QStringLiteral("annotations/upstream_v4.json"));
        writeFile(setup.annotationPath(), external);

        const SaveOutcome outcome = setup.coordinator().save(test::kReliableSaveImage, annotation, 4);
        QCOMPARE(outcome.result.status, SaveStatus::ExternallyModified);
        QVERIFY(outcome.recoveryRecordKept);
        QCOMPARE(readFile(setup.annotationPath()), external);

        const RecoveryScan scan = setup.recovery().scan();
        QCOMPARE(scan.candidates.size(), 1);
        QCOMPARE(scan.candidates[0].state, RecoveryState::Conflict);
        QCOMPARE(scan.candidates[0].record.documentVersion, 4);
        QCOMPARE(setup.recovery().restore(scan.candidates[0]).status, RecordStatus::Conflict);
        QCOMPARE(readFile(setup.annotationPath()), external);

        // The user chose the recorded version.
        QCOMPARE(setup.recovery().restore(scan.candidates[0], true).status, RecordStatus::Done);
        QCOMPARE(readFile(setup.annotationPath()), annotation.file.toBytes());
        QVERIFY(setup.records().isEmpty());
    }

    // Forced kill after the record, before the annotation file: the file is untouched and the
    // edit comes back from the record.
    void killedBeforeReplaceIsRestorable_data()
    {
        QTest::addColumn<bool>("withAnnotation");
        QTest::newRow("existing file") << true;
        QTest::newRow("new file") << false;
    }

    void killedBeforeReplaceIsRestorable()
    {
        QFETCH(bool, withAnnotation);
        Setup setup(withAnnotation);
        QVERIFY(setup.killDuringSave("after-record"));
        QCOMPARE(QFile::exists(setup.annotationPath()), withAnnotation);
        QCOMPARE(readFile(setup.annotationPath()), setup.original);

        const RecoveryScan scan = setup.recovery().scan();
        QVERIFY(scan.unusable.isEmpty());
        QCOMPARE(scan.candidates.size(), 1);
        const RecoveryCandidate& candidate = scan.candidates[0];
        QCOMPARE(candidate.state, RecoveryState::Restorable);
        QCOMPARE(candidate.record.imagePath, test::kReliableSaveImage);
        QCOMPARE(candidate.record.documentVersion, 7);
        QCOMPARE(candidate.record.rootPathAtWrite, setup.data.path());
        QCOMPARE(candidate.record.baseDigest, withAnnotation ? contentDigest(setup.original) : QString());

        QCOMPARE(setup.recovery().restore(candidate).status, RecordStatus::Done);
        QCOMPARE(readFile(setup.annotationPath()), candidate.record.annotation);
        QVERIFY(hasShape(setup.load(), test::reliableSaveShape()));
        QVERIFY(setup.records().isEmpty());
    }

    // Changed by another program after the kill: never written back without the user.
    void killedThenChangedIsAConflict_data()
    {
        QTest::addColumn<bool>("withAnnotation");
        QTest::newRow("file rewritten") << true;
        QTest::newRow("file created") << false;
    }

    void killedThenChangedIsAConflict()
    {
        QFETCH(bool, withAnnotation);
        Setup setup(withAnnotation);
        QVERIFY(setup.killDuringSave("after-record"));
        const QByteArray external = test::readTestData(QStringLiteral("annotations/upstream_v4.json"));
        writeFile(setup.annotationPath(), external);

        RecoveryScan scan = setup.recovery().scan();
        QCOMPARE(scan.candidates.size(), 1);
        QCOMPARE(scan.candidates[0].state, RecoveryState::Conflict);
        QCOMPARE(scan.candidates[0].current.digest, contentDigest(external));
        QCOMPARE(setup.recovery().restore(scan.candidates[0]).status, RecordStatus::Conflict);
        QCOMPARE(readFile(setup.annotationPath()), external);
        QCOMPARE(setup.records().size(), 1);

        // The user dropped the edit.
        QCOMPARE(setup.recovery().discard(scan.candidates[0]).status, RecordStatus::Done);
        QVERIFY(setup.records().isEmpty());
        QCOMPARE(readFile(setup.annotationPath()), external);
    }

    // A file changed between the scan and the restore is not overwritten either.
    void restoreChecksTheFileAgain()
    {
        Setup setup;
        QVERIFY(setup.killDuringSave("after-record"));
        const RecoveryScan scan = setup.recovery().scan();
        QCOMPARE(scan.candidates[0].state, RecoveryState::Restorable);
        writeFile(setup.annotationPath(), "{}");
        QCOMPARE(setup.recovery().restore(scan.candidates[0]).status, RecordStatus::Conflict);
        QCOMPARE(readFile(setup.annotationPath()), QByteArray("{}"));
    }

    // Forced kill after the replace, before the record is removed: the record is left over.
    void killedAfterReplaceIsAlreadySaved()
    {
        Setup setup;
        QVERIFY(setup.killDuringSave("after-annotation"));
        QVERIFY(hasShape(setup.load(), test::reliableSaveShape()));

        const RecoveryScan scan = setup.recovery().scan();
        QCOMPARE(scan.candidates.size(), 1);
        QCOMPARE(scan.candidates[0].state, RecoveryState::AlreadySaved);
        QCOMPARE(setup.recovery().removeAlreadySaved(scan), 1);
        QVERIFY(setup.records().isEmpty());
    }

    // A candidate from an old scan never removes a record a newer edit wrote since.
    void oldScanDoesNotRemoveANewerRecord()
    {
        Setup setup;
        QVERIFY(setup.killDuringSave("after-annotation"));
        const RecoveryScan old = setup.recovery().scan();
        QCOMPARE(old.candidates.size(), 1);
        QCOMPARE(old.candidates[0].state, RecoveryState::AlreadySaved);

        // A newer edit of the same image meets an external change; its record is kept.
        LoadedAnnotation annotation = setup.load();
        Shape other = test::reliableSaveShape();
        other.label = QStringLiteral("dent");
        QVERIFY(annotation.file.document().addShape(other) >= 0);
        writeFile(setup.annotationPath(), test::readTestData(QStringLiteral("annotations/upstream_v4.json")));
        const SaveOutcome outcome = setup.coordinator().save(test::kReliableSaveImage, annotation, 8);
        QVERIFY(outcome.recoveryRecordKept);
        const QByteArray newer = readFile(setup.recoveryDir() + QLatin1String("/") + setup.records().constFirst());

        QCOMPARE(setup.recovery().removeAlreadySaved(old), 0);
        QCOMPARE(setup.recovery().discard(old.candidates[0]).status, RecordStatus::Stale);
        QCOMPARE(setup.recovery().restore(old.candidates[0], true).status, RecordStatus::Stale);
        QCOMPARE(setup.records().size(), 1);
        QCOMPARE(readFile(setup.recoveryDir() + QLatin1String("/") + setup.records().constFirst()), newer);
        QCOMPARE(setup.recovery().scan().candidates[0].record.documentVersion, 8);
    }

    // An unreadable file is neither a conflict nor restorable: it is read again, or the
    // restore fails and keeps everything.
    void unreadableFileDuringRestore()
    {
        Setup setup;
        QVERIFY(setup.killDuringSave("after-record"));
        const RecoveryScan scan = setup.recovery().scan();
        QCOMPARE(scan.candidates[0].state, RecoveryState::Restorable);

        {
            test::FileBlocker blocker(setup.annotationPath(), test::FileBlocker::Mode::Read);
            if (!blocker.isActive())
                QSKIP("File permissions do not stop root");
            QCOMPARE(setup.recovery().scan().candidates[0].state, RecoveryState::TargetUnreadable);
            SaveOptions options;
            options.maxAttempts = 3;
            options.retryDelayMs = 1;
            QCOMPARE(setup.recovery().restore(scan.candidates[0], false, options).status, RecordStatus::Failed);
        }
        QCOMPARE(readFile(setup.annotationPath()), setup.original);
        QCOMPARE(setup.records().size(), 1);

        test::FileBlocker blocker(setup.annotationPath(), test::FileBlocker::Mode::Read);
        QVERIFY(blocker.isActive());
        SaveOptions options;
        options.retryDelayMs = 1;
        options.beforeRetry = [&blocker](int attempt) {
            if (attempt == 2)
                blocker.release();
        };
        QCOMPARE(setup.recovery().restore(scan.candidates[0], false, options).status, RecordStatus::Done);
        QCOMPARE(readFile(setup.annotationPath()), scan.candidates[0].record.annotation);
        QVERIFY(setup.records().isEmpty());
    }

    void unusableRecordsAreReportedAndKept()
    {
        Setup setup;
        QVERIFY(setup.killDuringSave("after-record"));
        const QString recordPath = setup.recoveryDir() + QLatin1String("/") + setup.records().constFirst();
        QByteArray record = readFile(recordPath);
        record.replace("\"line1/0002.jpg\"", "\"../outside.jpg\"");
        writeFile(recordPath, record);
        writeFile(setup.recoveryDir() + QLatin1String("/broken.json"), "{");

        const RecoveryScan scan = setup.recovery().scan();
        QVERIFY(scan.candidates.isEmpty());
        QCOMPARE(scan.unusable.size(), 2);
        QCOMPARE(setup.records().size(), 2);
        QVERIFY(!QFile::exists(setup.data.filePath(QStringLiteral("../outside.json"))));
    }

    // Review of 083aa48: a record this program cannot use is never replaced by a new save.
    // It is moved aside, byte for byte, before the new record is written.
    void unusableRecordIsMovedAsideBeforeASave_data()
    {
        QTest::addColumn<QByteArray>("from");
        QTest::addColumn<QByteArray>("to");
        QTest::newRow("newer format") << QByteArray("\"formatVersion\": \"1.0\"") << QByteArray("\"formatVersion\": \"2.0\"");
        QTest::newRow("broken") << QByteArray("\"formatVersion\"") << QByteArray("\"formatVersion");
        QTest::newRow("other image") << QByteArray("\"line1/0002.jpg\"") << QByteArray("\"line1/0003.jpg\"");
    }

    void unusableRecordIsMovedAsideBeforeASave()
    {
        QFETCH(QByteArray, from);
        QFETCH(QByteArray, to);
        Setup setup;
        QVERIFY(setup.killDuringSave("after-record"));
        QCOMPARE(setup.records().size(), 1);
        const QString recordPath = setup.recoveryDir() + QLatin1String("/") + setup.records().constFirst();
        QByteArray unusable = readFile(recordPath);
        QVERIFY(unusable.contains(from));
        unusable.replace(from, to);
        writeFile(recordPath, unusable);
        writeFile(setup.annotationPath(), setup.original);

        LoadedAnnotation annotation = setup.load();
        QVERIFY(annotation.file.document().addShape(test::reliableSaveShape()) >= 0);
        const SaveOutcome outcome = setup.coordinator().save(test::kReliableSaveImage, annotation, 1);
        QCOMPARE(saveStatusName(outcome.result.status), saveStatusName(SaveStatus::Saved));
        QVERIFY(setup.records().isEmpty());

        const QDir aside(setup.recoveryDir() + QLatin1String("/unusable"));
        const QStringList kept = aside.entryList(QDir::Files);
        QCOMPARE(kept.size(), 1);
        QCOMPARE(readFile(aside.filePath(kept.first())), unusable);
        // Not scanned again as a record of this location.
        const RecoveryScan scan = setup.recovery().scan();
        QVERIFY(scan.unusable.isEmpty());
        QVERIFY(scan.candidates.isEmpty());
    }

    // When the record cannot be moved, nothing is written.
    void unusableRecordThatCannotBeMovedStopsTheSave()
    {
        Setup setup;
        QVERIFY(setup.killDuringSave("after-record"));
        const QString recordPath = setup.recoveryDir() + QLatin1String("/") + setup.records().constFirst();
        writeFile(recordPath, "{");
        writeFile(setup.annotationPath(), setup.original);
        // A file where the folder for moved records would be.
        writeFile(setup.recoveryDir() + QLatin1String("/unusable"), "blocked");

        LoadedAnnotation annotation = setup.load();
        QVERIFY(annotation.file.document().addShape(test::reliableSaveShape()) >= 0);
        const SaveOutcome outcome = setup.coordinator().save(test::kReliableSaveImage, annotation, 1);
        QCOMPARE(saveStatusName(outcome.result.status), saveStatusName(SaveStatus::Failed));
        QVERIFY(!outcome.recoveryRecordKept);
        QCOMPARE(readFile(recordPath), QByteArray("{"));
        QCOMPARE(readFile(setup.annotationPath()), setup.original);
    }

    // Records of another project or another location are in other directories.
    void otherLocationsSeeNoRecords()
    {
        Setup setup;
        QVERIFY(setup.killDuringSave("after-record"));
        ProjectLocation moved = setup.location();
        moved.bindingId = QStringLiteral("fedcba9876543210fedcba9876543210");
        QVERIFY(RecoveryService(setup.paths(), moved).scan().candidates.isEmpty());
    }

    void lockIsExclusive()
    {
        QTemporaryDir local;
        const QString path = LocalDataPaths(local.path()).lockFile(QStringLiteral("0123456789abcdef0123456789abcdef"));
        ProjectLock first(path);
        QCOMPARE(first.tryLock(), ProjectLock::Status::Locked);
        QCOMPARE(first.tryLock(), ProjectLock::Status::Locked);

        ProjectLock second(path);
        QCOMPARE(second.tryLock(), ProjectLock::Status::HeldElsewhere);
        QCOMPARE(second.holder().pid, QCoreApplication::applicationPid());

        first.unlock();
        QCOMPARE(second.tryLock(), ProjectLock::Status::Locked);
    }

    // A lock left by a process that was killed is taken over: the project opens again after
    // a crash (format spec 8.2).
    void lockOfKilledProcessIsTakenOver()
    {
        QTemporaryDir local;
        const QString path = LocalDataPaths(local.path()).lockFile(QStringLiteral("0123456789abcdef0123456789abcdef"));
        QProcess holder;
        holder.start(QStringLiteral(LITEANNO_TEST_HELPER), {QStringLiteral("hold-lock"), path});
        QVERIFY(holder.waitForReadyRead(30000));
        QCOMPARE(holder.readAllStandardOutput().trimmed(), QByteArray("locked"));

        ProjectLock lock(path);
        QCOMPARE(lock.tryLock(), ProjectLock::Status::HeldElsewhere);
        QCOMPARE(lock.holder().pid, holder.processId());

        holder.kill();
        QVERIFY(holder.waitForFinished(30000));
        QVERIFY(QFile::exists(path));
        QCOMPARE(lock.tryLock(), ProjectLock::Status::Locked);
    }
};

QTEST_GUILESS_MAIN(TestReliableSave)
#include "tst_reliablesave.moc"
