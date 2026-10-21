// itemsDigest / listDigest samples of format spec 13.2.

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
};

QTEST_GUILESS_MAIN(TestWorksetFiles)
#include "tst_worksetfiles.moc"
