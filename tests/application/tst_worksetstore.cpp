// Batches on disk (format spec 7): creation, numbering, damaged lists and progress.

#include "application/WorksetStore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using liteanno::application::WorksetStore;
namespace formats = liteanno::formats;

namespace {

void writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList images(int count)
{
    QStringList paths;
    for (int i = 1; i <= count; ++i)
        paths.append(QStringLiteral("img/%1.jpg").arg(i, 4, 10, QLatin1Char('0')));
    return paths;
}

} // namespace

class TestWorksetStore : public QObject
{
    Q_OBJECT

private slots:
    void noFolderMeansNoBatch()
    {
        QTemporaryDir root;
        WorksetStore store(root.path(), QStringLiteral("p"));
        QVERIFY(store.load());
        QVERIFY(store.batches().isEmpty());
        QCOMPARE(store.lastWorkedOn(), nullptr);
    }

    void createsBatchesInOrder()
    {
        QTemporaryDir root;
        WorksetStore store(root.path(), QStringLiteral("p"));
        const QStringList all = images(5);
        const auto first = store.createNext(all, 2);
        QVERIFY2(first.batch, qPrintable(first.error));
        QCOMPARE(first.batch->number, 1);
        QCOMPARE(first.batch->stem, QStringLiteral("b000001"));
        QCOMPARE(first.batch->list->items, all.mid(0, 2));
        QCOMPARE(first.batch->list->batchId.size(), 32);

        const auto second = store.createNext(all, 2);
        QVERIFY(second.batch);
        QCOMPARE(second.batch->number, 2);
        QCOMPARE(second.batch->list->items, all.mid(2, 2));
        QVERIFY(second.batch->list->batchId != store.find(QStringLiteral("b000001"))->list->batchId);
        const auto third = store.createNext(all, 2);
        QCOMPARE(third.batch->list->items, all.mid(4, 1));
        const auto none = store.createNext(all, 2);
        QCOMPARE(none.batch, nullptr);
        QVERIFY(none.error.isEmpty());

        // A new store reads the same, from the files only.
        WorksetStore again(root.path(), QStringLiteral("p"));
        QVERIFY(again.load());
        QCOMPARE(again.batches().size(), 3);
        QVERIFY(again.isAssigned(all[4]));
        QVERIFY(again.duplicates().isEmpty());

        // The list file is the format of spec 7.1, with a progress file that started it.
        const QByteArray bytes = readBytes(root.path() + QStringLiteral("/liteanno/worksets/b000001.list.json"));
        const formats::WorksetListResult parsed = formats::parseWorksetList(bytes);
        QVERIFY(parsed.list);
        QCOMPARE(parsed.list->projectId, QStringLiteral("p"));
        const auto progress = again.progress(again.batches().first());
        QVERIFY(progress);
        QCOMPARE(progress->events.size(), 1);
        QCOMPARE(progress->events.first().type, QStringLiteral("started"));
        // No temporary files are left behind.
        QCOMPARE(QDir(store.directory()).entryList(QDir::Files).size(), 6);
    }

    void numberFollowsTheHighestList()
    {
        QTemporaryDir root;
        WorksetStore store(root.path(), QStringLiteral("p"));
        // An engineer's script made batch 37; a damaged file named b000040 also counts.
        formats::WorksetList list{QString(), QStringLiteral("p"), QStringLiteral("x"), 37, QStringLiteral("t"),
                                  {QStringLiteral("img/0001.jpg")}, {}};
        writeBytes(store.directory() + QStringLiteral("/b000037.list.json"), formats::worksetListToBytes(list));
        writeBytes(store.directory() + QStringLiteral("/b000040.list.json"), "{\"items\": [\"img/0002.jpg\"]}");
        const auto created = store.createNext(images(4), 10);
        QVERIFY(created.batch);
        QCOMPARE(created.batch->number, 41);
        // Images of the damaged list are not given out again.
        QCOMPARE(created.batch->list->items, (QStringList{QStringLiteral("img/0003.jpg"), QStringLiteral("img/0004.jpg")}));
        const WorksetStore::Batch* damaged = store.find(QStringLiteral("b000040"));
        QVERIFY(damaged);
        QVERIFY(!damaged->list);
        QVERIFY(!damaged->error.isEmpty());
    }

