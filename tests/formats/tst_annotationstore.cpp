// Loading and saving files on disk: "unedited files are not written" (format spec section 1),
// read-only files are never touched, changes by other programs are not overwritten
// (architecture section 8.3) and a blocked replace is retried (8.1).

#include "common/FileBlocker.h"
#include "common/TestData.h"
#include "formats/AnnotationStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#ifdef Q_OS_LINUX
#include <sys/syscall.h>

// Calls of fsync/fdatasync, counted: these take the place of the C library's.
static int syncCalls = 0;
extern "C" int fsync(int fd)
{
    ++syncCalls;
    return int(::syscall(SYS_fsync, fd));
}
extern "C" int fdatasync(int fd)
{
    ++syncCalls;
    return int(::syscall(SYS_fdatasync, fd));
}
#endif

#ifndef Q_OS_WIN
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif


using namespace liteanno;
using namespace liteanno::formats;

namespace {

const QDateTime kOldTime(QDate(2020, 1, 2), QTime(3, 4, 5), QTimeZone::utc());

struct FileState {
    QByteArray bytes;
    QDateTime modified;
};

FileState fileState(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return {file.readAll(), QFileInfo(path).lastModified()};
}

// Copies a fixture into the temporary directory and gives it an old modification time, so
// any write is visible in both the content and the time stamp.
QString placeFixture(const QTemporaryDir& dir, const char* fixture, const char* targetName)
{
    const QString target = dir.filePath(QLatin1String(targetName));
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly))
        qFatal("cannot create %s", qPrintable(target));
    file.write(test::readTestData(QStringLiteral("annotations/") + QLatin1String(fixture)));
    file.close();
    if (!file.open(QIODevice::ReadWrite) || !file.setFileTime(kOldTime, QFileDevice::FileModificationTime))
        qFatal("cannot set the modification time of %s", qPrintable(target));
    return target;
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
}

void setModified(const QString& path, const QDateTime& time)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite) || !file.setFileTime(time, QFileDevice::FileModificationTime))
        qFatal("cannot set the modification time of %s", qPrintable(path));
}

Shape rectangle(double x)
{
    Shape shape;
    shape.type = ShapeType::Rectangle;
    shape.label = QStringLiteral("scratch");
    shape.points = {QPointF(x, 10.0), QPointF(x + 20.0, 30.0)};
    return shape;
}

const ImageInfo kImage{QStringLiteral("0002.jpg"), 640, 480, 1};

} // namespace

class TestAnnotationStore : public QObject
{
    Q_OBJECT

private slots:
    void uneditedFilesAreNotWritten_data()
    {
        QTest::addColumn<QByteArray>("fixture");
        // Files a save would normalize (two-point rectangle, missing keys, BOM and CRLF) are
        // still left alone when nothing was edited.
        QTest::newRow("upstream 2.x") << QByteArray("upstream_v2_legacy.json");
        QTest::newRow("missing fields") << QByteArray("missing_fields.json");
        QTest::newRow("BOM and CRLF") << QByteArray("bom_crlf.json");
        QTest::newRow("unknown fields") << QByteArray("unknown_fields.json");
    }

    void uneditedFilesAreNotWritten()
    {
        QFETCH(QByteArray, fixture);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, fixture.constData(), "image.json");
        const FileState before = fileState(path);

        LoadedAnnotation loaded = loadAnnotationFile(path, ImageInfo{QStringLiteral("image.png"), 0, 0, 1});
        QVERIFY(!loaded.file.isReadOnly());
        const SaveResult result = saveAnnotationFile(path, loaded);
        QCOMPARE(result.status, SaveStatus::NotModified);

