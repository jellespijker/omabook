// Bringing a library in an older format up to date before the window opens
// it (#54): it runs on a worker, and a window says so while it does. The
// first command, or window, after an update takes about as long with a
// plugin installed as without (#60).

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "fixtures.h"
#include "library.h"
#include "librarymigrations.h"
#include "libraryupgrade.h"
#include "samples.h"

namespace {

// A plugin that opens the library as it loads, as Storyteller does, and
// keeps how many books it found there.
constexpr const char *opener = R"(omabook.settings.set("books", omabook.library.list().length);)";

// A home of its own whose library in use (~/Books) is the 0.1.5 sample, in
// format 12, with the opener installed or not.
[[nodiscard]] bool makeHome(const QString &home, bool withPlugin) {
    int format = 0;
    if (!QDir().mkpath(home + "/Books") || !QDir().mkpath(home + "/.config/omabook"))
        return false;
    Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), home + "/Books/omabook.db", format);
    if (format != 12)
        return false;
    const QString plugin = home + "/.config/omabook/plugins/opener";
    const QJsonObject manifest{{"name", "Opener"}, {"version", "1.0"}, {"script", "main.js"}};
    return !withPlugin
           || (QDir().mkpath(plugin) && Fixtures::write(plugin + "/plugin.json", QJsonDocument(manifest).toJson())
               && Fixtures::write(plugin + "/main.js", opener));
}

// How many books the opener found, or -1 when it didn't run.
[[nodiscard]] int booksTheOpenerFound(const QString &home) {
    QFile kept(home + "/.config/omabook/plugins/opener/data/settings.json");
    return kept.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(kept.readAll()).object().value("books").toInt(-1) : -1;
}

struct Ran {
    bool finished = false;
    qint64 took = 0;
    QString printed, said; // said: standard error, and what the log has
};

// Runs omabook in that home, as the first start after an update there.
[[nodiscard]] Ran run(const QString &home, const QStringList &arguments) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("HOME", home);
    environment.insert("XDG_CONFIG_HOME", home + "/.config");
    environment.insert("XDG_DATA_HOME", home + "/.local/share");
    environment.insert("XDG_CACHE_HOME", home + "/.cache");
    environment.insert("XDG_STATE_HOME", home + "/.local/state");
    QProcess omabook;
    omabook.setProcessEnvironment(environment);
    QElapsedTimer timer;
    timer.start();
    omabook.start(OMABOOK_BINARY, arguments);
    // Far less than the two minutes it waited for itself (#60).
    Ran ran;
    ran.finished = omabook.waitForFinished(45'000) && omabook.exitStatus() == QProcess::NormalExit && omabook.exitCode() == 0;
    ran.took = timer.elapsed();
    if (omabook.state() != QProcess::NotRunning) {
        omabook.kill();
        omabook.waitForFinished();
    }
    ran.printed = QString::fromUtf8(omabook.readAllStandardOutput());
    ran.said = QString::fromUtf8(omabook.readAllStandardError());
    if (QFile log(home + "/.local/state/omabook/omabook.log"); log.open(QIODevice::ReadOnly))
        ran.said += QString::fromUtf8(log.readAll());
    return ran;
}

// The first start in a home without the plugin and in one with it: both
// done, about as soon, and nothing waited for "another program".
[[nodiscard]] QString compare(const Ran &without, const Ran &with) {
    if (!without.finished)
        return "Without the plugin it didn't finish: " + without.said;
    if (!with.finished)
        return QString("With the plugin it didn't finish in %1 ms: %2").arg(with.took).arg(with.said);
    if (with.said.contains("another program") || with.said.contains("was stopped"))
        return "With the plugin: " + with.said;
    if (with.took > without.took + 5000)
        return QString("%1 ms with the plugin, %2 ms without").arg(with.took).arg(without.took);
    return {};
}

} // namespace

class UpgradingTests : public QObject {
    Q_OBJECT

private slots:
    // On two computers with Storyteller installed the first `omabook list`
    // after an update took two minutes: setting the library up ran the
    // plugin, which opened the library and waited for the lock its own
    // program held (#60). Listing books runs no plugin.
    void theFirstCommandAfterAnUpdateDoesNotWaitForAPlugin() {
        const QTemporaryDir dir;
        QVERIFY(makeHome(dir.filePath("without"), false));
        QVERIFY(makeHome(dir.filePath("with"), true));
        const Ran without = run(dir.filePath("without"), {"list"});
        const Ran with = run(dir.filePath("with"), {"list"});
        const QString differs = compare(without, with);
        QVERIFY2(differs.isEmpty(), qPrintable(differs));
        QCOMPARE(with.printed, without.printed);
        QVERIFY(!with.printed.isEmpty());
        QCOMPARE(booksTheOpenerFound(dir.filePath("with")), -1);
        Library library(dir.filePath("with/Books"));
        QVERIFY(library.open());
        QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
    }

    // The window brings it up to date on a worker before it opens it (#54),
    // and starts the plugins on the library brought up to date.
    void theFirstWindowAfterAnUpdateDoesNotWaitForAPlugin() {
        const QTemporaryDir dir;
        QVERIFY(makeHome(dir.filePath("without"), false));
        QVERIFY(makeHome(dir.filePath("with"), true));
        const Ran without = run(dir.filePath("without"), {"open", "--screenshot", dir.filePath("without.png")});
        const Ran with = run(dir.filePath("with"), {"open", "--screenshot", dir.filePath("with.png")});
        const QString differs = compare(without, with);
        QVERIFY2(differs.isEmpty(), qPrintable(differs));
        QCOMPARE(booksTheOpenerFound(dir.filePath("with")), 3);
    }

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
