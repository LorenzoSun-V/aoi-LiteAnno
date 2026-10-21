// Recovery records (format spec 8.1).

#include "common/TestData.h"
#include "formats/RecoveryRecord.h"

#include <QTest>

using namespace liteanno;
using namespace liteanno::formats;

namespace {

RecoveryRecord sample()
{
    RecoveryRecord record;
    record.formatVersion = QStringLiteral("1.0");
    record.projectId = QStringLiteral("3f6c2a9e-8b1d-4c57-9a0e-2d4b7f1e6c33");
    record.bindingId = QStringLiteral("0123456789abcdef0123456789abcdef");
    record.rootPathAtWrite = QStringLiteral("E:/data/lineA");
    record.imagePath = QStringLiteral("line1/2026-10-20/0123.bmp");
    record.documentVersion = 17;
    record.baseDigest = QStringLiteral("sha256:") + QString(64, QLatin1Char('a'));
    record.createdAt = QStringLiteral("2026-10-20T15:20:03+08:00");
    record.annotation = test::readTestData(QStringLiteral("annotations/golden_new_file.json"));
    return record;
}

} // namespace

class TestRecoveryRecord : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip_data()
    {
        QTest::addColumn<bool>("hasBase");
        QTest::newRow("with base") << true;
        QTest::newRow("no file before") << false;
    }

    void roundTrip()
    {
        QFETCH(bool, hasBase);
        RecoveryRecord record = sample();
        if (!hasBase)
            record.baseDigest.clear();
        const std::optional<QByteArray> bytes = recoveryRecordToBytes(record);
        QVERIFY(bytes);

        const Json json = test::parseOrFail(*bytes);
        QCOMPARE(json["baseDigest"].is_null(), !hasBase);
        QVERIFY(json["annotation"].is_object());
        QCOMPARE(json["documentVersion"].get<int>(), 17);

        const RecoveryRecordResult parsed = parseRecoveryRecord(*bytes);
        QVERIFY2(parsed.record, qPrintable(parsed.error));
        QCOMPARE(parsed.record->projectId, record.projectId);
        QCOMPARE(parsed.record->bindingId, record.bindingId);
        QCOMPARE(parsed.record->rootPathAtWrite, record.rootPathAtWrite);
        QCOMPARE(parsed.record->imagePath, record.imagePath);
        QCOMPARE(parsed.record->documentVersion, record.documentVersion);
        QCOMPARE(parsed.record->baseDigest, record.baseDigest);
        QCOMPARE(parsed.record->createdAt, record.createdAt);
        // The annotation comes back as the exact bytes that would be written.
        QCOMPARE(parsed.record->annotation, record.annotation);
    }

    void annotationMustBeAnObject()
    {
        RecoveryRecord record = sample();
        record.annotation = "[1]";
        QVERIFY(!recoveryRecordToBytes(record));
        record.annotation = "{";
        QVERIFY(!recoveryRecordToBytes(record));
    }

    void invalidRecords_data()
    {
        QTest::addColumn<QByteArray>("from");
        QTest::addColumn<QByteArray>("to");
        QTest::newRow("newer version") << QByteArray(R"("formatVersion": "1.0")") << QByteArray(R"("formatVersion": "2.0")");
        QTest::newRow("no project") << QByteArray(R"("projectId")") << QByteArray(R"("project")");
        QTest::newRow("version text") << QByteArray(R"("documentVersion": 17)") << QByteArray(R"("documentVersion": "17")");
        QTest::newRow("base number") << QByteArray(R"("baseDigest": "sha256:)") << QByteArray(R"("baseDigest": 1, "x": ")");
        QTest::newRow("annotation null") << QByteArray(R"("annotation": {)") << QByteArray(R"("annotation": null, "x": {)");
    }

    void invalidRecords()
    {
        QFETCH(QByteArray, from);
        QFETCH(QByteArray, to);
        QByteArray bytes = *recoveryRecordToBytes(sample());
        QVERIFY(bytes.contains(from));
        bytes.replace(from, to);
        const RecoveryRecordResult parsed = parseRecoveryRecord(bytes);
        QVERIFY(!parsed.record);
        QVERIFY(!parsed.error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestRecoveryRecord)
#include "tst_recoveryrecord.moc"
