#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "calibreplugin.h"
#include "fixtures.h"
#include "mcp.h"
#include "pluginmanager.h"
#include "plugins.h"
#include "plugintools.h"
#include "scriptplugins.h"
#include "scripttools.h"
#include "settings.h"

namespace {

// A picker that answers with whatever the test set, at once.
class FakePicker : public FilePicker {
public:
    QList<QUrl> next;
    QList<Purpose> asked;
    void pick(Purpose purpose, const QString &) override {
        asked << purpose;
        if (!next.isEmpty())
            emit picked(purpose, next);
    }
};

const char *const scriptManifest = "{\"name\": \"Reading Log\", \"version\": \"1.2\", \"author\": \"Someone\", "
                                   "\"description\": \"Keeps a list.\", \"script\": \"main.js\", \"permissions\": [\"http\", \"files\"]}";
const char *const script = "omabook.on(\"book.added\", function (event) {});\n";

// A script plugin as a folder someone might hand over.
bool scriptFolder(const QString &folder) {
    return QDir().mkpath(folder) && Fixtures::write(folder + "/plugin.json", scriptManifest)
           && Fixtures::write(folder + "/main.js", script);
}

// Everything in the plugin folder, hidden or not: what an install left behind.
QStringList leftIn(const QString &folder) {
    return QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}

QJsonObject byId(const QJsonArray &overview, const QString &id) {
    for (const QJsonValue &value : overview)
        if (value.toObject().value("id").toString() == id)
            return value.toObject();
    return {};
}

Mcp::Result callTool(const QString &name, const QJsonObject &arguments) {
    for (const Mcp::Tool &tool : Mcp::tools())
        if (tool.name == name)
            return tool.handler(arguments);
    return Mcp::Result::fail("There is no tool " + name);
}

} // namespace

class PluginManagerTests : public QObject {
    Q_OBJECT

