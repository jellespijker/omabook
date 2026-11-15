#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>

#include "calibreplugin.h"
#include "fakegithub.h"
#include "fixtures.h"
#include "mcp.h"
#include "metadatasource.h"
#include "pluginfinder.h"
#include "pluginmanager.h"
#include "plugins.h"
#include "pluginsources.h"
#include "plugintools.h"
#include "scriptapi.h"
#include "scriptplugins.h"
#include "scripttools.h"
#include "secretstore.h"
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

QByteArray slurp(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

Mcp::Result callTool(const QString &name, const QJsonObject &arguments) {
    for (const Mcp::Tool &tool : Mcp::tools())
        if (tool.name == name)
            return tool.handler(arguments);
    return Mcp::Result::fail("There is no tool " + name);
}


// A plugin folder, settings and a GitHub of the test's own, and the sources
// that talk to it. The clock is the test's too.
struct Hub {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    FakeGitHub github;
    qint64 time = 1800000000;
    PluginTools::Context context{dir.filePath("plugins"), &settings, {}};
    std::unique_ptr<PluginSources> sources;
    const QString repository = "someone/omabook-storyteller";

    Hub() { again(); }
    // As another run of Omabook would find things.
    void again() {
        sources = std::make_unique<PluginSources>(context);
        PluginSources::Service service;
        service.api = github.api();
        service.hosts = QStringList{"127.0.0.1"};
        service.httpsOnly = false;
        sources->setService(service);
        sources->setClock([this] { return time; });
    }
    QJsonObject add(const QString &address, const PluginSources::AddOptions &options = {}) {
        return PluginSources::wait([&](PluginSources::Done done) { sources->add(address, options, done); }, 10000);
    }
    QJsonObject look(const QString &address, const QString &pin = {}) {
        return PluginSources::wait([&](PluginSources::Done done) { sources->look(address, pin, {}, done); }, 10000);
    }
    QJsonObject check(const QString &name = "storyteller") {
        return PluginSources::wait([&](PluginSources::Done done) { sources->check(name, done); }, 10000);
    }
    QJsonObject update(bool approved = false, const QString &name = "storyteller") {
        return PluginSources::wait([&](PluginSources::Done done) { sources->update(name, approved, done); }, 10000);
    }
    QJsonObject checkAll(bool apply, bool dueOnly) {
        return PluginSources::wait([&](PluginSources::Done done) { sources->checkAll(apply, dueOnly, done); }, 10000);
    }
    QString installedVersion(const QString &name = "storyteller") const {
        QFile file(dir.filePath("plugins/" + name + "/plugin.json"));
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object().value("version").toString() : QString();
    }
    // The storyteller on its default branch, installed and switched on.
    bool installed(const Made &made = {}) {
        github.push(repository, "main", sha('a'), made.packed());
        PluginSources::AddOptions options;
        options.enable = true;
        return !ScriptTools::failed(add(repository, options));
    }
};

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

    // --- Plugins from GitHub repositories ---

    void everyFormOfAddressIsRead() {
        struct Case {
            const char *text, *repository, *ref, *subfolder;
        };
        const QList<Case> cases{
            {"jellespijker/omabook-storyteller", "jellespijker/omabook-storyteller", "", ""},
            {"https://github.com/jellespijker/omabook-storyteller", "jellespijker/omabook-storyteller", "", ""},
            {" https://github.com/jellespijker/omabook-storyteller.git ", "jellespijker/omabook-storyteller", "", ""},
            {"http://www.github.com/Someone/Repo/", "Someone/Repo", "", ""},
            {"github.com/someone/repo", "someone/repo", "", ""},
            {"git@github.com:someone/repo.git", "someone/repo", "", ""},
            {"https://github.com/someone/repo/releases", "someone/repo", "", ""},
            {"https://github.com/someone/repo/releases/latest", "someone/repo", "", ""},
            {"https://github.com/someone/repo/releases/tag/v1.2", "someone/repo", "v1.2", ""},
            {"https://github.com/someone/repo/tree/v1.2", "someone/repo", "v1.2", ""},
            {"https://github.com/someone/repo/tree/main/plugins/reading-log", "someone/repo", "main", "plugins/reading-log"},
            {"https://github.com/someone/repo/tree/main/plugins/Reading%20Log?tab=readme#top", "someone/repo", "main", "plugins/Reading Log"},
            {"https://github.com/someone/repo/blob/dev/plugins/log/plugin.json", "someone/repo", "dev", "plugins/log"},
            {"https://github.com/someone/repo/commit/0123abc", "someone/repo", "0123abc", ""},
            {"https://github.com/someone/repo/archive/refs/tags/v2.0.tar.gz", "someone/repo", "v2.0", ""},
        };
        for (const Case &given : cases) {
            PluginSources::Address address;
            QString error;
            QVERIFY2(PluginSources::parse(given.text, &address, &error), qPrintable(QString(given.text) + ": " + error));
            QCOMPARE(address.repository(), QString(given.repository));
            QCOMPARE(address.ref, QString(given.ref));
            QCOMPARE(address.subfolder, QString(given.subfolder));
        }
        // What isn't a repository on GitHub says so, and names nothing.
        for (const char *text : {"", "storyteller", "https://gitlab.com/someone/repo", "https://example.com/a/b",
                                 "https://github.com/someone", "someone/repo/issues/4", "https://github.com/someone/repo/tree/main/../../etc",
                                 "../etc/passwd", "someone/.."}) {
            PluginSources::Address address;
            QString error;
            QVERIFY2(!PluginSources::parse(text, &address, &error), text);
            QVERIFY2(!error.isEmpty(), text);
        }
        // The folder it goes in, when nobody says.
        PluginSources::Address address;
        QString error;
        QVERIFY(PluginSources::parse("jellespijker/omabook-storyteller", &address, &error));
        QCOMPARE(PluginSources::nameFor(address), QString("storyteller"));
        QVERIFY(PluginSources::parse("someone/Reading.Log", &address, &error));
        QCOMPARE(PluginSources::nameFor(address), QString("reading-log"));
        QVERIFY(PluginSources::parse("someone/tools/tree/main/plugins/Shelf Notes", &address, &error));
        QCOMPARE(PluginSources::nameFor(address), QString("shelf-notes"));
    }

    void aPluginIsAddedFromItsRepository() {
        Hub hub;
        hub.github.push(hub.repository, "main", sha('a'), Made().packed());

        // Looked at first: fetched and described, with nothing installed.
        const QJsonObject offered = hub.look("https://github.com/someone/omabook-storyteller");
        QVERIFY2(!ScriptTools::failed(offered), qPrintable(offered.value("error").toString()));
        QCOMPARE(offered.value("name").toString(), QString("storyteller"));
        QCOMPARE(offered.value("title").toString(), QString("Storyteller"));
        QCOMPARE(offered.value("version").toString(), QString("1.0"));
        QCOMPARE(offered.value("permissions").toVariant().toStringList(), QStringList{"files"});
        // What it looks to add, from reading its script; it was not run.
        QCOMPARE(offered.value("adds").toVariant().toStringList(), (QStringList{"command", "hook"}));
        QCOMPARE(offered.value("repository").toString(), hub.repository);
        // No release, so it follows the default branch, at the commit that is its head.
        QCOMPARE(offered.value("ref").toString(), QString("main"));
        QCOMPARE(offered.value("commit").toString(), sha('a'));
        QVERIFY(leftIn(hub.dir.filePath("plugins")).isEmpty());
        // Asked as Omabook, of the stand-in and nobody else, with no token.
        for (const QByteArray &head : std::as_const(hub.github.heads)) {
            QVERIFY(FakeGitHub::header(head, "user-agent").startsWith("Omabook/"));
            QVERIFY(FakeGitHub::header(head, "authorization").isEmpty());
        }

        // Installed switched off, and where it came from is remembered.
        const QJsonObject installed = hub.sources->installLooked(false);
        QVERIFY2(!ScriptTools::failed(installed), qPrintable(installed.value("error").toString()));
        QVERIFY(!hub.settings.pluginEnabled("folder:storyteller", true));
        QVERIFY(QFile::exists(hub.dir.filePath("plugins/storyteller/lib/words.js")));
        QVERIFY(QFile::exists(hub.dir.filePath("plugins/sources.json")));
        // The registry is a file, so the plugins beside it are all there is to load.
        QCOMPARE(QDir(hub.dir.filePath("plugins")).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{"storyteller"});

        // Another run reads the same.
        hub.again();
        const QJsonArray list = hub.sources->list();
        QCOMPARE(list.size(), 1);
        const QJsonObject entry = list.first().toObject();
        QCOMPARE(entry.value("name").toString(), QString("storyteller"));
        QCOMPARE(entry.value("repository").toString(), hub.repository);
        QCOMPARE(entry.value("channel").toObject().value("kind").toString(), QString("auto"));
        QCOMPARE(entry.value("installed").toObject().value("version").toString(), QString("1.0"));
        QCOMPARE(entry.value("installed").toObject().value("commit").toString(), sha('a'));
        QCOMPARE(entry.value("installed").toObject().value("kind").toString(), QString("branch"));
        QCOMPARE(qint64(entry.value("checked_at").toDouble()), hub.time);
        QVERIFY(!entry.value("update").toBool() && !entry.value("missing").toBool());
        // It shows on the plugin in the manager's overview.
        Plugins plugins(&hub.settings, hub.dir.filePath("plugins"), "-");
        const QJsonObject row = byId(hub.sources->annotate(PluginTools::overview(plugins.known())), "folder:storyteller");
        QCOMPARE(row.value("source").toObject().value("repository").toString(), hub.repository);

        // One of the same name is in the way unless it is to be replaced.
        QVERIFY(hub.add(hub.repository).value("error").toString().contains("already"));
        // Forgotten, the plugin stays and is no longer followed; removed, it is forgotten too.
        QVERIFY(!ScriptTools::failed(hub.sources->forget("storyteller")));
        QVERIFY(hub.sources->list().isEmpty());
        QVERIFY(QFile::exists(hub.dir.filePath("plugins/storyteller/main.js")));
        PluginSources::AddOptions replace;
        replace.replace = true;
        QVERIFY(!ScriptTools::failed(hub.add(hub.repository, replace)));
        QVERIFY(!ScriptTools::failed(PluginTools::remove(hub.context, "storyteller")));
        QVERIFY(hub.sources->list().isEmpty());
    }

    void releasesBranchesTagsAndSubfoldersAreFollowed() {
        Hub hub;
        Made first, second, inFolder;
        second.version = "2.0";
        inFolder.subfolder = "plugins/shelf-notes";
        inFolder.title = "Shelf Notes";
        hub.github.push(hub.repository, "main", sha('a'), second.packed());
        hub.github.push(hub.repository, "dev", sha('d'), second.packed());
        hub.github.tag(hub.repository, "v1.0", sha('b'), first.packed(), true);

        // A repository with releases is followed by its latest release, not its branch.
        QJsonObject added = hub.add(hub.repository);
        QVERIFY2(!ScriptTools::failed(added), qPrintable(added.value("error").toString()));
        QCOMPARE(hub.installedVersion(), QString("1.0"));
        QCOMPARE(hub.sources->source("storyteller").value("installed").toObject().value("kind").toString(), QString("release"));
        QCOMPARE(hub.sources->source("storyteller").value("installed").toObject().value("ref").toString(), QString("v1.0"));
        // A newer release is an update.
        hub.github.tag(hub.repository, "v2.0", sha('c'), second.packed(), true);
        QVERIFY(hub.check().value("update").toBool());
        QVERIFY(hub.update().value("updated").toBool());
        QCOMPARE(hub.installedVersion(), QString("2.0"));

        // A link to a tag stays at that tag, and is never asked about again.
        PluginSources::AddOptions asTag;
        asTag.name = "pinned";
        added = hub.add("https://github.com/someone/omabook-storyteller/releases/tag/v1.0", asTag);
        QVERIFY2(!ScriptTools::failed(added), qPrintable(added.value("error").toString()));
        QCOMPARE(hub.installedVersion("pinned"), QString("1.0"));
        QCOMPARE(hub.sources->source("pinned").value("channel").toObject().value("kind").toString(), QString("tag"));
        const int before = int(hub.github.asked.size());
        QVERIFY(hub.check("pinned").value("pinned").toBool());
        QCOMPARE(int(hub.github.asked.size()), before);

        // --pin with a branch follows that branch.
        PluginSources::AddOptions asBranch;
        asBranch.name = "edge";
        asBranch.pin = "dev";
        added = hub.add(hub.repository, asBranch);
        QVERIFY2(!ScriptTools::failed(added), qPrintable(added.value("error").toString()));
        QCOMPARE(hub.sources->source("edge").value("channel").toObject().value("kind").toString(), QString("branch"));
        QCOMPARE(hub.sources->source("edge").value("installed").toObject().value("commit").toString(), sha('d'));
        // And a ref that is neither says so.
        asBranch.name = "nowhere";
        asBranch.pin = "v9";
        QVERIFY(hub.add(hub.repository, asBranch).value("error").toString().contains("v9"));
        QVERIFY(!QFile::exists(hub.dir.filePath("plugins/nowhere")));

        // Pinned afterwards, a source goes to that tag now; following again
        // brings it back to the latest release.
        QJsonObject pinned = PluginSources::wait([&](PluginSources::Done done) { hub.sources->pin("storyteller", "v1.0", done); }, 10000);
        QVERIFY2(pinned.value("updated").toBool(), qPrintable(pinned.value("error").toString()));
        QCOMPARE(hub.installedVersion(), QString("1.0"));
        QVERIFY(hub.checkAll(true, false).value("results").toArray().size() == 1); // only "edge" is asked: the others stay put
        QVERIFY(!ScriptTools::failed(hub.sources->follow("storyteller")));
        QVERIFY(hub.update().value("updated").toBool());
        QCOMPARE(hub.installedVersion(), QString("2.0"));

        // A plugin kept in a folder of a larger repository: only that folder is installed.
        hub.github.push("someone/tools", "main", sha('e'), inFolder.packed("someone-tools-eeeeeee"));
        added = hub.add("https://github.com/someone/tools/tree/main/plugins/shelf-notes");
        QVERIFY2(!ScriptTools::failed(added), qPrintable(added.value("error").toString()));
        QCOMPARE(added.value("name").toString(), QString("shelf-notes"));
        QCOMPARE(leftIn(hub.dir.filePath("plugins/shelf-notes")), (QStringList{"lib", "main.js", "plugin.json"}));
        QCOMPARE(hub.sources->source("shelf-notes").value("subfolder").toString(), QString("plugins/shelf-notes"));
        // A folder that holds no plugin is refused.
        QVERIFY(hub.look("https://github.com/someone/tools/tree/main/docs").value("error").toString().contains("plugin.json"));
    }

    void anUnchangedSourceCostsNoDownload() {
        Hub hub;
        QVERIFY(hub.installed());
        QCOMPARE(hub.github.downloads, 1);
        hub.github.asked.clear();
        hub.github.heads.clear();

        // Asked again: what was answered last time is named, and GitHub says
        // nothing changed. GitHub doesn't count such an answer against the hour.
        const QJsonObject checked = hub.check();
        QVERIFY2(!ScriptTools::failed(checked), qPrintable(checked.value("error").toString()));
        QVERIFY(!checked.value("update").toBool());
        QVERIFY(checked.value("unchanged").toBool());
        QCOMPARE(hub.github.downloads, 1);
        QCOMPARE(hub.github.count("/commits/main"), 1);
        QCOMPARE(FakeGitHub::header(hub.github.heads.last(), "if-none-match"), QByteArray("\"" + sha('a').toLatin1() + "\""));
        // Nor does "update" fetch what it already has.
        QVERIFY(hub.update().value("upToDate").toBool());
        QCOMPARE(hub.github.downloads, 1);

        // The daily look leaves a source alone that was checked within the day.
        hub.github.asked.clear();
        hub.time += 3600;
        QVERIFY(!hub.sources->anyDue());
        QVERIFY(hub.checkAll(true, true).value("results").toArray().isEmpty());
        QVERIFY(hub.github.asked.isEmpty());
        hub.time += PluginSources::checkEvery;
        QVERIFY(hub.sources->anyDue());
        QCOMPARE(hub.checkAll(true, true).value("results").toArray().size(), 1);
        QCOMPARE(hub.github.downloads, 1);
    }

    void anUpdateInstallsItselfAndKeepsDataAndSwitch() {
        Hub hub;
        QVERIFY(hub.installed());
        QVERIFY(hub.settings.pluginEnabled("folder:storyteller", false));
        // What the plugin kept of its own.
        QVERIFY(QDir(hub.dir.path()).mkpath("plugins/storyteller/data"));
        QVERIFY(Fixtures::write(hub.dir.filePath("plugins/storyteller/data/stories.json"), "[\"one\"]"));

        Made newer;
        newer.version = "1.1";
        hub.github.push(hub.repository, "main", sha('b'), newer.packed());
        hub.time += 2 * PluginSources::checkEvery;
        const QJsonObject looked = hub.checkAll(true, true);
        const QJsonObject result = looked.value("results").toArray().first().toObject();
        QVERIFY2(result.value("updated").toBool(), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("from").toString(), QString("1.0"));
        QCOMPARE(result.value("to").toString(), QString("1.1"));
        QCOMPARE(hub.installedVersion(), QString("1.1"));
        // Its data and its switch are as they were, and nothing is left lying about.
        QCOMPARE(slurp(hub.dir.filePath("plugins/storyteller/data/stories.json")), QByteArray("[\"one\"]"));
        QVERIFY(hub.settings.pluginEnabled("folder:storyteller", false));
        QCOMPARE(QDir(hub.dir.filePath("plugins")).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{"storyteller"});
        QVERIFY(hub.sources->issues().isEmpty());
        // Switched off, an update leaves it off.
        hub.settings.setPluginEnabled("folder:storyteller", false);
        Made third;
        third.version = "1.2";
        hub.github.push(hub.repository, "main", sha('c'), third.packed());
        QVERIFY(hub.update().value("updated").toBool());
        QVERIFY(!hub.settings.pluginEnabled("folder:storyteller", true));

        // The version before is kept for one step back, by hand. What the
        // plugin wrote since comes along.
        QVERIFY(Fixtures::write(hub.dir.filePath("plugins/storyteller/data/stories.json"), "[\"one\", \"two\"]"));
        QVERIFY(hub.sources->list().first().toObject().value("previous").toBool());
        const QJsonObject back = hub.sources->rollBack("storyteller");
        QVERIFY2(!ScriptTools::failed(back), qPrintable(back.value("error").toString()));
        QCOMPARE(back.value("version").toString(), QString("1.1"));
        QCOMPARE(hub.installedVersion(), QString("1.1"));
        QCOMPARE(slurp(hub.dir.filePath("plugins/storyteller/data/stories.json")), QByteArray("[\"one\", \"two\"]"));
        QCOMPARE(hub.sources->source("storyteller").value("installed").toObject().value("commit").toString(), sha('b'));
        // Only one step: there is nothing before that.
        QVERIFY(ScriptTools::failed(hub.sources->rollBack("storyteller")));
        // What was gone back from is not taken again by itself, though it is
        // there to take by hand; the one after it is.
        hub.time += 2 * PluginSources::checkEvery;
        QVERIFY(!hub.checkAll(true, true).value("results").toArray().first().toObject().value("updated").toBool());
        QCOMPARE(hub.installedVersion(), QString("1.1"));
        QCOMPARE(hub.sources->issues().first().toObject().value("issue").toString(), QString("update"));
        Made fourth;
        fourth.version = "1.3";
        hub.github.push(hub.repository, "main", sha('d'), fourth.packed());
        hub.time += 2 * PluginSources::checkEvery;
        QVERIFY(hub.checkAll(true, true).value("results").toArray().first().toObject().value("updated").toBool());
        QCOMPARE(hub.installedVersion(), QString("1.3"));

        // Told only: with "apply" off, what is newer is said and left.
        Made fifth;
        fifth.version = "1.4";
        hub.github.push(hub.repository, "main", sha('e'), fifth.packed());
        hub.time += 2 * PluginSources::checkEvery;
        QVERIFY(hub.checkAll(false, true).value("results").toArray().first().toObject().value("update").toBool());
        QCOMPARE(hub.installedVersion(), QString("1.3"));
    }

    void anUpdateThatAsksForMoreWaitsForThePerson() {
        Hub hub;
        QVERIFY(hub.installed());
        Made greedy;
        greedy.version = "2.0";
        greedy.permissions = QStringList{"files", "http"};
        hub.github.push(hub.repository, "main", sha('b'), greedy.packed());

        // Not installed: it asks for the network, which the one installed doesn't have.
        const QJsonObject held = hub.update();
        QVERIFY(!ScriptTools::failed(held));
        QVERIFY(!held.value("updated").toBool() && held.value("review").toBool());
        QCOMPARE(held.value("pending").toObject().value("permissions").toVariant().toStringList(), QStringList{"http"});
        QCOMPARE(hub.installedVersion(), QString("1.0"));
        QJsonObject issue = hub.sources->issues().first().toObject();
        QCOMPARE(issue.value("issue").toString(), QString("review"));
        QCOMPARE(issue.value("name").toString(), QString("Storyteller"));
        QCOMPARE(issue.value("permissions").toVariant().toStringList(), QStringList{"http"});
        // The daily look doesn't fetch it again while it waits.
        const int downloads = hub.github.downloads;
        hub.time += 2 * PluginSources::checkEvery;
        hub.checkAll(true, true);
        QCOMPARE(hub.github.downloads, downloads);
        QCOMPARE(hub.installedVersion(), QString("1.0"));

        // Shown for review, it says what is new about it; approved, it goes in.
        const QJsonObject review = PluginSources::wait([&](PluginSources::Done done) { hub.sources->review("storyteller", done); }, 10000);
        QVERIFY(review.value("update").toBool());
        QCOMPARE(review.value("newPermissions").toVariant().toStringList(), QStringList{"http"});
        QCOMPARE(review.value("fromVersion").toString(), QString("1.0"));
        const QJsonObject approved = hub.sources->installLooked(false);
        QVERIFY2(approved.value("updated").toBool(), qPrintable(approved.value("error").toString()));
        QCOMPARE(hub.installedVersion(), QString("2.0"));
        QVERIFY(hub.settings.pluginEnabled("folder:storyteller", false));
        QVERIFY(hub.sources->issues().isEmpty());

        // One that goes by another name waits likewise, and `--yes` takes it.
        Made renamed;
        renamed.version = "2.1";
        renamed.permissions = QStringList{"files", "http"};
        renamed.title = "Something Else";
        hub.github.push(hub.repository, "main", sha('c'), renamed.packed());
        QCOMPARE(hub.update().value("pending").toObject().value("reason").toString(), QString("name"));
        QCOMPARE(hub.installedVersion(), QString("2.0"));
        QVERIFY(hub.update(true).value("updated").toBool());
        QCOMPARE(hub.installedVersion(), QString("2.1"));
        // Asking for less is no reason to wait.
        Made modest;
        modest.version = "2.2";
        modest.permissions = QStringList{};
        modest.title = "Something Else";
        hub.github.push(hub.repository, "main", sha('d'), modest.packed());
        QVERIFY(hub.update().value("updated").toBool());
    }

    void anUpdateThatDoesNotLoadIsTakenBackOut() {
        Hub hub;
        QVERIFY(hub.installed());
        QVERIFY(QDir(hub.dir.path()).mkpath("plugins/storyteller/data"));
        QVERIFY(Fixtures::write(hub.dir.filePath("plugins/storyteller/data/stories.json"), "[\"one\"]"));
        Made broken;
        broken.version = "1.1";
        broken.script = "var story = [];\nomabook.nothing.here();\n";
        hub.github.push(hub.repository, "main", sha('b'), broken.packed());

        const QJsonObject result = hub.update();
        QVERIFY(ScriptTools::failed(result));
        QVERIFY(result.value("rolledBack").toBool());
        QVERIFY2(result.value("error").toString().contains("1.1") && result.value("error").toString().contains("went back to 1.0"),
                 qPrintable(result.value("error").toString()));
        // The one that worked is back, with its data, and still on.
        QCOMPARE(hub.installedVersion(), QString("1.0"));
        QVERIFY(slurp(hub.dir.filePath("plugins/storyteller/main.js")).contains("commands.register"));
        QCOMPARE(slurp(hub.dir.filePath("plugins/storyteller/data/stories.json")), QByteArray("[\"one\"]"));
        QVERIFY(hub.settings.pluginEnabled("folder:storyteller", false));
        QVERIFY(ScriptTools::check(hub.context, "storyteller").value("ok").toBool());
        // It is said, and the broken one isn't tried again by itself.
        QCOMPARE(hub.sources->issues().first().toObject().value("issue").toString(), QString("source"));
        hub.time += 2 * PluginSources::checkEvery;
        const int downloads = hub.github.downloads;
        hub.checkAll(true, true);
        QCOMPARE(hub.github.downloads, downloads);
        QCOMPARE(hub.installedVersion(), QString("1.0"));
        // The fix that follows is.
        Made fixed;
        fixed.version = "1.2";
        hub.github.push(hub.repository, "main", sha('c'), fixed.packed());
        hub.time += 2 * PluginSources::checkEvery;
        QVERIFY(hub.checkAll(true, true).value("results").toArray().first().toObject().value("updated").toBool());
        QCOMPARE(hub.installedVersion(), QString("1.2"));
        QVERIFY(hub.sources->issues().isEmpty());
    }

    void aSourceThatIsGoneLeavesThePluginAlone() {
        Hub hub;
        QVERIFY(hub.installed());
        const QByteArray script = slurp(hub.dir.filePath("plugins/storyteller/main.js"));

        // The repository is deleted, or made private.
        hub.github.repos[hub.repository].gone = true;
        QJsonObject checked = hub.check();
        QVERIFY(ScriptTools::failed(checked) && checked.value("gone").toBool());
        QCOMPARE(slurp(hub.dir.filePath("plugins/storyteller/main.js")), script);
        QJsonObject issue = hub.sources->issues().first().toObject();
        QCOMPARE(issue.value("issue").toString(), QString("source"));
        QVERIFY(issue.value("problem").toString().contains("isn't on GitHub any more"));
        // Back again, the problem is gone with the next look.
        hub.github.repos[hub.repository].gone = false;
        QVERIFY(!ScriptTools::failed(hub.check()));
        QVERIFY(hub.sources->issues().isEmpty());

        // It moved: GitHub sends the question on, and Omabook doesn't follow
        // a plugin to an address the person never saw.
        hub.github.repos[hub.repository].moved = true;
        hub.github.push(hub.repository, "main", sha('b'), Made().packed());
        checked = hub.update();
        QVERIFY2(checked.value("error").toString().contains("moved"), qPrintable(checked.value("error").toString()));
        QCOMPARE(hub.installedVersion(), QString("1.0"));
        QCOMPARE(hub.sources->source("storyteller").value("installed").toObject().value("commit").toString(), sha('a'));
        hub.github.repos[hub.repository].moved = false;

        // What is there now is no plugin: a repository that was emptied, one
        // whose plugin.json is broken, one written for a newer Omabook.
        hub.github.push(hub.repository, "main", sha('c'), tarball({{"someone-repo-ccc/README.md", "Moved elsewhere.\n", {}}}));
        checked = hub.update();
        QVERIFY2(checked.value("error").toString().contains("not a plugin"), qPrintable(checked.value("error").toString()));
        QCOMPARE(slurp(hub.dir.filePath("plugins/storyteller/main.js")), script);
        QCOMPARE(hub.sources->issues().first().toObject().value("issue").toString(), QString("source"));
        // Off the network altogether, nothing is said against the source.
        hub.github.close();
        hub.github.push(hub.repository, "main", sha('d'), Made().packed());
        QVERIFY(!ScriptTools::failed(hub.sources->forget("storyteller")));
        // (Remembered again, without GitHub: as the file would have it.)
        QVERIFY(Fixtures::write(hub.dir.filePath("plugins/sources.json"),
                                QJsonDocument(QJsonObject{{"sources", QJsonObject{{"storyteller", QJsonObject{
                                    {"repository", hub.repository}, {"channel", QJsonObject{{"kind", "auto"}}},
                                    {"installed", QJsonObject{{"version", "1.0"}, {"commit", sha('a')}, {"ref", "main"}}}}}}}}).toJson()));
        checked = hub.check();
        QVERIFY(checked.value("offline").toBool());
        QVERIFY(hub.sources->issues().isEmpty());
        QCOMPARE(qint64(hub.sources->source("storyteller").value("checked_at").toDouble()), qint64(0));
    }

    void gitHubAskingToWaitIsHonoured() {
        Hub hub;
        QVERIFY(hub.installed());
        hub.github.limited = true;
        hub.github.reset = hub.time + 1800;
        hub.github.asked.clear();

        QJsonObject checked = hub.check();
        QVERIFY(checked.value("limited").toBool());
        QCOMPARE(int(hub.github.asked.size()), 1);
        // Until then nothing is asked, by this run or the next, of any source.
        QVERIFY(hub.check().value("limited").toBool());
        hub.again();
        QVERIFY(hub.update().value("limited").toBool());
        QVERIFY(hub.look("someone/other").value("limited").toBool());
        QVERIFY(hub.checkAll(true, false).value("limited").toBool());
        QCOMPARE(int(hub.github.asked.size()), 1);
        // It is nothing against the source, and the plugin is as it was.
        QVERIFY(hub.sources->issues().isEmpty());
        QCOMPARE(hub.installedVersion(), QString("1.0"));
        // After the time GitHub named, it is asked again.
        hub.github.limited = false;
        hub.time += 1801;
        QVERIFY(!ScriptTools::failed(hub.check()));
        // Whether there is a release by now, and where the branch stands.
        QCOMPARE(int(hub.github.asked.size()), 3);
    }

    void whatIsFetchedIsHeldToTheSameChecks() {
        Hub hub;
        const QString top = "someone-evil-abc/";
        const QByteArray manifest = "{\"name\": \"Evil\", \"script\": \"main.js\"}";
        struct Case {
            const char *what;
            QList<TarEntry> entries;
            const char *says;
        };
        const QList<Case> cases{
            {"a step up and out", {{top + "plugin.json", manifest, {}}, {top + "main.js", "1", {}}, {top + "../../evil.js", "x", {}}}, "outside"},
            {"an absolute path", {{top + "plugin.json", manifest, {}}, {top + "main.js", "1", {}}, {"/tmp/omabook-evil.js", "x", {}}}, "outside"},
            {"a link", {{top + "plugin.json", manifest, {}}, {top + "main.js", "1", {}}, {top + "notes", {}, "/etc/passwd"}}, "link"},
            {"a file too large", {{top + "plugin.json", manifest, {}}, {top + "main.js", QByteArray(9000, ' '), {}}}, "larger"},
            {"too many files", {{top + "plugin.json", manifest, {}}, {top + "main.js", "1", {}}, {top + "a.js", "1", {}},
                                {top + "b.js", "1", {}}, {top + "c.js", "1", {}}}, "files"},
            {"no plugin at all", {{top + "README.md", "hello", {}}}, "plugin.json"},
            {"a permission nobody knows", {{top + "plugin.json", "{\"script\": \"main.js\", \"permissions\": [\"root\"]}", {}},
                                           {top + "main.js", "1", {}}}, "permission"},
        };
        PluginTools::Limits small;
        small.files = 4;
        small.fileBytes = 4096;
        small.totalBytes = 64 * 1024;
        hub.sources->setLimits(small);
        char letter = 'a';
        for (const Case &test : cases) {
            hub.github.push("someone/evil", "main", sha(letter++), tarball(test.entries));
            PluginSources::AddOptions options;
            options.enable = true;
            const QJsonObject answer = hub.add("someone/evil", options);
            QVERIFY2(ScriptTools::failed(answer), test.what);
            QVERIFY2(answer.value("error").toString().contains(test.says),
                     qPrintable(QString("%1: %2").arg(test.what, answer.value("error").toString())));
            // Nothing of it is installed, remembered or switched on.
            QVERIFY2(leftIn(hub.dir.filePath("plugins")).isEmpty(), test.what);
            QVERIFY2(!QFile::exists(hub.dir.filePath("evil.js")) && !QFile::exists("/tmp/omabook-evil.js"), test.what);
            QVERIFY2(!hub.settings.pluginEnabled("folder:evil", false), test.what);
        }
        // A download larger than a plugin may be is cut off, not unpacked.
        QByteArray noise(200 * 1024, '\0');
        for (qsizetype i = 0; i < noise.size(); ++i)
            noise[i] = char(QRandomGenerator::global()->bounded(256));
        hub.github.push("someone/evil", "main", sha('z'), tarball({{top + "plugin.json", manifest, {}}, {top + "main.js", noise, {}}}));
        QVERIFY(hub.github.repos["someone/evil"].tarballs.value(sha('z')).size() > small.totalBytes);
        const QJsonObject large = hub.add("someone/evil");
        QVERIFY2(large.value("error").toString().contains("larger"), qPrintable(large.value("error").toString()));
        QVERIFY(leftIn(hub.dir.filePath("plugins")).isEmpty());
    }

    void aDownloadSentElsewhereIsNotFetched() {
        Hub hub;
        hub.github.push(hub.repository, "main", sha('a'), Made().packed());
        // Somewhere that answers, and isn't one of GitHub's own addresses.
        FakeGitHub elsewhere;
        elsewhere.push(hub.repository, "main", sha('a'), Made().packed());
        hub.github.sendDownloadsTo = QString("http://localhost:%1").arg(elsewhere.serverPort());
        QJsonObject answer = hub.add(hub.repository);
        QVERIFY2(answer.value("error").toString().contains("isn't GitHub"), qPrintable(answer.value("error").toString()));
        QVERIFY(elsewhere.asked.isEmpty());
        QVERIFY(leftIn(hub.dir.filePath("plugins")).isEmpty());
        // Nor to somewhere that isn't HTTPS, when that is what is asked for.
        hub.github.sendDownloadsTo.clear();
        PluginSources::Service strict;
        strict.api = hub.github.api();
        strict.hosts = QStringList{"127.0.0.1"};
        hub.sources->setService(strict);
        answer = hub.add(hub.repository);
        QVERIFY2(answer.value("error").toString().contains("HTTPS"), qPrintable(answer.value("error").toString()));
        QVERIFY(leftIn(hub.dir.filePath("plugins")).isEmpty());
        // As shipped, it is GitHub's own addresses over HTTPS and no others.
        const PluginSources::Service shipped;
        QCOMPARE(shipped.api, QString("https://api.github.com"));
        QVERIFY(shipped.httpsOnly && shipped.hosts.contains("codeload.github.com") && !shipped.hosts.contains("localhost"));
    }

    void aPluginSaysWhichOmabookItNeeds() {
        Hub hub;
        const QString here = ScriptApi::hostVersion();
        // What it asks for is read as a version; missing, equal and older are fine.
        QVERIFY(ScriptApi::needsNewer(QJsonValue::Undefined).isEmpty());
        QVERIFY(ScriptApi::needsNewer(here).isEmpty());
        QVERIFY(ScriptApi::needsNewer("0.0.1").isEmpty());
        QCOMPARE(ScriptApi::needsNewer("99.0"), QString("Needs Omabook 99.0 or newer; this is %1.").arg(here));
        for (const QJsonValue &odd : {QJsonValue("newest"), QJsonValue(3), QJsonValue("1.0-beta"), QJsonValue("")}) {
            bool malformed = false;
            QVERIFY(!ScriptApi::needsNewer(odd, &malformed).isEmpty());
            QVERIFY(malformed);
        }
        QCOMPARE(ScriptApi::json().value("omabook").toString(), here);

        // A new plugin says the version it was started on, and loads.
        QVERIFY(!ScriptTools::failed(ScriptTools::create(hub.context, "mine", "hook")));
        QCOMPARE(QJsonDocument::fromJson(slurp(hub.dir.filePath("plugins/mine/plugin.json"))).object().value("omabook").toString(), here);
        QVERIFY(ScriptTools::check(hub.context, "mine").value("ok").toBool());

        // One written for a newer Omabook is not loaded or run, and says why.
        auto withNeed = [&](const QString &name, const QJsonValue &need, bool program = false) {
            QJsonObject manifest{{"name", name}, {"omabook", need}};
            if (program)
                manifest.insert("command", QJsonArray{"sh"});
            else
                manifest.insert("script", "main.js");
            return QDir(hub.dir.path()).mkpath("plugins/" + name)
                   && Fixtures::write(hub.dir.filePath("plugins/" + name + "/plugin.json"), QJsonDocument(manifest).toJson())
                   && Fixtures::write(hub.dir.filePath("plugins/" + name + "/main.js"), "omabook.settings.set(\"ran\", true);\n");
        };
        QVERIFY(withNeed("future", "99.0"));
        QVERIFY(withNeed("future-program", "99.0", true));
        QVERIFY(withNeed("odd", "soon"));
        const QJsonObject checked = ScriptTools::check(hub.context, "future");
        QVERIFY(!checked.value("ok").toBool());
        QVERIFY2(checked.value("error").toString().contains("Needs Omabook 99.0 or newer"), qPrintable(checked.value("error").toString()));
        QVERIFY(ScriptTools::check(hub.context, "odd").value("error").toString().contains("should be the oldest version"));
        ScriptServices services;
        services.settings = &hub.settings;
        ScriptPlugins scripts(services, hub.dir.filePath("plugins"), nullptr, false);
        QVERIFY(scripts.plugin("mine"));
        QVERIFY(!scripts.plugin("future"));
        QVERIFY(!QFile::exists(hub.dir.filePath("plugins/future/data/settings.json")));
        // In the manager it needs something, and isn't called broken; a
        // program that asks the same is not among the sources that are asked.
        Plugins plugins(&hub.settings, hub.dir.filePath("plugins"), "-");
        const QJsonArray overview = PluginTools::overview(plugins.known(), &scripts);
        QCOMPARE(byId(overview, "folder:future").value("state").toString(), QString("needs"));
        QCOMPARE(byId(overview, "folder:future").value("note").toString(), QString("Needs Omabook 99.0 or newer; this is %1.").arg(here));
        QVERIFY(!byId(overview, "folder:future").contains("error"));
        QCOMPARE(byId(overview, "folder:future-program").value("state").toString(), QString("needs"));
        QCOMPARE(byId(overview, "folder:mine").value("state").toString(), QString("on"));
        QNetworkAccessManager network;
        QObject holder;
        for (const MetadataSource *source : plugins.sources(plugins.known(), &network, &holder))
            QVERIFY(source->name() != "future-program");
        QStringList issues;
        for (const QJsonValue &value : PluginTools::health(overview, plugins.calibre()))
            issues << value.toObject().value("id").toString() + " " + value.toObject().value("issue").toString();
        QVERIFY2(issues.contains("folder:future needs-omabook") && issues.contains("folder:future-program needs-omabook"),
                 qPrintable(issues.join(", ")));

        // Installing one is refused, from a folder as from a repository.
        QVERIFY(QDir(hub.dir.path()).mkpath("handed"));
        QVERIFY(QFile::copy(hub.dir.filePath("plugins/future/plugin.json"), hub.dir.filePath("handed/plugin.json")));
        QVERIFY(QFile::copy(hub.dir.filePath("plugins/future/main.js"), hub.dir.filePath("handed/main.js")));
        QVERIFY(PluginTools::install(hub.context, hub.dir.filePath("handed")).value("error").toString().contains("needs Omabook 99.0"));
        QVERIFY(QFile::copy(hub.dir.filePath("plugins/odd/plugin.json"), hub.dir.filePath("handed/odd.json")));
        QVERIFY(QFile::remove(hub.dir.filePath("handed/plugin.json")) && QFile::rename(hub.dir.filePath("handed/odd.json"), hub.dir.filePath("handed/plugin.json")));
        QVERIFY(PluginTools::install(hub.context, hub.dir.filePath("handed")).value("error").toString().contains("oldest version"));
        Made future;
        future.version = "3.0";
        const QByteArray futureManifest = "{\"name\": \"Storyteller\", \"version\": \"3.0\", \"omabook\": \"99.0\", \"script\": \"main.js\", "
                                          "\"permissions\": [\"files\"]}";
        const QByteArray packed = tarball({{"top/plugin.json", futureManifest, {}}, {"top/main.js", "var a = 1;\n", {}}});
        hub.github.push("someone/future", "main", sha('f'), packed);
        QVERIFY(hub.add("someone/future").value("error").toString().contains("needs Omabook 99.0"));
        QVERIFY(!QFile::exists(hub.dir.filePath("plugins/future/data")));

        // An update to such a version is not applied: the one installed
        // stays, and it is said that the update waits for a newer Omabook.
        QVERIFY(hub.installed());
        hub.github.push(hub.repository, "main", sha('b'), packed);
        const QJsonObject held = hub.update();
        QVERIFY(!held.value("updated").toBool() && held.value("waits").toBool());
        QCOMPARE(hub.installedVersion(), QString("1.0"));
        QJsonObject waiting;
        for (const QJsonValue &value : hub.sources->issues())
            if (value.toObject().value("id").toString() == "folder:storyteller")
                waiting = value.toObject();
        QCOMPARE(waiting.value("issue").toString(), QString("update-needs-omabook"));
        QCOMPARE(waiting.value("needs").toString(), QString("99.0"));
        // Not even approving installs what can't run here.
        QVERIFY(!hub.update(true).value("updated").toBool());
        QCOMPARE(hub.installedVersion(), QString("1.0"));
    }

    void aTokenOpensTheApiAndGoesNowhereElse() {
        Hub hub;
        // The keyring is the test's own.
        SessionSecrets secrets;
        Secrets::useInstead(&secrets);
        const auto restore = qScopeGuard([] { Secrets::useInstead(nullptr); });
        QVERIFY(secrets.put("github", "s3cret-token"));
        hub.github.push(hub.repository, "main", sha('a'), Made().packed());
        // Downloads come from another of GitHub's addresses, as they really do.
        FakeGitHub downloads;
        downloads.push(hub.repository, "main", sha('a'), Made().packed());
        hub.github.sendDownloadsTo = QString("http://localhost:%1").arg(downloads.serverPort());
        const auto withDownloads = [&] {
            PluginSources::Service service;
            service.api = hub.github.api();
            service.hosts = QStringList{"127.0.0.1", "localhost"};
            service.httpsOnly = false;
            hub.sources->setService(service);
        };

        // The keyring is not even asked unless a token was put there.
        withDownloads();
        QVERIFY(!ScriptTools::failed(hub.look(hub.repository)));
        for (const QByteArray &head : hub.github.heads + downloads.heads)
            QVERIFY(FakeGitHub::header(head, "authorization").isEmpty());

        // With one, it goes to the API, and not along to where the download is.
        hub.settings.setPluginToken(true);
        hub.again();
        withDownloads();
        hub.github.heads.clear();
        downloads.heads.clear();
        PluginSources::AddOptions options;
        const QJsonObject added = hub.add(hub.repository, options);
        QVERIFY2(!ScriptTools::failed(added), qPrintable(added.value("error").toString()));
        QVERIFY(!hub.github.heads.isEmpty() && !downloads.heads.isEmpty());
        for (const QByteArray &head : std::as_const(hub.github.heads))
            QCOMPARE(FakeGitHub::header(head, "authorization"), QByteArray("Bearer s3cret-token"));
        for (const QByteArray &head : std::as_const(downloads.heads))
            QVERIFY(FakeGitHub::header(head, "authorization").isEmpty());
        // And it is written nowhere: not in the settings, not with the sources.
        QVERIFY(!slurp(hub.dir.filePath("config.json")).contains("s3cret"));
        QVERIFY(!slurp(hub.dir.filePath("plugins/sources.json")).contains("s3cret"));
        QVERIFY(!QJsonDocument(added).toJson().contains("s3cret"));
    }

    void theDailyLookDoesNotHoldUpTheStart() {
        Hub hub;
        QVERIFY(hub.installed());
        Made newer;
        newer.version = "1.1";
        hub.github.push(hub.repository, "main", sha('b'), newer.packed());
        // Last checked long ago, as far as the real clock goes.
        QJsonObject all = QJsonDocument::fromJson(slurp(hub.dir.filePath("plugins/sources.json"))).object();
        QJsonObject sources = all.value("sources").toObject();
        QJsonObject entry = sources.value("storyteller").toObject();
        entry.insert("checked_at", 1000);
        sources.insert("storyteller", entry);
        all.insert("sources", sources);
        QVERIFY(Fixtures::write(hub.dir.filePath("plugins/sources.json"), QJsonDocument(all).toJson()));
        hub.github.asked.clear();

        // The window's manager finds GitHub where the environment says, as
        // the tests of the command line do.
        qputenv("OMABOOK_GITHUB_API", hub.github.api().toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("OMABOOK_GITHUB_API"); });
        Plugins registry(&hub.settings, hub.dir.filePath("plugins"), "-");
        ScriptServices services;
        services.settings = &hub.settings;
        ScriptPlugins scripts(services, hub.dir.filePath("plugins"));
        FakePicker picker;
        QNetworkAccessManager network;
        QStringList said;
        {
            // Told never to check, nothing is asked however long it waits.
            hub.settings.setPluginUpdates("never");
            PluginManager quiet(&hub.settings, &registry, &scripts, &picker, &network);
            quiet.checkSoon(0);
            QTest::qWait(200);
            QVERIFY(hub.github.asked.isEmpty());
        }
        hub.settings.setPluginUpdates("auto");
        QElapsedTimer started;
        started.start();
        PluginManager manager(&hub.settings, &registry, &scripts, &picker, &network);
        connect(&manager, &PluginManager::said, &manager, [&](const QString &text) { said << text; });
        manager.checkSoon(0);
        // Made, and everything it shows is there, before GitHub was asked anything.
        QVERIFY(started.elapsed() < 2000);
        QVERIFY(hub.github.asked.isEmpty());
        QVERIFY(manager.sourced());
        QVERIFY(!manager.plugins().isEmpty());
        // And then, beside whatever else goes on, the update comes in and is said once.
        QTRY_COMPARE_WITH_TIMEOUT(hub.installedVersion(), QString("1.1"), 10000);
        QTRY_COMPARE(said, QStringList{"Storyteller was updated from 1.0 to 1.1."});
        QTRY_VERIFY(manager.checking().isEmpty());
        QVERIFY(scripts.plugin("storyteller"));
        // "Tell me" says there is one and leaves it.
        Made third;
        third.version = "1.2";
        hub.github.push(hub.repository, "main", sha('c'), third.packed());
        hub.settings.setPluginUpdates("tell");
        said.clear();
        manager.checkAll();
        QTRY_VERIFY(manager.checking().isEmpty());
        QCOMPARE(hub.installedVersion(), QString("1.1"));
        QVariantMap health;
        for (const QVariant &issue : manager.health())
            if (issue.toMap().value("issue").toString() == "update")
                health = issue.toMap();
        QCOMPARE(health.value("id").toString(), QString("folder:storyteller"));
        // Updated by hand from there, with a look at it first when it asks for more.
        manager.updateNow("folder:storyteller");
        QTRY_COMPARE_WITH_TIMEOUT(hub.installedVersion(), QString("1.2"), 10000);
        QTRY_VERIFY(manager.checking().isEmpty());
        QCOMPARE(manager.sourceNote().value("text").toString(), QString("Storyteller was updated from 1.1 to 1.2."));

        // An address looked up for the install sheet is put on offer, and
        // installed only once it was said how.
        hub.github.push("someone/omabook-shelf-notes", "main", sha('d'), Made().packed("someone-omabook-shelf-notes-ddd"));
        manager.lookUp("someone/omabook-shelf-notes");
        QVERIFY(manager.fetching());
        QTRY_VERIFY(!manager.fetching());
        QCOMPARE(manager.offer().value("name").toString(), QString("shelf-notes"));
        QCOMPARE(manager.offer().value("from").toString(), QString("github"));
        QVERIFY(!QFile::exists(hub.dir.filePath("plugins/shelf-notes")));
        QVERIFY(!manager.install(false).contains("error"));
        QVERIFY(QFile::exists(hub.dir.filePath("plugins/shelf-notes/main.js")));
        QVERIFY(!hub.settings.pluginEnabled("folder:shelf-notes", true));
        QCOMPARE(manager.sources()->list().size(), 2);
        manager.forgetSource("folder:shelf-notes");
        QCOMPARE(manager.sources()->list().size(), 1);
    }

    void anAgentAddsAndUpdatesFromGitHub() {
        Hub hub;
        hub.github.push(hub.repository, "main", sha('a'), Made().packed());
        qputenv("OMABOOK_GITHUB_API", hub.github.api().toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("OMABOOK_GITHUB_API"); });
        const QString folder = hub.dir.filePath("cli-plugins");

        QCOMPARE(runPluginCli({"omabook", "plugin", "add", "https://github.com/someone/omabook-storyteller", "--folder", folder}), 0);
        QVERIFY(QFile::exists(folder + "/storyteller/main.js"));
        QVERIFY(!Settings().pluginEnabled("folder:storyteller", true));
        QCOMPARE(runPluginCli({"omabook", "plugin", "sources", "--folder", folder, "--json"}), 0);
        QCOMPARE(runPluginCli({"omabook", "plugin", "update", "storyteller", "--check", "--folder", folder}), 0);
        Made newer;
        newer.version = "1.1";
        hub.github.push(hub.repository, "main", sha('b'), newer.packed());
        QCOMPARE(runPluginCli({"omabook", "plugin", "update", "--all", "--folder", folder}), 0);
        QVERIFY(slurp(folder + "/storyteller/plugin.json").contains("1.1"));
        QCOMPARE(runPluginCli({"omabook", "plugin", "rollback", "storyteller", "--folder", folder}), 0);
        QVERIFY(slurp(folder + "/storyteller/plugin.json").contains("1.0"));
        QCOMPARE(runPluginCli({"omabook", "plugin", "pin", "storyteller", "nothing-there", "--folder", folder}), 1);
        QCOMPARE(runPluginCli({"omabook", "plugin", "update", "--folder", folder}), 1);
        QCOMPARE(runPluginCli({"omabook", "plugin", "add", "https://example.com/a/b", "--folder", folder}), 1);
        QCOMPARE(runPluginCli({"omabook", "plugin", "sources", "--updates", "sometimes", "--folder", folder}), 1);
        QCOMPARE(runPluginCli({"omabook", "plugin", "sources", "--updates", "tell", "--folder", folder}), 0);
        QCOMPARE(Settings().pluginUpdates(), QString("tell"));
        Settings().setPluginUpdates("auto");
        QCOMPARE(runPluginCli({"omabook", "plugin", "forget-source", "storyteller", "--folder", folder}), 0);
        QCOMPARE(runPluginCli({"omabook", "plugin", "forget-source", "storyteller", "--folder", folder}), 1);
        QVERIFY(QFile::exists(folder + "/storyteller/main.js"));

        // The same as tools, on the user's own plugin folder, which is the test's home.
        const QString plugins = home.filePath("config/omabook/plugins");
        Mcp::Result result = callTool("plugin_add", {{"address", "someone/omabook-storyteller"}});
        QVERIFY2(!result.failed(), qPrintable(result.error));
        QCOMPARE(result.data.value("enabled").toBool(), false);
        QCOMPARE(result.data.value("permissions").toVariant().toStringList(), QStringList{"files"});
        QVERIFY(QFile::exists(plugins + "/storyteller/plugin.json"));
        result = callTool("plugin_sources", {});
        QCOMPARE(result.data.value("sources").toArray().first().toObject().value("repository").toString(), hub.repository);
        Made third;
        third.version = "1.2";
        hub.github.push(hub.repository, "main", sha('c'), third.packed());
        result = callTool("plugin_update", {{"name", "storyteller"}, {"check", true}});
        QVERIFY2(!result.failed(), qPrintable(result.error));
        QVERIFY(result.data.value("update").toBool());
        result = callTool("plugin_update", {});
        QVERIFY(result.data.value("results").toArray().first().toObject().value("updated").toBool());
        QVERIFY(!callTool("plugin_forget_source", {{"name", "storyteller"}}).failed());
        QVERIFY(callTool("plugin_forget_source", {{"name", "storyteller"}}).failed());
        QVERIFY(!callTool("plugin_remove", {{"name", "storyteller"}}).failed());
    }

    // --- Finding plugins ---------------------------------------------------

    void theRegistryShippedPassesItsOwnCheck() {
        const QJsonObject shipped = PluginFinder::shipped();
        QVERIFY2(PluginFinder::problem(shipped).isEmpty(), qPrintable(PluginFinder::problem(shipped)));
        Hub hub;
        PluginFinder finder(hub.context);
        finder.setDiscovery(false);
        const QJsonObject storyteller = finder.listed("storyteller");
        QCOMPARE(storyteller.value("repository").toString(), QString("jellespijker/omabook-storyteller"));
        QVERIFY(storyteller.value("reviewed").toBool());
        QCOMPARE(PluginFinder::addressOf(storyteller), QString("jellespijker/omabook-storyteller"));
        QVERIFY(storyteller.value("permissions").toArray().contains("files"));
        // What a registry must hold, each said.
        const auto with = [&shipped](const QString &field, const QJsonValue &value) {
            QJsonObject registry = shipped;
            QJsonArray plugins = registry.value("plugins").toArray();
            QJsonObject entry = plugins.first().toObject();
            if (value.isUndefined())
                entry.remove(field);
            else
                entry.insert(field, value);
            plugins[0] = entry;
            registry.insert("plugins", plugins);
            return PluginFinder::problem(registry);
        };
        QVERIFY(with("name", "Not A Name").contains("no proper name"));
        QVERIFY(with("repository", "nowhere").contains("names no repository"));
        QVERIFY(with("permissions", QJsonArray{"everything"}).contains("\"everything\""));
        QVERIFY(with("kinds", QJsonArray{"magic"}).contains("\"magic\""));
        QVERIFY(with("reviewed", QJsonValue::Undefined).contains("reviewed"));
        QVERIFY(with("omabook", "soon").contains("version of Omabook"));
        QVERIFY(PluginFinder::problem(QJsonObject{{"version", 2}}).contains("version 1"));
        QJsonObject twice = shipped;
        twice.insert("plugins", QJsonArray{shipped.value("plugins").toArray().first(), shipped.value("plugins").toArray().first()});
        QVERIFY(PluginFinder::problem(twice).contains("twice"));
    }

    void theRegistryIsFetchedOnceADayAndABadOneIsRefused() {
        Hub hub;
        PluginFinder finder(hub.context);
        PluginSources::Service service;
        service.api = hub.github.api();
        service.raw = hub.github.api() + "/raw";
        service.hosts = QStringList{"127.0.0.1"};
        service.httpsOnly = false;
        finder.setService(service);
        finder.setClock([&hub] { return hub.time; });
        finder.setDiscovery(true);
        const auto refresh = [&finder](bool force = false) {
            return PluginSources::wait([&](PluginSources::Done done) { finder.refresh(force, done); }, 10000);
        };
        const QString path = "jellespijker/omabook/main/share/omabook/plugins.json";
        // Offline (nothing there): quietly the shipped one, asked again in an hour.
        QJsonObject answer = refresh();
        QVERIFY(!answer.value("problem").toString().isEmpty() && !ScriptTools::failed(answer));
        QVERIFY(!finder.listed("storyteller").isEmpty());
        QVERIFY(finder.listed("reading-log").isEmpty());
        const qsizetype asked = hub.github.asked.size();
        QVERIFY(refresh().value("unchanged").toBool());
        QCOMPARE(hub.github.asked.size(), asked);

        // One listed since this Omabook was built is found.
        QJsonObject registry = PluginFinder::shipped();
        QJsonArray plugins = registry.value("plugins").toArray();
        plugins << QJsonObject{{"name", "reading-log"}, {"title", "Reading Log"}, {"description", "Keeps a log of what you read."},
                               {"repository", "someone/omabook-reading-log"}, {"author", "Someone"}, {"license", "MIT"},
                               {"permissions", QJsonArray{"files"}}, {"omabook", "0.1.0"}, {"kinds", QJsonArray{"hook"}}, {"reviewed", false}};
        registry.insert("plugins", plugins);
        hub.github.raw.insert(path, QJsonDocument(registry).toJson());
        hub.time += 3601;
        QVERIFY(refresh().value("refreshed").toBool());
        QCOMPARE(finder.listed("reading-log").value("title").toString(), QString("Reading Log"));
        // Within the day nothing is asked; after it, only whether it changed.
        hub.time += 3600;
        const qsizetype fetched = hub.github.asked.size();
        refresh();
        QCOMPARE(hub.github.asked.size(), fetched);
        hub.time += PluginSources::checkEvery;
        QVERIFY(refresh().value("unchanged").toBool());
        QVERIFY(!FakeGitHub::header(hub.github.heads.last(), "if-none-match").isEmpty());
        // A registry that doesn't pass is not taken: the last good one stays.
        hub.github.raw.insert(path, "{\"version\": 1, \"plugins\": [{\"name\": \"BAD NAME\"}]}");
        hub.time += PluginSources::checkEvery;
        answer = refresh(true);
        QVERIFY(answer.value("problem").toString().contains("no proper name"));
        QCOMPARE(finder.listed("reading-log").value("title").toString(), QString("Reading Log"));
        hub.github.raw.insert(path, "not json");
        QVERIFY(refresh(true).value("problem").toString().contains("can't be read"));
        QVERIFY(!finder.listed("reading-log").isEmpty());

        // With looking on GitHub switched off, nothing is asked at all.
        const qsizetype before = hub.github.asked.size();
        hub.settings.setPluginDiscovery(false);
        PluginFinder quiet(hub.context);
        quiet.setService(service);
        const QJsonObject found = PluginSources::wait([&](PluginSources::Done done) { quiet.search("", done); }, 10000);
        QVERIFY(!found.value("discovery").toBool());
        QCOMPARE(found.value("results").toArray().first().toObject().value("name").toString(), QString("storyteller"));
        QCOMPARE(hub.github.asked.size(), before);
        hub.settings.setPluginDiscovery(true);
    }

    void pluginsAreFoundByTheirTopicAndCheckedFirst() {
        Hub hub;
        PluginFinder finder(hub.context);
        PluginSources::Service service;
        service.api = hub.github.api();
        service.raw = hub.github.api() + "/raw";
        service.hosts = QStringList{"127.0.0.1"};
        service.httpsOnly = false;
        finder.setService(service);
        finder.setClock([&hub] { return hub.time; });
        finder.setDiscovery(true);
        const auto manifest = [](const QString &name, const QString &omabook = "0.1.0", const QJsonArray &permissions = {}) {
            return QJsonDocument(QJsonObject{{"name", name}, {"version", "1.0"}, {"script", "main.js"}, {"omabook", omabook},
                                             {"description", name + " does a thing."}, {"permissions", permissions}}).toJson();
        };
        // Two a page, so the search pages.
        hub.github.pageSize = 2;
        hub.github.topical = {FakeGitHub::found("jellespijker/omabook-storyteller", 7), FakeGitHub::found("someone/omabook-reading-log", 3),
                              FakeGitHub::found("someone/omabook-tools", 1), FakeGitHub::found("someone/omabook-future"),
                              FakeGitHub::found("someone/omabook-old", 0, true), FakeGitHub::found("someone/omabook-empty")};
        hub.github.files["jellespijker/omabook-storyteller"]["plugin.json"] = manifest("Storyteller", "0.1.4", {"files", "http"});
        hub.github.files["someone/omabook-reading-log"]["plugin.json"] = manifest("Reading Log", "0.1.0", {"files"});
        hub.github.files["someone/omabook-tools"]["tools/plugin.json"] = manifest("Tools");
        hub.github.files["someone/omabook-future"]["plugin.json"] = manifest("Future", "99.0");
        hub.github.files["someone/omabook-old"]["plugin.json"] = manifest("Old");
        const auto search = [&finder](const QString &words = {}) {
            return PluginSources::wait([&](PluginSources::Done done) { finder.search(words, done); }, 10000);
        };
        QJsonObject found = search();
        QVERIFY2(!ScriptTools::failed(found), qPrintable(found.value("error").toString()));
        QCOMPARE(hub.github.count("/search/repositories"), 3);
        const QJsonArray results = found.value("results").toArray();
        QStringList names;
        for (const QJsonValue &one : results)
            names << one.toObject().value("name").toString();
        QCOMPARE(names, (QStringList{"storyteller", "reading-log", "tools"}));
        // The listed one first, with what GitHub says of it, and shown once.
        const QJsonObject storyteller = results.at(0).toObject();
        QVERIFY(storyteller.value("listed").toBool() && storyteller.value("reviewed").toBool());
        QCOMPARE(storyteller.value("stars").toInt(), 7);
        QVERIFY(!results.at(1).toObject().value("listed").toBool() && !results.at(1).toObject().value("reviewed").toBool());
        QCOMPARE(results.at(1).toObject().value("permissions").toArray(), QJsonArray{"files"});
        QCOMPARE(results.at(1).toObject().value("license").toString(), QString("MIT"));
        QCOMPARE(results.at(1).toObject().value("updated").toString(), QString("2026-10-01"));
        QCOMPARE(results.at(2).toObject().value("subfolder").toString(), QString("tools"));
        QCOMPARE(results.at(2).toObject().value("address").toString(), QString("https://github.com/someone/omabook-tools/tree/main/tools"));
        // What wasn't offered, and why.
        QStringList why;
        for (const QJsonValue &one : found.value("skipped").toArray())
            why << one.toObject().value("repository").toString() + ": " + one.toObject().value("problem").toString();
        QCOMPARE(why.size(), 3);
        QVERIFY(why.join('\n').contains("omabook-future: ") && why.join('\n').contains("99.0"));
        QVERIFY(why.join('\n').contains("omabook-old: It is archived"));
        QVERIFY(why.join('\n').contains("omabook-empty: It has no plugin.json"));
        // Only the one file of each was fetched, never an archive.
        QCOMPARE(hub.github.count("/tarball"), 0);

        // The same search again is kept for a while; other words ask again.
        const qsizetype asked = hub.github.asked.size();
        QCOMPARE(search().value("results").toArray().size(), 3);
        QCOMPARE(hub.github.asked.size(), asked);
        QCOMPARE(search("reading").value("results").toArray().last().toObject().value("name").toString(), QString("reading-log"));
        QVERIFY(hub.github.asked.size() > asked);
        // A repository that wasn't pushed to isn't looked into again.
        QCOMPARE(hub.github.count("omabook-reading-log/contents"), 1);

        // GitHub asking to wait is kept to: the registry meanwhile, and nothing asked.
        hub.github.limited = true;
        hub.github.reset = hub.time + 600;
        hub.time += PluginFinder::keepSearch + 1;
        found = search("anything");
        QVERIFY(found.value("limited").toBool());
        QVERIFY(found.value("problem").toString().contains("wait"));
        const qsizetype limited = hub.github.asked.size();
        found = search("else");
        QVERIFY(found.value("limited").toBool());
        QCOMPARE(hub.github.asked.size(), limited);
        hub.github.limited = false;
        hub.time += 601;
        QVERIFY(!search("again").value("limited").toBool());
        QVERIFY(hub.github.asked.size() > limited);
    }

    void aListedPluginIsAddedByItsName() {
        Hub hub;
        hub.github.push("jellespijker/omabook-storyteller", "main", sha('a'), Made().packed());
        PluginSources::AddOptions options;
        options.enable = true;
        const QJsonObject added = hub.add("storyteller", options);
        QVERIFY2(!ScriptTools::failed(added), qPrintable(added.value("error").toString()));
        QCOMPARE(added.value("name").toString(), QString("storyteller"));
        QCOMPARE(added.value("repository").toString(), QString("jellespijker/omabook-storyteller"));
        QVERIFY(hub.add("nothing-listed").value("error").toString().contains("No plugin called \"nothing-listed\" is listed"));
        // What is installed says so when it is found.
        PluginFinder finder(hub.context);
        finder.setDiscovery(false);
        const QJsonObject found = PluginSources::wait([&](PluginSources::Done done) { finder.search("story", done); }, 10000);
        QVERIFY(found.value("results").toArray().first().toObject().value("installed").toBool());

        // From the command line and as a tool, the same.
        qputenv("OMABOOK_GITHUB_API", hub.github.api().toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("OMABOOK_GITHUB_API"); });
        const QString folder = hub.dir.filePath("cli-plugins");
        QCOMPARE(runPluginCli({"omabook", "plugin", "search", "story", "--folder", folder}), 0);
        QCOMPARE(runPluginCli({"omabook", "plugin", "search", "--json", "--folder", folder}), 0);
        QCOMPARE(runPluginCli({"omabook", "plugin", "add", "storyteller", "--folder", folder}), 0);
        QVERIFY(QFile::exists(folder + "/storyteller/main.js"));
        const Mcp::Result result = callTool("plugin_search", {{"words", "story"}});
        QVERIFY2(!result.failed(), qPrintable(result.error));
        QCOMPARE(result.data.value("results").toArray().first().toObject().value("name").toString(), QString("storyteller"));
    }

    void theManagerFindsForTheWindow() {
        Hub hub;
        hub.settings.setPluginDiscovery(false);
        Plugins registry(&hub.settings, hub.dir.filePath("plugins"), "-");
        ScriptServices services;
        services.settings = &hub.settings;
        ScriptPlugins scripts(services, hub.dir.filePath("plugins"));
        FakePicker picker;
        QNetworkAccessManager network;
        PluginManager manager(&hub.settings, &registry, &scripts, &picker, &network);
        QVERIFY(!manager.discovery());
        QSignalSpy found(&manager, &PluginManager::foundChanged);
        manager.find("");
        QTRY_VERIFY(!manager.finding());
        QCOMPARE(manager.found().first().toMap().value("name").toString(), QString("storyteller"));
        manager.find("no such words at all");
        QTRY_VERIFY(!manager.finding());
        QVERIFY(manager.found().isEmpty());
        manager.setDiscovery(true);
        QVERIFY(hub.settings.pluginDiscovery());
    }
};

QTEST_MAIN(PluginManagerTests)
#include "pluginmanager_tests.moc"
