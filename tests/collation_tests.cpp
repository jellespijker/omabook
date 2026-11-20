// The order names are shown in (src/collation.h): the desktop language's,
// numbers by value and case ignored, and the root collation under a C locale,
// which would otherwise collate by bytes (#121). CMakeLists.txt runs this
// suite once more under LANG=C.UTF-8.

#include <QtTest>
#include <algorithm>

#include "collation.h"

class CollationTests : public QObject {
    Q_OBJECT

private slots:
    void aCLocaleCollatesByTheRootCollation() {
        QCOMPARE(Collation::localeFor(QLocale::c()).language(), QLocale::English);
        QCOMPARE(Collation::localeFor(QLocale(QLocale::German)).language(), QLocale::German);
    }

    void namesSortByNumberAndIgnoreCaseInEveryLocale() {
        for (const QLocale &system : {QLocale::c(), QLocale(QLocale::English), QLocale(QLocale::Dutch)}) {
            QStringList names{"apple 10", "Zebra", "Apple 9", "éclair", "eagle"};
            std::ranges::sort(names, Collation::natural(system));
            QCOMPARE(names, (QStringList{"Apple 9", "apple 10", "eagle", "éclair", "Zebra"}));
        }
    }

    void theSystemLocaleSortsNamesNaturally() {
        // Whatever this test runs under, C.UTF-8 included.
        QStringList pages{"page10.png", "Page1.png", "page2.png"};
        std::ranges::sort(pages, Collation::natural());
        QCOMPARE(pages, (QStringList{"Page1.png", "page2.png", "page10.png"}));
    }
};

QTEST_MAIN(CollationTests)
#include "collation_tests.moc"