    void damagedLists()
    {
        QTemporaryDir root;
        WorksetStore store(root.path(), QStringLiteral("p"));
        formats::WorksetList other{QString(), QStringLiteral("other"), QStringLiteral("x"), 1, QStringLiteral("t"),
                                   {QStringLiteral("img/0001.jpg")}, {}};
        writeBytes(store.directory() + QStringLiteral("/b000001.list.json"), formats::worksetListToBytes(other));
        formats::WorksetList edited{QString(), QStringLiteral("p"), QStringLiteral("y"), 2, QStringLiteral("t"),
                                    {QStringLiteral("img/0002.jpg")}, {}};
        writeBytes(store.directory() + QStringLiteral("/b000002.list.json"),
                   formats::worksetListToBytes(edited).replace("img/0002.jpg", "img/0003.jpg"));
        QVERIFY(store.load());
        QCOMPARE(store.batches().size(), 2);
        QVERIFY(!store.batches()[0].list);
        QVERIFY(store.batches()[0].error.contains(QStringLiteral("projectId")));
        QVERIFY(!store.batches()[1].list);
        QVERIFY(store.isAssigned(QStringLiteral("img/0001.jpg")));
        QVERIFY(store.isAssigned(QStringLiteral("img/0003.jpg")));
        QCOMPARE(store.lastWorkedOn(), nullptr);
    }

    void duplicatesAreReported()
    {
        QTemporaryDir root;
        WorksetStore store(root.path(), QStringLiteral("p"));
        for (int n : {1, 2}) {
            formats::WorksetList list{QString(), QStringLiteral("p"), QStringLiteral("id%1").arg(n), n,
                                      QStringLiteral("t"), {QStringLiteral("img/0001.jpg")}, {}};
            writeBytes(store.directory() + QStringLiteral("/b00000%1.list.json").arg(n), formats::worksetListToBytes(list));
        }
        QVERIFY(store.load());
        QCOMPARE(store.duplicates(), QStringList{QStringLiteral("img/0001.jpg")});
    }

    void progressMustMatchItsList()
    {
        QTemporaryDir root;
        WorksetStore store(root.path(), QStringLiteral("p"));
        const auto created = store.createNext(images(3), 3);
        QVERIFY(created.batch);
        formats::WorksetProgress progress = *store.progress(*created.batch);
        progress.lastItem = QStringLiteral("img/0002.jpg");
        QCOMPARE(store.writeProgress(*created.batch, progress).status, formats::SaveStatus::Saved);
        QCOMPARE(store.progress(*created.batch)->lastItem, QStringLiteral("img/0002.jpg"));

        // Another list's progress, or a damaged file, only loses the position.
        progress.listDigest = formats::computeItemsDigest({QStringLiteral("x.jpg")});
        QCOMPARE(store.writeProgress(*created.batch, progress).status, formats::SaveStatus::Saved);
        QVERIFY(!store.progress(*created.batch));
        writeBytes(store.directory() + QStringLiteral("/b000001.progress.json"), "{");
        QVERIFY(!store.progress(*created.batch));
        QCOMPARE(store.lastWorkedOn(), created.batch);
    }

    void lastWorkedOnIsTheNewestProgress()
    {
        QTemporaryDir root;
        WorksetStore store(root.path(), QStringLiteral("p"));
        QVERIFY(store.createNext(images(4), 2).batch);
        QVERIFY(store.createNext(images(4), 2).batch);
        QCOMPARE(store.lastWorkedOn()->number, 2);
        const WorksetStore::Batch& first = store.batches().first();
        formats::WorksetProgress progress = *store.progress(first);
        progress.lastItem = first.list->items.last();
        QTest::qWait(20);
        QFile file(store.directory() + QStringLiteral("/b000001.progress.json"));
        QCOMPARE(store.writeProgress(first, progress).status, formats::SaveStatus::Saved);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
        file.close();
        QCOMPARE(store.lastWorkedOn()->number, 1);
    }
};

QTEST_GUILESS_MAIN(TestWorksetStore)
#include "tst_worksetstore.moc"
