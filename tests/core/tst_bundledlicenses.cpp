#include "core/BundledLicenses.h"

#include <QtTest>

using namespace liteanno;

// Built with tests/data/bundled-licenses as ":/licenses": every required text but
// libjpeg-COPYRIGHT.txt, and a zlib-LICENSE holding only white space.
class TestBundledLicenses : public QObject
{
    Q_OBJECT

private slots:
    void namesAreSorted()
    {
        const QStringList names = bundledLicenseNames();
        QCOMPARE(names.size(), 7);
        QStringList sorted = names;
        sorted.sort();
        QCOMPARE(names, sorted);
        QVERIFY(names.contains(QStringLiteral("qt-LGPL-3.0-only.txt")));
    }

    void textsAreReadByName()
    {
        QCOMPARE(bundledLicenseText(QStringLiteral("opencv-LICENSE")), QStringLiteral("text of opencv-LICENSE\n"));
        QVERIFY(bundledLicenseText(QStringLiteral("no-such-LICENSE")).isEmpty());
        QVERIFY(bundledLicenseText(QString()).isEmpty());
    }

    void missingAndEmptyRequiredTextsAreReported()
    {
        QCOMPARE(missingRequiredLicenses(),
                 (QStringList{QStringLiteral("libjpeg-COPYRIGHT.txt"), QStringLiteral("zlib-LICENSE")}));
    }
};

QTEST_GUILESS_MAIN(TestBundledLicenses)
#include "tst_bundledlicenses.moc"