        const FileState after = fileState(path);
        QCOMPARE(after.bytes, before.bytes);
        QCOMPARE(after.modified, before.modified);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("image.json")});
    }

    void newDocumentsAreWrittenOnlyAfterAnEdit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("0001.json"));
        const ImageInfo image{QStringLiteral("0001.bmp"), 768, 768, 1};

        LoadedAnnotation loaded = loadAnnotationFile(path, image, {QStringLiteral("OK"), QStringLiteral("NG")});
        QVERIFY(loaded.file.isNew());
        QCOMPARE(saveAnnotationFile(path, loaded).status, SaveStatus::NotModified);
        QVERIFY(!QFile::exists(path));

        Shape rectangle;
        rectangle.type = ShapeType::Rectangle;
        rectangle.label = QStringLiteral("scratch");
        rectangle.points = {QPointF(120.5, 80.0), QPointF(260.0, 150.25)};
        QVERIFY(loaded.file.document().addShape(rectangle) >= 0);
        QVERIFY(loaded.file.document().setImageLabel(QStringLiteral("NG"), true));
        const SaveResult result = saveAnnotationFile(path, loaded);
        QCOMPARE(result.status, SaveStatus::Saved);
        QCOMPARE(fileState(path).bytes, test::readTestData(QStringLiteral("annotations/golden_new_file.json")));

        // Saved content is the new base: saving again writes nothing.
        QVERIFY(!loaded.file.needsWrite());
        QCOMPARE(saveAnnotationFile(path, loaded).status, SaveStatus::NotModified);
    }

    void editedFilesAreWritten()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        const ImageInfo image{QStringLiteral("0002.jpg"), 640, 480, 1};

        LoadedAnnotation loaded = loadAnnotationFile(path, image);
        QVERIFY(loaded.file.document().confirm());
        QCOMPARE(saveAnnotationFile(path, loaded).status, SaveStatus::Saved);

        const FileState after = fileState(path);
        QVERIFY(after.modified != kOldTime);
        QCOMPARE(after.bytes, loaded.file.toBytes());
        AnnotationFile reread = loadAnnotationFile(path, image).file;
        QVERIFY(reread.document().review().checked);
        QVERIFY(reread.diagnostics().isEmpty());
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("0002.json")});
    }

    void readOnlyFilesAreNeverWritten_data()
    {
        QTest::addColumn<QByteArray>("fixture");
        QTest::addColumn<int>("exifOrientation");
        QTest::addColumn<int>("expectedReason");
        QTest::newRow("JSON error") << QByteArray("broken/nan_coordinate.json") << 1
                                    << int(ReadOnlyReason::JsonError);
        // A complete object followed by a NUL byte and more content (format spec 4.6).
        QTest::newRow("content after NUL") << QByteArray("broken/nul_suffix.json") << 1
                                           << int(ReadOnlyReason::JsonError);
        QTest::newRow("future version") << QByteArray("future_version.json") << 1
                                        << int(ReadOnlyReason::UnsupportedFormatVersion);
        QTest::newRow("EXIF orientation") << QByteArray("upstream_v4.json") << 6
                                          << int(ReadOnlyReason::ExifOrientationUnconfirmed);
    }

    void readOnlyFilesAreNeverWritten()
    {
        QFETCH(QByteArray, fixture);
        QFETCH(int, exifOrientation);
        QFETCH(int, expectedReason);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, fixture.constData(), "image.json");
        const FileState before = fileState(path);

        LoadedAnnotation loaded = loadAnnotationFile(path, ImageInfo{QStringLiteral("image.jpg"), 0, 0, exifOrientation});
        QCOMPARE(int(loaded.file.readOnlyReason()), expectedReason);
        loaded.file.document().confirm();
        QCOMPARE(saveAnnotationFile(path, loaded).status, SaveStatus::ReadOnly);

        const FileState after = fileState(path);
        QCOMPARE(after.bytes, before.bytes);
        QCOMPARE(after.modified, before.modified);
    }

    void unreadableFileIsNotTreatedAsNew()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A directory where the annotation file should be cannot be opened as a file.
        const QString path = dir.filePath(QStringLiteral("image.json"));
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("image.json")));

        LoadedAnnotation loaded = loadAnnotationFile(path, ImageInfo{QStringLiteral("image.png"), 1, 1, 1});
        QCOMPARE(loaded.file.readOnlyReason(), ReadOnlyReason::Unreadable);
        QVERIFY(!loaded.file.isNew());
        QCOMPARE(saveAnnotationFile(path, loaded).status, SaveStatus::ReadOnly);
    }
    // Architecture section 8.3: another program's change is detected before saving and the
    // file is left as that program wrote it.
    void externalChangeIsNotOverwritten()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        LoadedAnnotation loaded = loadAnnotationFile(path, kImage);
        QVERIFY(loaded.file.document().addShape(rectangle(1.0)) >= 0);

        const QByteArray external = test::readTestData(QStringLiteral("annotations/upstream_v4.json"));
        writeFile(path, external);
        const SaveResult result = saveAnnotationFile(path, loaded);
        QCOMPARE(result.status, SaveStatus::ExternallyModified);
        QCOMPARE(fileState(path).bytes, external);
        QVERIFY(loaded.file.needsWrite());

        // "Use my version" (format spec 5.2).
        SaveOptions overwrite;
        overwrite.overwriteExternalChanges = true;
        QCOMPARE(saveAnnotationFile(path, loaded, overwrite).status, SaveStatus::Saved);
        QCOMPARE(fileState(path).bytes, loaded.file.toBytes());
    }

    // A rewrite with the same size and the same modification time, as can happen within the
    // 2-second time granularity of FAT32, is still detected.
    void sameSizeRewriteInSameSecondIsDetected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        LoadedAnnotation loaded = loadAnnotationFile(path, kImage);
        QVERIFY(loaded.file.document().confirm());

        QByteArray rewritten = fileState(path).bytes;
        const qsizetype at = rewritten.indexOf("\"label\": \"");
        QVERIFY(at >= 0);
        rewritten[at + 10] = rewritten[at + 10] == 'x' ? 'y' : 'x';
        writeFile(path, rewritten);
        setModified(path, kOldTime);
        QCOMPARE(QFileInfo(path).size(), loaded.stamp.size);
        QCOMPARE(QFileInfo(path).lastModified(), loaded.stamp.modified);

        QCOMPARE(saveAnnotationFile(path, loaded).status, SaveStatus::ExternallyModified);
        QCOMPARE(fileState(path).bytes, rewritten);
    }

    void externalCreateAndDeleteAreDetected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // A file created by another program while this one edited a new document.
        const QString created = dir.filePath(QStringLiteral("new.json"));
        LoadedAnnotation fresh = loadAnnotationFile(created, kImage);
        QVERIFY(fresh.file.document().addShape(rectangle(1.0)) >= 0);
        writeFile(created, "{}");
        QCOMPARE(saveAnnotationFile(created, fresh).status, SaveStatus::ExternallyModified);
        QCOMPARE(fileState(created).bytes, QByteArray("{}"));

        // A file deleted by another program.
        const QString deleted = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        LoadedAnnotation loaded = loadAnnotationFile(deleted, kImage);
        QVERIFY(loaded.file.document().confirm());
        QVERIFY(QFile::remove(deleted));
        QCOMPARE(saveAnnotationFile(deleted, loaded).status, SaveStatus::ExternallyModified);
        QVERIFY(!QFile::exists(deleted));
    }

    // Rewriting the same bytes (a copy, a touch) is not a change.
    void sameBytesWithNewTimeAreNotAChange()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        LoadedAnnotation loaded = loadAnnotationFile(path, kImage);
        QVERIFY(loaded.file.document().confirm());
        setModified(path, QDateTime::currentDateTimeUtc());
        QCOMPARE(saveAnnotationFile(path, loaded).status, SaveStatus::Saved);
    }

    // After a save the stamp describes this program's own write, so the next save goes through.
    void ownSavesAreNotExternalChanges()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("0001.json"));
        LoadedAnnotation loaded = loadAnnotationFile(path, kImage);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(loaded.file.document().addShape(rectangle(10.0 * i)) >= 0);
            QCOMPARE(saveAnnotationFile(path, loaded).status, SaveStatus::Saved);
            QCOMPARE(loaded.stamp.digest, contentDigest(fileState(path).bytes));
        }
    }

    // Architecture section 8.1: on Windows a virus scanner or the search indexer can keep the
    // file open for a moment, so the replace fails; it is retried and then succeeds.
    void blockedReplaceIsRetried()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        LoadedAnnotation loaded = loadAnnotationFile(path, kImage);
        QVERIFY(loaded.file.document().confirm());

        test::FileBlocker blocker(path, test::FileBlocker::Mode::Replace);
        if (!blocker.isActive())
            QSKIP("File permissions do not stop root");
        SaveOptions options;
        options.retryDelayMs = 10;
        options.beforeRetry = [&blocker](int attempt) {
            if (attempt == 3)
                blocker.release();
        };
        const SaveResult result = saveAnnotationFile(path, loaded, options);
        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::Saved));
        QCOMPARE(result.attempts, 3);
        QCOMPARE(fileState(path).bytes, loaded.file.toBytes());
    }

    // Another program writes the file while a retry waits: the next attempt sees it and stops.
    void externalChangeDuringRetryIsNotOverwritten()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        LoadedAnnotation loaded = loadAnnotationFile(path, kImage);
        QVERIFY(loaded.file.document().confirm());

        test::FileBlocker blocker(path, test::FileBlocker::Mode::Replace);
        if (!blocker.isActive())
            QSKIP("File permissions do not stop root");
        const QByteArray external = test::readTestData(QStringLiteral("annotations/upstream_v4.json"));
        SaveOptions options;
        options.retryDelayMs = 1;
        options.beforeRetry = [&](int attempt) {
            if (attempt == 2) {
                blocker.release();
                writeFile(path, external);
            }
        };
        const SaveResult result = saveAnnotationFile(path, loaded, options);
        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::ExternallyModified));
        QCOMPARE(result.attempts, 2);
        QCOMPARE(fileState(path).bytes, external);
        QVERIFY(loaded.file.needsWrite());
    }

    // A file that cannot be read for a moment is read again, and is not taken for a change.
    void unreadableFileIsCheckedAgain()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        LoadedAnnotation loaded = loadAnnotationFile(path, kImage);
        QVERIFY(loaded.file.document().confirm());

        test::FileBlocker blocker(path, test::FileBlocker::Mode::Read);
        if (!blocker.isActive())
            QSKIP("File permissions do not stop root");
        SaveOptions options;
        options.retryDelayMs = 1;
        options.beforeRetry = [&blocker](int attempt) {
            if (attempt == 2)
                blocker.release();
        };
        const SaveResult result = saveAnnotationFile(path, loaded, options);
        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::Saved));
        QCOMPARE(result.attempts, 2);
    }

    // When every attempt fails the result says so and the file keeps its old content.
    void failedReplaceLeavesTheFile_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("not replaceable") << int(test::FileBlocker::Mode::Replace);
        QTest::newRow("not readable") << int(test::FileBlocker::Mode::Read);
    }

    void failedReplaceLeavesTheFile()
    {
        QFETCH(int, mode);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = placeFixture(dir, "upstream_v2_legacy.json", "0002.json");
        const FileState before = fileState(path);
        LoadedAnnotation loaded = loadAnnotationFile(path, kImage);
        QVERIFY(loaded.file.document().confirm());

        SaveOptions options;
        options.maxAttempts = 3;
        options.retryDelayMs = 1;
        test::FileBlocker blocker(path, test::FileBlocker::Mode(mode));
        if (!blocker.isActive())
            QSKIP("File permissions do not stop root");
        const SaveResult result = saveAnnotationFile(path, loaded, options);
        blocker.release();

        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::Failed));
        QCOMPARE(result.attempts, 3);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(loaded.file.needsWrite());
        QCOMPARE(fileState(path).bytes, before.bytes);
        QCOMPARE(fileState(path).modified, before.modified);
    }

    // Review of PR #8 (second round), item 3: a project file made by another program between
    // the check and the write is kept, and the new one is not written.
    void exclusiveCreateNeverReplaces()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("project.json"));
        SaveResult result = createFileExclusively(path, "first");
        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::Saved));
        QCOMPARE(fileState(path).bytes, QByteArray("first"));
        QVERIFY(QFileInfo(path).permissions().testFlag(QFile::ReadOther));
        // Taken before the call.
        result = createFileExclusively(path, "second");
        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::ExternallyModified));
        QCOMPARE(fileState(path).bytes, QByteArray("first"));
        // Taken while the new file is being written.
        QVERIFY(QFile::remove(path));
        result = createFileExclusively(path, "mine", [&path] {
            QFile other(path);
            QVERIFY(other.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            other.write("theirs");
        });
        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::ExternallyModified));
        QCOMPARE(fileState(path).bytes, QByteArray("theirs"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("project.json")});
    }

    // Review of PR #8 (third round), item 2: the new file is on the disk before it gets its
    // name (format spec 1), as with the other writes.
    void exclusiveCreateSyncsBeforeNaming()
    {
#ifndef Q_OS_LINUX
        QSKIP("the sync calls are counted on Linux only");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("project.json"));
        syncCalls = 0;
        int beforeNaming = -1;
        const SaveResult result = createFileExclusively(path, "content", [&beforeNaming] { beforeNaming = syncCalls; });
        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::Saved));
        QVERIFY(beforeNaming >= 1);
        // The other writes sync too: the counting works.
        syncCalls = 0;
        QCOMPARE(saveStatusName(writeFileAtomically(dir.filePath(QStringLiteral("other.json")), "x").status),
                 saveStatusName(SaveStatus::Saved));
        QVERIFY(syncCalls >= 1);
