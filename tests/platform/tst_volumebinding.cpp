// The bindingId of a data root (format spec 8).

#include "platform/VolumeBinding.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

using namespace liteanno::platform;

class TestVolumeBinding : public QObject
{
    Q_OBJECT

private slots:
    void digestOfVolumeAndPath()
    {
        const QString id = computeBindingId(QStringLiteral("1a2b3c4d"), QStringLiteral("/data/lineA"));
        QCOMPARE(id.size(), 32);
        QCOMPARE(id, computeBindingId(QStringLiteral("1a2b3c4d"), QStringLiteral("/data/lineA")));
        QVERIFY(id != computeBindingId(QStringLiteral("1a2b3c4e"), QStringLiteral("/data/lineA")));
        QVERIFY(id != computeBindingId(QStringLiteral("1a2b3c4d"), QStringLiteral("/data/lineB")));
    }

    void directoriesOnDisk()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("a/b")));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("c")));

        const VolumeLocation location = locateOnVolume(dir.filePath(QStringLiteral("a/b")));
        QVERIFY2(location.ok, qPrintable(location.error));
        QVERIFY(!location.volumeId.isEmpty());
        QVERIFY(location.pathInVolume.startsWith(QLatin1Char('/')));
        QVERIFY(location.pathInVolume.endsWith(QLatin1String("/b")));
        QVERIFY(!location.pathInVolume.contains(QLatin1Char(':')));

        const QString ab = bindingIdForDirectory(dir.filePath(QStringLiteral("a/b")));
        QCOMPARE(ab.size(), 32);
        // The same directory written another way.
        QCOMPARE(bindingIdForDirectory(dir.filePath(QStringLiteral("a/./b/../b/"))), ab);
        QVERIFY(bindingIdForDirectory(dir.filePath(QStringLiteral("c"))) != ab);
#ifdef Q_OS_WIN
        QCOMPARE(bindingIdForDirectory(dir.filePath(QStringLiteral("A/B"))), ab);
#endif
    }

    void missingDirectory()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(bindingIdForDirectory(dir.filePath(QStringLiteral("missing")), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestVolumeBinding)
#include "tst_volumebinding.moc"
