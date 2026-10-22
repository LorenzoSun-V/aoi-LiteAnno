// Batch list and progress files (format spec 7.1, 7.2) and the digest samples of 13.2.

#include "formats/WorksetFiles.h"

#include <QTest>

using namespace liteanno::formats;

class TestWorksetFiles : public QObject
{
    Q_OBJECT

private slots:
    void itemsDigest_data()
    {
        QTest::addColumn<QStringList>("items");
        QTest::addColumn<QString>("digest");
        QTest::newRow("three paths")
            << QStringList{QStringLiteral("line1/2026-10-20/0001.bmp"), QStringLiteral("line1/2026-10-20/0002.bmp"),
                           QStringLiteral(u"划伤/样本 01.png")}
            << QStringLiteral("sha256:cd5d21128f07a26a80232ea199aa0098463e1fa03ac87f85f5e0ce6d97f27ce6");
        QTest::newRow("one path") << QStringList{QStringLiteral("a.jpg")}
                                  << QStringLiteral("sha256:509b0d4641a7c3ba088ffa28559d1f57207ea980447bc1773b1d406d788386ee");
        QTest::newRow("empty") << QStringList{}
                               << QStringLiteral("sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        // The same-looking name in two Unicode normalization forms gives two digests.
        QTest::newRow("NFC") << QStringList{QString::fromUtf8("\xC3\xA9.jpg")}
                             << QStringLiteral("sha256:b83fca42cffd3efcf3ee233eb729083ec1e189d8a06e553ff7f3cb72bafc3f12");
        QTest::newRow("NFD") << QStringList{QString::fromUtf8("e\xCC\x81.jpg")}
                             << QStringLiteral("sha256:146f2b565ba13d45845187f1cc2adfc5b5089ef37831f157adabc7f31e6d3c85");
    }

    void itemsDigest()
    {
        QFETCH(QStringList, items);
        QFETCH(QString, digest);
        QCOMPARE(computeItemsDigest(items), digest);
    }

    void listRoundTrip()
    {
        WorksetList list;
        list.projectId = QStringLiteral("p1");
        list.batchId = QStringLiteral("0123456789abcdef0123456789abcdef");
        list.number = 3;
        list.createdAt = QStringLiteral("2026-10-22T08:00:00+08:00");
        list.items = {QStringLiteral("a.jpg"), QStringLiteral(u"划伤/样本 01.png")};
        const QByteArray bytes = worksetListToBytes(list);
        QVERIFY(!bytes.endsWith('\n'));
        QVERIFY(bytes.startsWith("{\n  \"formatVersion\": \"1.0\",\n  \"projectId\": \"p1\""));
        const WorksetListResult parsed = parseWorksetList(bytes);
        QVERIFY2(parsed.list.has_value(), qPrintable(parsed.error));
        QCOMPARE(parsed.list->number, 3);
        QCOMPARE(parsed.list->items, list.items);
        QCOMPARE(parsed.list->itemsDigest, computeItemsDigest(list.items));
        QCOMPARE(worksetListToBytes(*parsed.list), bytes);
    }

    void damagedLists_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const QByteArray good = worksetListToBytes(
            {QString(), QStringLiteral("p"), QStringLiteral("b"), 1, QStringLiteral("t"), {QStringLiteral("a.jpg")}, {}});
        QTest::newRow("not JSON") << QByteArray("{");
        QTest::newRow("digest") << QByteArray(good).replace("a.jpg", "b.jpg");
        QTest::newRow("number zero") << QByteArray(good).replace("\"number\": 1", "\"number\": 0");
        QTest::newRow("number text") << QByteArray(good).replace("\"number\": 1", "\"number\": \"1\"");
        QTest::newRow("newer major") << QByteArray(good).replace("\"1.0\"", "\"2.0\"");
        QTest::newRow("empty batchId") << QByteArray(good).replace("\"batchId\": \"b\"", "\"batchId\": \"\"");
        QTest::newRow("no items") << QByteArray(good).replace("\"items\"", "\"things\"");
    }

    void damagedLists()
    {
        QFETCH(QByteArray, bytes);
        const WorksetListResult parsed = parseWorksetList(bytes);
        QVERIFY(!parsed.list.has_value());
        QVERIFY(!parsed.error.isEmpty());
    }

    void invalidItemPath()
    {
        WorksetList list{QString(), QStringLiteral("p"), QStringLiteral("b"), 1, QStringLiteral("t"),
                         {QStringLiteral("../a.jpg")}, {}};
        QVERIFY(!parseWorksetList(worksetListToBytes(list)).list.has_value());
    }

    void progressRoundTrip()
    {
        WorksetProgress progress;
        progress.batchId = QStringLiteral("b");
        progress.listDigest = computeItemsDigest({QStringLiteral("a.jpg")});
        progress.events = {{QStringLiteral("started"), QStringLiteral("2026-10-22T08:00:00+08:00")}};
        QByteArray bytes = worksetProgressToBytes(progress);
        QVERIFY(bytes.contains("\"lastItem\": null"));
        std::optional<WorksetProgress> parsed = parseWorksetProgress(bytes);
        QVERIFY(parsed.has_value());
        QVERIFY(parsed->lastItem.isEmpty());
        QCOMPARE(parsed->events.size(), 1);
        QCOMPARE(parsed->events[0].type, QStringLiteral("started"));

        progress.lastItem = QStringLiteral("a.jpg");
        bytes = worksetProgressToBytes(progress);
        parsed = parseWorksetProgress(bytes);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->lastItem, QStringLiteral("a.jpg"));
        QCOMPARE(worksetProgressToBytes(*parsed), bytes);

        QVERIFY(!parseWorksetProgress("{").has_value());
        QVERIFY(!parseWorksetProgress(QByteArray(bytes).replace("\"events\": [", "\"events\": [1, ")).has_value());
    }

    void progressKeepsUnknownFields()
    {
        // The review's steps: a newer minor version or an engineer adds optional fields; this
        // program only moves lastItem and adds an event, and keeps them.
        const QByteArray file = R"({"formatVersion": "1.1", "batchId": "b", "listDigest": "d", "engineerNote": "check line 2", )"
                                R"("lastItem": null, "events": [{"type": "started", "at": "2026-10-22T08:00:00+08:00", "operator": "li"}]})";
        std::optional<WorksetProgress> progress = parseWorksetProgress(file);
        QVERIFY(progress.has_value());
        progress->lastItem = QStringLiteral("a.jpg");
        progress->events.append({QStringLiteral("completed"), QStringLiteral("2026-10-22T09:00:00+08:00")});
        const QByteArray bytes = worksetProgressToBytes(*progress);
        QVERIFY2(bytes.contains("\"engineerNote\": \"check line 2\""), bytes.constData());
        QVERIFY2(bytes.contains("\"operator\": \"li\""), bytes.constData());
        QVERIFY2(bytes.contains("\"formatVersion\": \"1.1\""), bytes.constData());
        std::optional<WorksetProgress> again = parseWorksetProgress(bytes);
        QVERIFY(again.has_value());
        QCOMPARE(again->lastItem, QStringLiteral("a.jpg"));
        QCOMPARE(again->events.size(), 2);
        QCOMPARE(again->events[1].type, QStringLiteral("completed"));
    }
};

QTEST_GUILESS_MAIN(TestWorksetFiles)
#include "tst_worksetfiles.moc"
