// Bringing a library in an older format up to date before the window opens
// it (#54): it runs on a worker, and a window says so while it does.

#include <QColor>
#include <QDir>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "library.h"
#include "librarymigrations.h"
#include "libraryupgrade.h"
#include "samples.h"

class UpgradingTests : public QObject {
    Q_OBJECT

private slots:
    void theWindowSaysSoWhileALibraryIsBroughtUpToDate() {
        const QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        QVERIFY(QDir().mkpath(root));
        int format = 0;
        Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), root + "/omabook.db", format);
        QVERIFY(format < LibraryMigrations::currentFormat());
        QVERIFY(LibraryUpgrade::due(root));

        QQmlPropertyMap theme;
        theme.insert("background", QColor("#1e1e2e"));
        theme.insert("foreground", QColor("#cdd6f4"));
        LibraryUpgrade upgrade;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("upgrade", &upgrade);
        QQmlComponent component(&engine, QUrl("qrc:/Upgrading.qml"));
        const std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        const auto *message = window->findChild<QQuickItem *>("upgradeMessage");
        QVERIFY(message);
        QVERIFY(!message->isVisible());

        // Said from the moment it starts until it is done.
        const QSignalSpy finished(&upgrade, &LibraryUpgrade::finished);
        QVERIFY(upgrade.start(root));
        QVERIFY(upgrade.running());
        QVERIFY(message->isVisible());
        QCOMPARE(message->property("text").toString(), QString("Bringing your library up to date…"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 60'000);
        QVERIFY(!upgrade.running());
        QVERIFY(!message->isVisible());

        // Done: in the newest format, and nothing more to do.
        QVERIFY(!LibraryUpgrade::due(root));
        Library library(root);
        QVERIFY(library.open());
        QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
        QVERIFY(!upgrade.start(root));
    }

    void aNewLibraryIsNotBroughtUpToDate() {
        const QTemporaryDir dir;
        QVERIFY(!LibraryUpgrade::due(dir.filePath("None")));
        {
            Library library(dir.filePath("New"));
            QVERIFY(library.open());
        }
        QVERIFY(!LibraryUpgrade::due(dir.filePath("New")));
        LibraryUpgrade upgrade;
        QVERIFY(!upgrade.start(dir.filePath("New")));
        QVERIFY(!upgrade.running());
    }
};

QTEST_MAIN(UpgradingTests)
#include "upgrading_tests.moc"