    // A home of the tests' own: nothing here reads or writes the user's
    // settings, plugins, trash or Calibre.
    QTemporaryDir home;

private slots:
    void initTestCase() {
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", home.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", home.filePath("cache").toUtf8());
        qputenv("CALIBRE_CONFIG_DIRECTORY", home.filePath("no-calibre").toUtf8());
    }

    void aPluginIsInstalledFromAFolder() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const PluginTools::Context context{dir.filePath("plugins"), &settings, {}};
        QVERIFY(scriptFolder(dir.filePath("Reading Log")));
        // What a repository keeps of itself is not part of the plugin.
        QVERIFY(QDir(dir.path()).mkpath("Reading Log/.git"));
        QVERIFY(Fixtures::write(dir.filePath("Reading Log/.git/config"), "[core]\n"));

        // Looked at first: what it is, and what it asks to be allowed.
        const QJsonObject seen = PluginTools::inspect(context, dir.filePath("Reading Log"));
        QVERIFY2(!ScriptTools::failed(seen), qPrintable(seen.value("error").toString()));
        QCOMPARE(seen.value("name").toString(), QString("reading-log"));
        QCOMPARE(seen.value("title").toString(), QString("Reading Log"));
        QCOMPARE(seen.value("kind").toString(), QString("script"));
        QCOMPARE(seen.value("permissions").toVariant().toStringList(), (QStringList{"http", "files"}));
        QCOMPARE(seen.value("installed").toBool(), false);
        // Looking put nothing in the plugin folder.
        QVERIFY(leftIn(dir.filePath("plugins")).isEmpty());

        const QJsonObject installed = PluginTools::install(context, dir.filePath("Reading Log"));
        QVERIFY2(!ScriptTools::failed(installed), qPrintable(installed.value("error").toString()));
        QCOMPARE(installed.value("id").toString(), QString("folder:reading-log"));
        QCOMPARE(leftIn(dir.filePath("plugins")), QStringList{"reading-log"});
        QCOMPARE(leftIn(dir.filePath("plugins/reading-log")), (QStringList{"main.js", "plugin.json"}));
        // It is off until someone who saw its permissions switches it on.
        QCOMPARE(installed.value("enabled").toBool(), false);
        QVERIFY(!settings.pluginEnabled("folder:reading-log", true));
        // And it is a plugin that loads.
        QVERIFY(ScriptTools::check(context, "reading-log").value("ok").toBool());

        // A second one of that name is refused unless it is to replace the
        // first, and then what the first kept of its own stays.
        QVERIFY(QDir(dir.path()).mkpath("plugins/reading-log/data"));
        QVERIFY(Fixtures::write(dir.filePath("plugins/reading-log/data/list.json"), "[1, 2]"));
        QVERIFY(PluginTools::install(context, dir.filePath("Reading Log")).value("error").toString().contains("already"));
        PluginTools::InstallOptions again;
        again.replace = again.enable = true;
        const QJsonObject replaced = PluginTools::install(context, dir.filePath("Reading Log"), again);
        QVERIFY2(!ScriptTools::failed(replaced), qPrintable(replaced.value("error").toString()));
        QVERIFY(replaced.value("replaced").toBool());
        QVERIFY(settings.pluginEnabled("folder:reading-log", false));
        QVERIFY(QFile::exists(dir.filePath("plugins/reading-log/data/list.json")));
        QCOMPARE(leftIn(dir.filePath("plugins")), QStringList{"reading-log"});
    }

    void aPluginIsInstalledFromAZip() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const PluginTools::Context context{dir.filePath("plugins"), &settings, {}};
        // The way archives are made: one folder, with the plugin inside it.
        QVERIFY(Fixtures::zip(dir.filePath("shelfmark.zip"),
                              {{"Shelfmark-2/plugin.json", "{\"name\": \"Shelfmark\", \"type\": \"metadata\", \"fields\": [\"title\"], "
                                                           "\"command\": [\"./lookup\"]}"},
                               {"Shelfmark-2/lookup", "#!/bin/sh\necho '{}'\n"},
                               {"Shelfmark-2/docs/README.md", "# Shelfmark\n"}}));
        const QJsonObject installed = PluginTools::install(context, dir.filePath("shelfmark.zip"));
        QVERIFY2(!ScriptTools::failed(installed), qPrintable(installed.value("error").toString()));
        QCOMPARE(installed.value("name").toString(), QString("shelfmark-2"));
        QCOMPARE(installed.value("kind").toString(), QString("program"));
        // A program is not held to permissions, and the answer says so.
        QCOMPARE(installed.value("permissions").toVariant().toStringList(), QStringList{"program"});
        QCOMPARE(installed.value("files").toInt(), 3);
        QVERIFY(QFile::exists(dir.filePath("plugins/shelfmark-2/docs/README.md")));
        QCOMPARE(leftIn(dir.filePath("plugins")), QStringList{"shelfmark-2"});

        // Flat, and under a name of the installer's choosing.
        QVERIFY(Fixtures::zip(dir.filePath("flat.zip"), {{"plugin.json", scriptManifest}, {"main.js", script}}));
        PluginTools::InstallOptions named;
        named.name = "log";
        QCOMPARE(PluginTools::install(context, dir.filePath("flat.zip"), named).value("folder").toString(),
                 dir.filePath("plugins/log"));
        named.name = "Not A Name";
        QVERIFY(PluginTools::install(context, dir.filePath("flat.zip"), named).value("error").toString().contains("name"));
    }

    void aPackageThatIsNoPluginIsRefusedWhole() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const PluginTools::Context context{dir.filePath("work/plugins"), &settings, {}};
        const QByteArray manifest = scriptManifest;
        PluginTools::InstallOptions small;
        small.limits.files = 3;
        small.limits.fileBytes = 4096;
        small.limits.totalBytes = 6000;

        struct Case {
            const char *what;
            Fixtures::Entries entries;
            const char *says;
        };
        const QList<Case> cases{
            {"a step up and out", {{"plugin.json", manifest}, {"main.js", script}, {"../evil.js", "x"}}, "outside"},
            {"a step up in the middle", {{"plugin.json", manifest}, {"main.js", script}, {"a/../../../evil.js", "x"}}, "outside"},
            {"an absolute path", {{"plugin.json", manifest}, {"main.js", script}, {"/tmp/omabook-evil.js", "x"}}, "outside"},
            {"a backslash", {{"plugin.json", manifest}, {"main.js", script}, {"..\\evil.js", "x"}}, "outside"},
            {"no plugin.json", {{"main.js", script}}, "no plugin.json"},
            {"two folders and no plugin.json", {{"a/plugin.json", manifest}, {"b/main.js", script}}, "no plugin.json"},
            {"a plugin.json that isn't JSON", {{"plugin.json", "{nope"}, {"main.js", script}}, "can't be read"},
            {"nothing to run", {{"plugin.json", "{\"name\": \"Empty\"}"}}, "neither"},
            {"a script that isn't there", {{"plugin.json", manifest}}, "isn't in it"},
            {"a script outside it", {{"plugin.json", "{\"script\": \"../main.js\"}"}, {"main.js", script}}, "isn't in it"},
            {"a permission nobody knows",
             {{"plugin.json", "{\"script\": \"main.js\", \"permissions\": [\"root\"]}"}, {"main.js", script}}, "permission"},
            {"a kind nobody knows", {{"plugin.json", "{\"type\": \"device\", \"command\": [\"./run\"]}"}, {"run", "x"}}, "type"},
            {"a file too large", {{"plugin.json", manifest}, {"main.js", QByteArray(5000, ' ')}}, "larger"},
            {"too much altogether",
             {{"plugin.json", manifest}, {"main.js", QByteArray(3000, ' ')}, {"more.js", QByteArray(3000, ' ')}}, "more than"},
            {"too many files", {{"plugin.json", manifest}, {"main.js", script}, {"a.js", "1"}, {"b.js", "2"}}, "files"},
        };
        for (const Case &test : cases) {
            const QString zip = dir.filePath("package.zip");
            QFile::remove(zip);
            QVERIFY(Fixtures::zip(zip, test.entries));
            for (const bool looking : {true, false}) {
                const QJsonObject answer = looking ? PluginTools::inspect(context, zip, small) : PluginTools::install(context, zip, small);
                QVERIFY2(ScriptTools::failed(answer), test.what);
                QVERIFY2(answer.value("error").toString().contains(test.says),
                         qPrintable(QString("%1: %2").arg(test.what, answer.value("error").toString())));
            }
            // Nothing of it is left where a plugin would be found, nor anywhere above.
            QVERIFY2(leftIn(dir.filePath("work/plugins")).isEmpty(), test.what);
            QVERIFY2(!QFile::exists(dir.filePath("work/evil.js")) && !QFile::exists(dir.filePath("evil.js")), test.what);
            QVERIFY2(!QFile::exists("/tmp/omabook-evil.js"), test.what);
            QVERIFY2(!settings.pluginEnabled("folder:package", false), test.what);
        }

        // A folder is held to the same: a link is not followed out of it.
        QVERIFY(scriptFolder(dir.filePath("linked")));
        QVERIFY(Fixtures::write(dir.filePath("secret.txt"), "not yours"));
        QVERIFY(QFile::link(dir.filePath("secret.txt"), dir.filePath("linked/notes.txt")));
        QVERIFY(PluginTools::install(context, dir.filePath("linked")).value("error").toString().contains("link"));
        QVERIFY(leftIn(dir.filePath("work/plugins")).isEmpty());
        // And what isn't there, or is no archive, says so.
        QVERIFY(ScriptTools::failed(PluginTools::install(context, dir.filePath("nowhere.zip"))));
        QVERIFY(Fixtures::write(dir.filePath("words.zip"), "these are only words"));
        QVERIFY(ScriptTools::failed(PluginTools::install(context, dir.filePath("words.zip"))));
        QVERIFY(leftIn(dir.filePath("work/plugins")).isEmpty());
    }

    void aPluginOfYourOwnIsRemoved() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        const PluginTools::Context context{dir.filePath("plugins"), &settings, {}};
        QVERIFY(!ScriptTools::failed(ScriptTools::create(context, "reading-log", "hook")));
        QVERIFY(Fixtures::write(dir.filePath("plugins/reading-log.error"), "main.js:2: It went wrong.\n"));

        // Only what is in the plugin folder, by a name that stays in it.
        for (const char *name : {"nothing", "../plugins/reading-log", "builtin:Crossref", ""})
            QVERIFY2(ScriptTools::failed(PluginTools::remove(context, name)), name);
        QVERIFY(QFile::exists(dir.filePath("plugins/reading-log/main.js")));

        const QJsonObject removed = PluginTools::remove(context, "reading-log");
        QVERIFY2(!ScriptTools::failed(removed), qPrintable(removed.value("error").toString()));
        QCOMPARE(removed.value("id").toString(), QString("folder:reading-log"));
        QVERIFY(leftIn(dir.filePath("plugins")).isEmpty());
        QVERIFY(ScriptTools::failed(PluginTools::remove(context, "reading-log")));
    }

    void aNameIsMadeOfWhateverWasTyped() {
        QCOMPARE(PluginTools::folderName("Reading Log 2"), QString("reading-log-2"));
        QCOMPARE(PluginTools::folderName("  Café des Livres! "), QString("cafe-des-livres"));
        QCOMPARE(PluginTools::folderName("shelf_mark"), QString("shelf_mark"));
        QCOMPARE(PluginTools::folderName("../../etc"), QString("etc"));
        QCOMPARE(PluginTools::folderName("!!!"), QString());
    }

    void theOrderOfTheSourcesIsKept() {
        QTemporaryDir dir;
        QStringList shipped;
        {
            Settings settings(dir.filePath("config.json"));
            Plugins plugins(&settings, dir.filePath("plugins"), "-");
            shipped = plugins.order();
            QVERIFY(shipped.size() >= 5);
            QCOMPARE(shipped.first(), QString("builtin:Open Library"));
            // Crossref goes first, one step at a time.
            const int steps = int(shipped.indexOf("builtin:Crossref"));
            for (int step = 0; step < steps; ++step)
                QVERIFY(plugins.move("builtin:Crossref", -1));
            // Neither off the end, nor something that is no source.
            QVERIFY(!plugins.move("builtin:Crossref", -1));
            QVERIFY(!plugins.move(shipped.last() == "builtin:Crossref" ? shipped.first() : plugins.order().last(), 1));
            QVERIFY(!plugins.move("format:EPUB", 1));
            QVERIFY(!plugins.setOrder({"format:EPUB"}));
            QCOMPARE(plugins.order().first(), QString("builtin:Crossref"));
        }
        // Another run finds the order as it was left, and asks in it.
        Settings settings(dir.filePath("config.json"));
        Plugins plugins(&settings, dir.filePath("plugins"), "-");
        QStringList wanted = shipped;
        wanted.move(wanted.indexOf("builtin:Crossref"), 0);
        QCOMPARE(plugins.order(), wanted);
        QNetworkAccessManager network;
        QObject holder;
        const QList<MetadataSource *> sources = plugins.sources(plugins.known(), &network, &holder);
        QCOMPARE(sources.first()->name(), QString("Crossref"));
        QCOMPARE(sources.at(1)->name(), QString("Open Library"));

        // A source that comes later (one of the user's own) takes its place
        // after the ones that were put in order, and can be moved like them.
        QVERIFY(QDir(dir.path()).mkpath("plugins/mine"));
        QVERIFY(Fixtures::write(dir.filePath("plugins/mine/plugin.json"),
                                "{\"name\": \"Mine\", \"fields\": [\"tags\"], \"command\": [\"./run\"]}"));
        QCOMPARE(plugins.order().last(), QString("folder:mine"));
        QVERIFY(plugins.setOrder({"folder:mine", "builtin:Crossref"}));
        QCOMPARE(plugins.order().mid(0, 3), (QStringList{"folder:mine", "builtin:Crossref", "builtin:Open Library"}));
        // The rest of the list is as it was: only sources are ordered.
        const QJsonArray overview = PluginTools::overview(plugins.known());
        QCOMPARE(byId(overview, "folder:mine").value("position").toInt(), 1);
        QCOMPARE(byId(overview, "folder:mine").value("of").toInt(), int(plugins.order().size()));
        QVERIFY(!byId(overview, "format:EPUB").contains("position"));
    }

    void whatNeedsAttentionIsSaid() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        // Calibre's settings are the test's own, naming one library.
        const QString library = dir.filePath("Calibre Library");
        QVERIFY(Fixtures::calibreLibrary(library));
        QVERIFY(Fixtures::calibreConfig(dir.filePath("calibre"), {library}));
        qputenv("CALIBRE_CONFIG_DIRECTORY", dir.filePath("calibre").toUtf8());
        const auto restore = qScopeGuard([this] { qputenv("CALIBRE_CONFIG_DIRECTORY", home.filePath("no-calibre").toUtf8()); });
        Plugins plugins(&settings, dir.filePath("plugins"), "-");

        // With nothing of the user's, nothing needs attention.
        QJsonArray overview = PluginTools::overview(plugins.known());
        QVERIFY(PluginTools::health(overview, plugins.calibre()).isEmpty());
        // Everything is sorted by what it does, in the order of the groups.
        QStringList groups;
        for (const QJsonValue &value : std::as_const(overview))
            if (groups.isEmpty() || groups.last() != value.toObject().value("group").toString())
                groups << value.toObject().value("group").toString();
        QCOMPARE(groups, (QStringList{"metadata", "reading", "calibre", "format", "catalog", "ai"}));
        QCOMPARE(byId(overview, "builtin:Crossref").value("state").toString(), QString("on"));
        QCOMPARE(byId(overview, "calibre").value("state").toString(), QString("on"));
        QVERIFY(!byId(overview, "builtin:Crossref").value("removable").toBool());

        // A script that went wrong, a program that isn't there, one whose
        // plugin.json is no use, one that is fine, and Calibre switched off
        // with its library lying there.
        const PluginTools::Context context{dir.filePath("plugins"), &settings, {}};
        QVERIFY(!ScriptTools::failed(ScriptTools::create(context, "reading-log", "hook")));
        QVERIFY(Fixtures::write(dir.filePath("plugins/reading-log.error"),
                                "main.js:3: ReferenceError: shelf is not defined\nOmabook is using the last version that worked.\n"));
        QVERIFY(!ScriptTools::failed(ScriptTools::create(context, "fine", "command")));
        QVERIFY(QDir(dir.path()).mkpath("plugins/shelfmark"));
        QVERIFY(Fixtures::write(dir.filePath("plugins/shelfmark/plugin.json"),
                                "{\"name\": \"Shelfmark\", \"fields\": [\"title\"], \"command\": [\"omabook-no-such-program\"]}"));
        QVERIFY(QDir(dir.path()).mkpath("plugins/odd"));
        QVERIFY(Fixtures::write(dir.filePath("plugins/odd/plugin.json"), "{\"name\": \"Odd\", \"type\": \"device\"}"));
        settings.setPluginEnabled("calibre", false);

        overview = PluginTools::overview(plugins.known());
        QCOMPARE(byId(overview, "folder:reading-log").value("state").toString(), QString("broken"));
        QCOMPARE(byId(overview, "folder:reading-log").value("group").toString(), QString("script"));
        const QJsonObject error = byId(overview, "folder:reading-log").value("error").toObject();
        QCOMPARE(error.value("file").toString(), QString("main.js"));
        QCOMPARE(error.value("line").toInt(), 3);
        QCOMPARE(error.value("message").toString(), QString("ReferenceError: shelf is not defined"));
        QVERIFY(error.value("kept").toBool());
        QCOMPARE(byId(overview, "folder:fine").value("state").toString(), QString("on"));
        QVERIFY(byId(overview, "folder:fine").value("removable").toBool());
        QCOMPARE(byId(overview, "folder:shelfmark").value("state").toString(), QString("needs"));
        QCOMPARE(byId(overview, "folder:shelfmark").value("group").toString(), QString("metadata"));
        QVERIFY(byId(overview, "folder:shelfmark").value("note").toString().contains("omabook-no-such-program"));
        QCOMPARE(byId(overview, "folder:odd").value("state").toString(), QString("broken"));
        QCOMPARE(byId(overview, "folder:odd").value("group").toString(), QString("program"));
        QCOMPARE(byId(overview, "calibre").value("state").toString(), QString("off"));

        QStringList issues;
        for (const QJsonValue &value : PluginTools::health(overview, plugins.calibre()))
            issues << value.toObject().value("id").toString() + " " + value.toObject().value("issue").toString();
        issues.sort();
        QCOMPARE(issues, (QStringList{"calibre calibre-off", "folder:odd broken", "folder:reading-log broken",
                                      "folder:shelfmark missing"}));

        // Put right, each is gone from the line: the error dismissed, the
        // program switched off, the odd one removed, Calibre switched on.
        QVERIFY(PluginTools::dismissError(dir.filePath("plugins"), "reading-log"));
        QVERIFY(PluginTools::lastError(dir.filePath("plugins"), "reading-log").isEmpty());
        settings.setPluginEnabled("folder:shelfmark", false);
        QVERIFY(!ScriptTools::failed(PluginTools::remove(context, "odd")));
        settings.setPluginEnabled("calibre", true);
        overview = PluginTools::overview(plugins.known());
        QVERIFY(PluginTools::health(overview, plugins.calibre()).isEmpty());
        QCOMPARE(byId(overview, "folder:shelfmark").value("state").toString(), QString("off"));
    }

    void theManagerDoesItForTheWindow() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        Plugins registry(&settings, dir.filePath("plugins"), "-");
        ScriptServices services;
        services.settings = &settings;
        ScriptPlugins scripts(services, dir.filePath("plugins"));
        FakePicker picker;
        QNetworkAccessManager network;
        PluginManager manager(&settings, &registry, &scripts, &picker, &network);
        QList<QUrl> opened;
        manager.setOpener([&](const QUrl &url) {
            opened << url;
            return true;
        });
        QSignalSpy touched(&manager, &PluginManager::touched);
        for (const char *name : {"catalog", "command", "hook", "metadata", "panel"})
            QVERIFY2(manager.templates().contains(name), name);
        QVERIFY(manager.health().isEmpty());

        // A new one from a template is there at once, running, and its
        // folder is shown.
        QCOMPARE(manager.folderName("Mark It Read"), QString("mark-it-read"));
        const QVariantMap made = manager.create("mark-it-read", "command");
        QVERIFY2(!made.contains("error"), qPrintable(made.value("error").toString()));
        QCOMPARE(opened, QList<QUrl>{QUrl::fromLocalFile(dir.filePath("plugins/mark-it-read"))});
        QVariantMap mine;
        for (const QVariant &plugin : manager.plugins())
            if (plugin.toMap().value("id").toString() == "folder:mark-it-read")
                mine = plugin.toMap();
        QCOMPARE(mine.value("state").toString(), QString("on"));
        QVERIFY(mine.value("running").toBool());
        // What it registered, in words for the detail view.
        const QVariantMap added = mine.value("adds").toList().value(0).toMap();
        QCOMPARE(added.value("what").toString(), QString("command"));
        QVERIFY(!added.value("name").toString().isEmpty());
        QVERIFY(manager.create("mark-it-read", "command").contains("error"));
        QVERIFY(manager.create("x", "nonsense").contains("error"));

        // One picked to install is on offer with its permissions until it is
        // approved, and goes in switched on or off as was answered.
        QVERIFY(Fixtures::zip(dir.filePath("log.zip"), {{"reading-log/plugin.json", scriptManifest}, {"reading-log/main.js", script}}));
        picker.next = {QUrl::fromLocalFile(dir.filePath("log.zip"))};
        manager.pick(false);
        QCOMPARE(picker.asked, QList<FilePicker::Purpose>{FilePicker::InstallPlugin});
        QCOMPARE(manager.offer().value("title").toString(), QString("Reading Log"));
        QCOMPARE(manager.offer().value("permissions").toStringList(), (QStringList{"http", "files"}));
        QVERIFY(!QFile::exists(dir.filePath("plugins/reading-log")));
        const QVariantMap installed = manager.install(false);
        QCOMPARE(installed.value("id").toString(), QString("folder:reading-log"));
        QVERIFY(manager.offer().isEmpty());
        QVERIFY(!settings.pluginEnabled("folder:reading-log", true));
        QVERIFY(!scripts.plugin("reading-log"));
        // Something that is no plugin stays on offer as what is wrong with it.
        QVERIFY(Fixtures::zip(dir.filePath("bad.zip"), {{"readme.txt", "hello"}}));
        manager.consider(dir.filePath("bad.zip"));
        QVERIFY(manager.offer().value("error").toString().contains("plugin.json"));
        QVERIFY(manager.install(true).contains("error"));
        manager.dismissOffer();
        QVERIFY(manager.offer().isEmpty());

        // A program of the user's own that answers look-ups is tried with
        // one title, and what it brought is listed.
        QVERIFY(QDir(dir.path()).mkpath("plugins/shelfmark"));
        QVERIFY(Fixtures::write(dir.filePath("plugins/shelfmark/lookup"),
                                "#!/bin/sh\ncat > /dev/null\n"
                                "echo '{\"candidates\": [{\"title\": \"Pride and Prejudice\", \"authors\": [\"Jane Austen\"], "
                                "\"published\": \"1813\", \"series\": \"Made Up\"}]}'\n"));
        QVERIFY(QFile::setPermissions(dir.filePath("plugins/shelfmark/lookup"), QFile::Permissions(0x7755)));
        QVERIFY(Fixtures::write(dir.filePath("plugins/shelfmark/plugin.json"),
                                QByteArray("{\"name\": \"Shelfmark\", \"fields\": [\"title\", \"series\"], \"command\": [\"")
                                    + dir.filePath("plugins/shelfmark/lookup").toUtf8() + "\"]}"));
        manager.refresh();
        manager.tryOut("folder:shelfmark", manager.sampleTitle());
        QVERIFY(manager.trial().value("asking").toBool());
        QTRY_VERIFY(manager.trial().value("answered").toBool());
        QCOMPARE(manager.trial().value("problem").toString(), QString());
        const QVariantMap result = manager.trial().value("results").toList().value(0).toMap();
        QCOMPARE(result.value("title").toString(), QString("Pride and Prejudice"));
        QCOMPARE(result.value("authors").toString(), QString("Jane Austen"));
        QVERIFY(result.value("brings").toStringList().contains("series"));
        // Switched off, there is nothing to ask, and it says so.
        settings.setPluginEnabled("folder:shelfmark", false);
        manager.tryOut("folder:shelfmark", "Anything");
        QVERIFY(manager.trial().value("answered").toBool());
        QVERIFY(!manager.trial().value("problem").toString().isEmpty());
        manager.stopTrial();
        QVERIFY(manager.trial().isEmpty());

        // The order is the registry's, moved from here.
        const QString last = registry.order().last();
        QVERIFY(manager.move(last, -1));
        QCOMPARE(registry.order().last() == last, false);

        // Removing is for what is in the plugin folder only.
        QVERIFY(manager.remove("builtin:Crossref").contains("error"));
        QVERIFY(!manager.remove("folder:mark-it-read").contains("error"));
        QVERIFY(!QFile::exists(dir.filePath("plugins/mark-it-read")));
        QVERIFY(!scripts.plugin("mark-it-read"));
        QVERIFY(touched.count() >= 4);
    }

    void anAgentInstallsAndRemoves() {
        // The command line and the MCP tools work on the user's own plugin
        // folder, which here is the test's home.
        QTemporaryDir dir;
        QVERIFY(scriptFolder(dir.filePath("Reading Log")));
        const QString plugins = home.filePath("config/omabook/plugins");
        QCOMPARE(runPluginCli({"omabook", "plugin", "install", dir.filePath("Reading Log"), "--json"}), 0);
        QVERIFY(QFile::exists(plugins + "/reading-log/main.js"));
        QVERIFY(!Settings().pluginEnabled("folder:reading-log", true));
        // Again, it is in the way; replacing and switching on are asked for.
        QCOMPARE(runPluginCli({"omabook", "plugin", "install", dir.filePath("Reading Log")}), 1);
        QCOMPARE(runPluginCli({"omabook", "plugin", "install", dir.filePath("Reading Log"), "--replace", "--enable"}), 0);
        QVERIFY(Settings().pluginEnabled("folder:reading-log", false));
        QCOMPARE(runPluginCli({"omabook", "plugin", "install"}), 1);
        QCOMPARE(runPluginCli({"omabook", "plugin", "remove", "reading-log"}), 0);
        QVERIFY(!QFile::exists(plugins + "/reading-log"));
        QCOMPARE(runPluginCli({"omabook", "plugin", "remove", "reading-log"}), 1);

        // The same as tools.
        QVERIFY(Fixtures::zip(dir.filePath("log.zip"), {{"plugin.json", scriptManifest}, {"main.js", script}}));
        Mcp::Result result = callTool("plugin_install", {{"source", dir.filePath("log.zip")}, {"name", "log"}});
        QVERIFY2(!result.failed(), qPrintable(result.error));
        QCOMPARE(result.data.value("enabled").toBool(), false);
        QCOMPARE(result.data.value("permissions").toVariant().toStringList(), (QStringList{"http", "files"}));
        QVERIFY(QFile::exists(plugins + "/log/plugin.json"));
        QVERIFY(Fixtures::zip(dir.filePath("evil.zip"), {{"plugin.json", scriptManifest}, {"main.js", script}, {"../../evil.js", "x"}}));
        QVERIFY(callTool("plugin_install", {{"source", dir.filePath("evil.zip")}}).error.contains("outside"));
        QVERIFY(!callTool("plugin_remove", {{"name", "log"}}).failed());
        QVERIFY(callTool("plugin_remove", {{"name", "log"}}).failed());
        QVERIFY(leftIn(plugins).isEmpty());
        // Both are in the table, and removing is marked as taking something away.
        for (const Mcp::Tool &tool : Mcp::tools())
            if (tool.name == "plugin_remove")
                QCOMPARE(tool.effect, Mcp::Destructive);
    }
};

QTEST_MAIN(PluginManagerTests)
#include "pluginmanager_tests.moc"