#endif
    }

    // The timing of each attempt (import diagnosis): retries show their wait and where they
    // stopped; the attempt that saved has every step. Nothing else changes.
    void attemptTiming()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.json"));
        SaveTiming timing;
        SaveOptions options;
        options.retryDelayMs = 5;
        options.timing = &timing;
        int checks = 0;
        int mayWrites = 0;
        options.mayWrite = [&mayWrites] {
            ++mayWrites;
            return SaveResult{SaveStatus::Saved, QString()};
        };
        const SaveResult result = writeFileAtomically(path, "x", options, [&checks] {
            return ++checks < 3 ? SaveResult{SaveStatus::Failed, QStringLiteral("held by a scanner")}
                                : SaveResult{SaveStatus::Saved, QString()};
        });
        QCOMPARE(saveStatusName(result.status), saveStatusName(SaveStatus::Saved));
        QCOMPARE(result.attempts, 3);
        QCOMPARE(mayWrites, 1);
        QCOMPARE(timing.attempts.size(), 3);
        QCOMPARE(timing.attempts[0].waitUs, 0);
        for (int k = 0; k < 2; ++k) {
            QCOMPARE(timing.attempts[k].stoppedAt, QStringLiteral("check"));
            QCOMPARE(timing.attempts[k].error, QStringLiteral("held by a scanner"));
            QVERIFY(timing.attempts[k].checkUs >= 0);
            QCOMPARE(timing.attempts[k].mayWriteUs, -1);
            QCOMPARE(timing.attempts[k].commitUs, -1);
        }
        QVERIFY(timing.attempts[1].waitUs >= 4000);
        const AttemptTiming& last = timing.attempts[2];
        QVERIFY(last.stoppedAt.isEmpty());
        QVERIFY(last.waitUs >= 4000);
        QVERIFY(last.checkUs >= 0 && last.mayWriteUs >= 0 && last.openUs >= 0 && last.writeUs >= 0 && last.commitUs >= 0);

        // Stopped by mayWrite: recorded there, no file step.
        timing.attempts.clear();
        options.mayWrite = [] { return SaveResult{SaveStatus::ReadOnly, QStringLiteral("name collision")}; };
        QCOMPARE(saveStatusName(writeFileAtomically(path, "y", options).status), saveStatusName(SaveStatus::ReadOnly));
        QCOMPARE(timing.attempts.size(), 1);
        QCOMPARE(timing.attempts[0].stoppedAt, QStringLiteral("may-write"));
        QCOMPARE(timing.attempts[0].openUs, -1);
    }

    // A disk that fills up part way through a write (here: a file size limit) must not leave
    // the first part of the new file in place of the old one.
    void writeCutShortIsNotCommitted()
    {
#ifdef Q_OS_WIN
        QSKIP("the file size limit is set with setrlimit on Unix only");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("0001.json"));
        const QByteArray old(50, 'o');
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly) && file.write(old) == old.size());
        file.close();
        const pid_t child = ::fork();
        QVERIFY(child >= 0);
        if (child == 0) {
            ::signal(SIGXFSZ, SIG_IGN);  // the write fails instead of ending the process
            const rlimit limit{100, 100};
            ::setrlimit(RLIMIT_FSIZE, &limit);
            const SaveResult result = writeFileAtomically(path, QByteArray(400, 'n'), SaveOptions());
            ::_exit(result.status == SaveStatus::Saved ? 0 : 1);
        }
        int status = 0;
        QCOMPARE(::waitpid(child, &status, 0), child);
        QVERIFY(WIFEXITED(status));
        QCOMPARE(WEXITSTATUS(status), 1);
        QCOMPARE(fileState(path).bytes, old);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("0001.json")});
#endif
    }
};

QTEST_GUILESS_MAIN(TestAnnotationStore)
#include "tst_annotationstore.moc"
