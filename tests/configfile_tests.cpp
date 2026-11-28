// A config.json that can't be read, a trailing comma after a hand edit, is
// never written over, is said with the line of the mistake, and lets nothing
// go to a hosted model by default (#222). So for the plugins' sources and a
// plugin's own settings.
#include <QtTest>

#include "aiprivacy.h"
#include "airegistry.h"
#include "desk.h"
#include "log.h"
#include "mcpharness.h"
#include "pluginsources.h"
#include "scriptplugins.h"

using namespace Qt::StringLiterals;

namespace {

// The file of the ticket: the comma before the brace on line 7 is one too many.
const QByteArray broken = R"({
  "config_version": 3,
  "ai_provider": "openai",
  "cover_size": 260,
  "font_size": 24,
  "align": "justify",
}
)";
const QByteArray readable = R"({"config_version": 3, "ai_provider": "claude", "font_size": 24,
 "capabilities": {"answers": {"provider": "claude", "hosted": true}, "quick": {"provider": "claude", "hosted": true}}}
)";

QByteArray contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString problemOf(const Settings &settings) { return settings.property("problem").toString(); }

// Every way a change reaches the file.
void changeThings(Settings &settings) {
    AiPrivacy::carryOver(settings);
    settings.setAlign("center");
    settings.setFontSize(30);
    settings.setLibraryPath("/tmp/another-library");
    settings.setTour("done");
}

} // namespace

class ConfigFileTests : public QObject {
    Q_OBJECT

    // A home of the tests' own: nothing here reads or writes the user's settings.
    QTemporaryDir home;

private slots:
    void initTestCase() {
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", home.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", home.filePath("cache").toUtf8());
    }

    void aFileThatCantBeReadIsLeftAsItIs_data() {
        QTest::addColumn<QByteArray>("held");
        QTest::addColumn<QString>("said");
        QTest::newRow("a trailing comma") << broken << QString("line 7");
        QTest::newRow("not an object") << QByteArray("[1, 2]\n") << QString("isn't a JSON object");
        QTest::newRow("empty") << QByteArray() << QString("line 1");
    }

    void aFileThatCantBeReadIsLeftAsItIs() {
        QFETCH(const QByteArray, held);
        QFETCH(const QString, said);
        QTemporaryDir dir;
        const QString file = dir.filePath("config.json");
        QVERIFY(Fixtures::write(file, held));

        Settings settings(file);
        changeThings(settings);
        QCOMPARE(contents(file), held);
        QVERIFY2(problemOf(settings).contains(file), qPrintable(problemOf(settings)));
        QVERIFY2(problemOf(settings).contains(said), qPrintable(problemOf(settings)));
        // Beside it, as keys.json's and style.json's mistakes are.
        QVERIFY(contents(dir.filePath("config.error")).contains(said.toUtf8()));
    }

    void nothingGoesToAHostedModelWhileItCantBeRead() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("config.json"), broken));
        Settings settings(dir.filePath("config.json"));
        AiPrivacy::carryOver(settings);

        QCOMPARE(settings.aiProvider(), QString("off"));
        for (const Capability capability : Capabilities::all)
            QVERIFY2(!AiPrivacy::hostedAllowed(settings, capability), qPrintable(Capabilities::name(capability)));
        QVERIFY(!AiRegistry::answers(settings));
        QCOMPARE(settings.pluginUpdates(), QString("never"));
    }

    void fixedItIsReadAndWrittenAgain() {
        QTemporaryDir dir;
        const QString file = dir.filePath("config.json");
        QVERIFY(Fixtures::write(file, broken));
        Settings settings(file);
        QVERIFY(!problemOf(settings).isEmpty());

        QVERIFY(Fixtures::write(file, readable));
        QTRY_COMPARE(settings.aiProvider(), QString("claude"));
        QCOMPARE(problemOf(settings), QString());
        QVERIFY(!QFile::exists(dir.filePath("config.error")));
        settings.setFontSize(30);
        QCOMPARE(Settings(file).fontSize(), 30);
        QCOMPARE(Settings(file).aiProvider(), QString("claude"));
    }

    void brokenWhileOpenTheSettingsStay() {
        QTemporaryDir dir;
        const QString file = dir.filePath("config.json");
        QVERIFY(Fixtures::write(file, readable));
        Settings settings(file);

        QVERIFY(Fixtures::write(file, broken));
        QTRY_VERIFY(!problemOf(settings).isEmpty());
        QCOMPARE(settings.aiProvider(), QString("claude"));
        // The window goes on working with the change; the file isn't touched.
        settings.setFontSize(30);
        QCOMPARE(settings.fontSize(), 30);
        QCOMPARE(contents(file), broken);

        QVERIFY(Fixtures::write(file, readable));
        QTRY_COMPARE(problemOf(settings), QString());
        QCOMPARE(settings.fontSize(), 24);
    }

    void brokenAndNotSeenYetItIsStillNotWrittenOver() {
        QTemporaryDir dir;
        const QString file = dir.filePath("config.json");
        QVERIFY(Fixtures::write(file, readable));
        Settings settings(file);
        // No event has run, so the watcher has said nothing.
        QVERIFY(Fixtures::write(file, broken));
        settings.setFontSize(30);
        QCOMPARE(contents(file), broken);
        QVERIFY(!problemOf(settings).isEmpty());
    }

    void takenAwayItStartsAsTheFirstTime() {
        QTemporaryDir dir;
        const QString file = dir.filePath("config.json");
        QVERIFY(Fixtures::write(file, broken));
        Settings settings(file);
        QVERIFY(QFile::remove(file));
        QTRY_COMPARE(problemOf(settings), QString());
        for (const Capability capability : Capabilities::all)
            QVERIFY(!AiPrivacy::hostedAllowed(settings, capability));
        settings.setFontSize(30);
        QCOMPARE(Settings(file).fontSize(), 30);
    }

    void theLogSaysItOnce() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("config.json"), broken));
        Log::install("window", dir.filePath("state"));
        const Settings first(dir.filePath("config.json"));
        const Settings second(dir.filePath("config.json"));
        const auto said = Log::read({.least = "warning", .words = dir.filePath("config.json")}, dir.filePath("state"));
        QCOMPARE(said.size(), 1);
        QVERIFY2(said.first().message.contains("line 7"), qPrintable(said.first().message));
    }

    void aCommandSaysSoAndFailsToChangeIt() {
        const McpHarness::Machine machine;
        const QString file = machine.dir.filePath("home/.config/omabook/config.json");
        QVERIFY(Fixtures::write(file, broken));

        const auto info = machine.omabook({"info"});
        QCOMPARE(info.code, 0);
        QVERIFY2(info.errors.contains("line 7"), qPrintable(info.errors));
        const auto format = machine.omabook({"format", "--align", "center", "--library", machine.library});
        QVERIFY(format.code != 0);
        QVERIFY2(format.errors.contains("The change was not saved."), qPrintable(format.errors));
        QCOMPARE(contents(file), broken);
        const auto logs = machine.omabook({"logs", "--lines", "50"});
        QVERIFY2(logs.output.contains("line 7"), qPrintable(logs.output));
    }

    void aToolThatWouldChangeItFails() {
        const McpHarness::Machine machine;
        // A library to work on, there before the settings go wrong.
        QCOMPARE(machine.omabook({"list", "--library", machine.library}).code, 0);
        const QString file = machine.dir.filePath("home/.config/omabook/config.json");
        QVERIFY(Fixtures::write(file, broken));
        McpHarness::Server server(machine);

        const QString switched = server.failure("plugin_set_enabled", {{"id", "builtin:Crossref"}, {"enabled", false}});
        QVERIFY2(switched.contains("line 7"), qPrintable(switched));
        const QString changed =
            server.failure("customize_set", {{"settings", QJsonObject{{"font_size", 30}}}, {"style", QJsonObject{{"radius", 3}}}});
        QVERIFY2(changed.contains("line 7"), qPrintable(changed));
        // Nothing of a change that can't be made whole is made.
        QVERIFY(!QFile::exists(machine.dir.filePath("home/.config/omabook/style.json")));
        QCOMPARE(contents(file), broken);
        // A tool that is one of Omabook's commands fails as the command does (#275).
        const QString formatted = server.failure("reading_format_set", {{"font_size", 30}});
        QVERIFY2(formatted.contains("not saved") && formatted.contains("line 7"), qPrintable(formatted));
        const QString left = server.failure("reading_site_disconnect", {{"site", "openlibrary"}});
        QVERIFY2(left.contains("not saved") && left.contains("line 7"), qPrintable(left));
        QCOMPARE(contents(file), broken);
        // One that changes no setting still answers.
        QVERIFY(server.failure("reading_format_get").isEmpty());
    }

    void theWindowSaysSo() {
        Desk desk("en_US", {}, false);
        QVERIFY(desk.show());
        QVERIFY(!desk.shows("settingsProblem"));
        const QString file = desk.dir.filePath("config.json");
        const QByteArray held = contents(file);
        QVERIFY(Fixtures::write(file, broken));
        QTRY_VERIFY(desk.shows("settingsProblem"));
        const QString said = desk.find("settingsProblemText")->property("text").toString();
        QVERIFY2(said.contains("line 7"), qPrintable(said));
        QVERIFY(Fixtures::write(file, held));
        QTRY_VERIFY(!desk.shows("settingsProblem"));
        QCOMPARE(desk.warnings, QStringList());
    }

    void thePluginsSourcesAreLeftAsTheyAre() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const QString file = dir.filePath("plugins/sources.json");
        const QByteArray held = "{\"version\": 1, \"sources\": {\"storyteller\": {\"repository\": \"a/b\"},}}\n";
        QVERIFY(QDir().mkpath(dir.filePath("plugins")));
        QVERIFY(Fixtures::write(file, held));
        PluginSources sources({dir.filePath("plugins"), &settings, {}});

        const QString added =
            PluginSources::wait([&](PluginSources::Done done) { sources.add("someone/other", {}, done); }, 10000).value("error").toString();
        QVERIFY2(added.contains("sources.json can't be read: line 1"), qPrintable(added));
        const QString forgotten = sources.forget("storyteller").value("error").toString();
        QVERIFY2(forgotten.contains("sources.json can't be read: line 1"), qPrintable(forgotten));
        QCOMPARE(contents(file), held);
    }

    void aPluginsOwnSettingsAreLeftAsTheyAre() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const QString plugin = dir.filePath("plugins/keeper");
        const QByteArray held = "{\"name\": \"kept\",}\n";
        QVERIFY(QDir().mkpath(plugin + "/data"));
        QVERIFY(Fixtures::write(plugin + "/plugin.json", "{\"name\": \"Keeper\", \"script\": \"main.js\"}"));
        QVERIFY(Fixtures::write(plugin + "/main.js", "omabook.settings.set(\"added\", 1);\n"));
        QVERIFY(Fixtures::write(plugin + "/data/settings.json", held));
        ScriptServices services;
        services.settings = &settings;

        const ScriptPlugins plugins(services, dir.filePath("plugins"), nullptr, false);
        QCOMPARE(contents(plugin + "/data/settings.json"), held);
        QCOMPARE(plugins.list().size(), 1);
        QVERIFY2(plugins.list().first().problem.contains("can't be read"), qPrintable(plugins.list().first().problem));
    }
};

QTEST_MAIN(ConfigFileTests)
#include "configfile_tests.moc"
