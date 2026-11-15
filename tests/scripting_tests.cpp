#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <QtTest>
#include <thread>

#include "ai.h"
#include "backend.h"
#include "booklistmodel.h"
#include "cli.h"
#include "customization.h"
#include "fixtures.h"
#include "formatprovider.h"
#include "formats.h"
#include "importer.h"
#include "library.h"
#include "mcp.h"
#include "reader.h"
#include "scriptapi.h"
#include "scriptform.h"
#include "scriptplugins.h"
#include "scripttools.h"
#include "settings.h"

namespace {

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// A model that answers every question with the same words.
class EchoAi : public AiProvider {
public:
    QString name() const override { return "Echo"; }
    void ask(const AiRequest &request) override {
        const QString prompt = request.messages.last().text;
        QTimer::singleShot(0, this, [this, prompt] {
            emit delta("You asked: ");
            emit delta(prompt);
            emit finished();
        });
    }
    void cancel() override {}
};

// An HTTP server that answers every request with the same body and keeps
// what it was sent.
class FakeSite : public QTcpServer {
public:
    QByteArray body = "{\"answer\": 42}";
    QByteArray received;

    FakeSite() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                received += socket->readAll();
                if (!received.contains("\r\n\r\n"))
                    return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }
    QString url() const { return QString("http://127.0.0.1:%1/books").arg(serverPort()); }
};

// A plugin folder, a library and the settings, all in a temp folder.
struct Bench {
    QTemporaryDir dir;
    Settings settings{dir.filePath("config.json")};
    Library library{dir.filePath("Library")};
    QString folder = dir.filePath("plugins");

    Bench() {
        QDir().mkpath(folder);
        library.open();
    }

    // Writes a plugin: its plugin.json with these permissions, and its script.
    QString plugin(const QString &name, const QByteArray &script, const QStringList &permissions = {}) {
        const QString path = folder + "/" + name;
        QDir().mkpath(path);
        const QJsonObject manifest{{"name", name}, {"version", "1.0"}, {"description", "A test plugin."}, {"script", "main.js"},
                                   {"permissions", QJsonArray::fromStringList(permissions)}};
        Fixtures::write(path + "/plugin.json", QJsonDocument(manifest).toJson());
        Fixtures::write(path + "/main.js", script);
        return path;
    }

    ScriptServices services() {
        ScriptServices services;
        services.settings = &settings;
        services.library = [this] { return &library; };
        services.ai = [](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<EchoAi>(); };
        return services;
    }
    ScriptTools::Context context() {
        ScriptTools::Context context;
        context.folder = folder;
        context.settings = &settings;
        context.library = [this] { return &library; };
        return context;
    }

    qint64 addBook(const QString &title) {
        const QString file = dir.filePath(title + ".epub");
        Fixtures::epub(file, title);
        return library.add(file).id;
    }
    QString error(const QString &name) const {
        QFile file(folder + "/" + name + ".error");
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }
};

// A model that takes its time: the answer comes in pieces, a while apart.
class SlowAi : public AiProvider {
public:
    SlowAi(int pieces, int apart) : m_pieces(pieces) {
        m_timer.setInterval(apart);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            if (m_sent == m_pieces) {
                m_timer.stop();
                emit finished();
                return;
            }
            emit delta(QString("Paragraph %1 of the chapter. ").arg(++m_sent));
        });
    }
    QString name() const override { return "Slow"; }
    void ask(const AiRequest &request) override {
        system = request.system;
        m_timer.start();
    }
    void cancel() override { m_timer.stop(); }
    static inline QString system;

private:
    QTimer m_timer;
    int m_pieces, m_sent = 0;
};

// A plugin that writes a story as a plain text book and adds chapters to it.
const QByteArray teller = R"js(
function story(chapters) {
    var text = "";
    for (var i = 1; i <= chapters; ++i)
        text += "CHAPTER " + i + "\n\nWhat happened in chapter " + i + ", told by " + omabook.plugin.id + ".\n\n"
                + new Array(40).join("And then a good deal more of it, at some length, as stories go.\n\n");
    return text;
}
function make(title, chapters) {
    omabook.files.write("data/story.txt", story(chapters || 1));
    return omabook.library.create({title: title, authors: ["Tess Teller"], tags: ["told"], series: "Tales", series_index: 2}, "data/story.txt");
}
function grow(id, chapters) {
    omabook.files.write("data/story.txt", story(chapters));
    return omabook.library.replaceFile(id, "data/story.txt");
}
function drop(id) { return omabook.library.remove(id); }
function mine() {
    return omabook.library.list().filter(function (book) { return book.generated === omabook.plugin.id; }).map(function (book) { return book.id; });
}
function tryTo(what) { try { what(); return "done"; } catch (error) { return error.message; } }
function trespass(id) {
    omabook.files.write("data/story.txt", story(9));
    return [
        tryTo(function () { omabook.library.replaceFile(id, "data/story.txt"); }),
        tryTo(function () { omabook.library.remove(id); }),
        tryTo(function () { omabook.library.setIdentifier(id, "generated", omabook.plugin.id); })
    ];
}
)js";

// A format for the tests: a book is whatever its parts.json says, each part
// a file given as HTML, Markdown or plain text.
const QByteArray partsFormat = R"js(
omabook.format.register({
    name: "Parts",
    extensions: ["parts"],
    claims: function (source) { return source.contains("parts.json"); },
    open: function (source) {
        var plan = JSON.parse(source.text("parts.json"));
        return {
            title: plan.title, authors: plan.authors, cover: plan.cover, published: 1999,
            chapters: plan.parts.map(function (part) {
                var chapter = {title: part.title, file: part.file, level: part.level || 0};
                chapter[part.kind] = source.text(part.file);
                return chapter;
            })
        };
    }
});
)js";

// The files of one such book: three chapters, one of each kind, a picture
// two of them show, a link from the first to the second, and a cover.
Fixtures::Entries partsBook(const QByteArray &title = "Three Parts") {
    const QJsonObject plan{
        {"title", QString::fromUtf8(title)}, {"authors", QJsonArray{"Pat Parts"}}, {"cover", "img/cover.png"},
        {"parts", QJsonArray{QJsonObject{{"title", "One"}, {"file", "one.html"}, {"kind", "html"}},
                             QJsonObject{{"title", "Two"}, {"file", "sub/two.md"}, {"kind", "markdown"}, {"level", 1}},
                             QJsonObject{{"title", "Three"}, {"file", "three.txt"}, {"kind", "text"}}}}};
    return {{"parts.json", QJsonDocument(plan).toJson()},
            {"one.html", "<p>First words, then <a href=\"sub/two.md\">onward</a>.</p><p><img src=\"img/pic.png\"></p>"},
            {"sub/two.md", "# Second\n\nSome *emphasis* here.\n\n![a picture](../img/pic.png)\n"},
            {"three.txt", "Plain <b>words</b>\nstill the same paragraph.\n\nAnother paragraph.\n"},
            {"img/pic.png", Fixtures::picture(40, 20, Qt::blue)},
            {"img/cover.png", Fixtures::picture(60, 90, Qt::red)}};
}

bool writeFolder(const QString &folder, const Fixtures::Entries &entries) {
    bool ok = true;
    for (const auto &entry : entries) {
        ok = ok && QDir().mkpath(QFileInfo(folder + "/" + entry.first).absolutePath());
        ok = ok && Fixtures::write(folder + "/" + entry.first, entry.second);
    }
    return ok;
}

QStringList texts(const QSignalSpy &spy) {
    QStringList list;
    for (const QList<QVariant> &call : spy)
        list << call.first().toString();
    return list;
}

} // namespace

// Script plugins: what a script can call, what it can't, what happens when
// it goes wrong, and each thing it can add to Omabook, from a metadata
// source to a key. Everything lives in temp folders.
class ScriptingTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_home;

private slots:
    void initTestCase() {
        // Nothing here may read or write the real settings, plugins or library.
        qputenv("HOME", m_home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", m_home.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_home.filePath("cache").toUtf8());
        qputenv("XDG_DATA_HOME", m_home.filePath("data").toUtf8());
        QDir().mkpath(m_home.filePath("config/omabook"));
        Fixtures::write(m_home.filePath("config/omabook/config.json"),
                        QJsonDocument(QJsonObject{{"library", m_home.filePath("Books")}, {"ai_provider", "off"}}).toJson());
    }

    void cleanup() {
        // The commands run here in-process ask for script formats to be
        // loaded on demand; no test should find another's plugins loaded.
        ScriptFormats::unload();
    }

    void theTableIsWhatAScriptSees() {
        // The reference agents are given is made from ScriptApi's table, so
        // the table must be exactly the object: nothing undocumented, and
        // nothing documented that isn't there.
        Bench bench;
        bench.plugin("all", "var loaded = true;", ScriptApi::permissionNames());
        ScriptPlugin all(ScriptManifest::read(bench.folder + "/all"), bench.services());
        QString problem;
        QVERIFY2(all.load(&problem), qPrintable(problem));
        QStringList documented, ungated;
        for (const ScriptApi::Entry &entry : ScriptApi::entries()) {
            documented << entry.name;
            if (entry.permission.isEmpty())
                ungated << entry.name;
            else
                QVERIFY2(ScriptApi::permissionNames().contains(entry.permission), qPrintable(entry.name));
            QVERIFY2(entry.signature.startsWith(entry.name), qPrintable(entry.name));
            QVERIFY2(!entry.what.isEmpty(), qPrintable(entry.name));
        }
        QStringList seen = all.surface();
        seen.sort();
        documented.sort();
        QCOMPARE(seen, documented);

        // Without a permission, what it guards is not there at all.
        bench.plugin("none", "var loaded = true;");
        ScriptPlugin none(ScriptManifest::read(bench.folder + "/none"), bench.services());
        QVERIFY2(none.load(&problem), qPrintable(problem));
        seen = none.surface();
        seen.sort();
        ungated.sort();
        QCOMPARE(seen, ungated);

        // Every event in the table can be listened to, and no other.
        QByteArray script;
        for (const QString &event : ScriptApi::eventNames())
            script += "omabook.on(\"" + event.toUtf8() + "\", function () {});\n";
        bench.plugin("listens", script);
        ScriptPlugin listens(ScriptManifest::read(bench.folder + "/listens"), bench.services());
        QVERIFY2(listens.load(&problem), qPrintable(problem));
        QStringList events = ScriptApi::eventNames();
        events.sort();
        QCOMPARE(listens.events(), events);
        bench.plugin("mishears", "omabook.on(\"book.exploded\", function () {});");
        ScriptPlugin mishears(ScriptManifest::read(bench.folder + "/mishears"), bench.services());
        QVERIFY(!mishears.load(&problem));
        QVERIFY2(problem.startsWith("main.js:1: ") && problem.contains("book.exploded") && problem.contains("book.added"), qPrintable(problem));
    }

    void aScriptReachesNothingItWasNotGiven() {
        Bench bench;
        bench.plugin("plain", R"js(
            function reach() {
                return {
                    http: typeof omabook.http, files: typeof omabook.files, run: typeof omabook.run,
                    host: typeof host, require: typeof require, fetch: typeof fetch, process: typeof process,
                    xhr: typeof XMLHttpRequest, timeout: typeof setTimeout, qt: typeof Qt
                };
            }
            function tamper() {
                try { omabook.http = { get: function () { return "mine"; } }; } catch (error) {}
                try { omabook.library.list = function () { return "mine"; }; } catch (error) {}
                return [typeof omabook.http, typeof omabook.library.list()];
            }
        )js");
        const QJsonObject reached = ScriptTools::run(bench.context(), "plain", "reach").value("result").toObject();
        QCOMPARE(reached.size(), 10);
        for (auto it = reached.begin(); it != reached.end(); ++it)
            QVERIFY2(it.value().toString() == "undefined", qPrintable(it.key() + " is " + it.value().toString()));
        // The object is frozen: a script can't hang something of its own on it.
        QCOMPARE(ScriptTools::run(bench.context(), "plain", "tamper").value("result").toArray(), (QJsonArray{"undefined", "object"}));

        // A permission that doesn't exist is a mistake in plugin.json, said plainly.
        bench.plugin("greedy", "var x = 1;", {"http", "everything"});
        const QJsonObject greedy = ScriptTools::check(bench.context(), "greedy");
        QVERIFY(ScriptTools::failed(greedy));
        QVERIFY2(greedy.value("error").toString().contains("everything") && greedy.value("error").toString().contains("files"),
                 qPrintable(greedy.value("error").toString()));
    }

    void filesStayInsideThePluginsOwnFolder() {
        Bench bench;
        const QString path = bench.plugin("scribe", R"js(
            function tryTo(what) { try { return what(); } catch (error) { return "refused: " + error.message; } }
            function work() {
                return {
                    manifest: omabook.files.read("plugin.json").indexOf("scribe") >= 0,
                    wrote: omabook.files.write("data/notes/today.txt", "read ten pages"),
                    back: omabook.files.read("data/notes/today.txt"),
                    listed: omabook.files.list("data/notes"),
                    missing: omabook.files.read("data/nothing.txt"),
                    exists: omabook.files.exists("main.js"),
                    overScript: tryTo(function () { return omabook.files.write("main.js", "gone"); }),
                    overManifest: tryTo(function () { return omabook.files.write("plugin.json", "{}"); }),
                    climbsOut: tryTo(function () { return omabook.files.write("data/../../escaped.txt", "out"); }),
                    readsOut: tryTo(function () { return omabook.files.read("../../config.json"); }),
                    absolute: tryTo(function () { return omabook.files.read("/etc/hostname"); }),
                    throughLink: tryTo(function () { return omabook.files.write("data/link/x.txt", "out"); }),
                    removedScript: omabook.files.remove("main.js"),
                    removed: omabook.files.remove("data/notes/today.txt")
                };
            }
        )js", {"files"});
        // A link out of the data folder is not a way out of it.
        QVERIFY(QDir().mkpath(path + "/data"));
        QVERIFY(QDir().mkpath(bench.dir.filePath("outside")));
        QVERIFY(QFile::link(bench.dir.filePath("outside"), path + "/data/link"));

        const QJsonObject answer = ScriptTools::run(bench.context(), "scribe", "work");
        QVERIFY2(!ScriptTools::failed(answer), qPrintable(answer.value("error").toString()));
        const QJsonObject did = answer.value("result").toObject();
        QCOMPARE(did.value("manifest").toBool(), true);
        QCOMPARE(did.value("wrote").toBool(), true);
        QCOMPARE(did.value("back").toString(), QString("read ten pages"));
        QCOMPARE(did.value("listed").toArray(), QJsonArray{"today.txt"});
        QVERIFY(did.value("missing").isNull());
        QCOMPARE(did.value("exists").toBool(), true);
        for (const char *refused : {"overScript", "overManifest", "climbsOut", "readsOut", "absolute", "throughLink"})
            QVERIFY2(did.value(refused).toString().startsWith("refused: "), qPrintable(QString(refused) + ": " + QJsonDocument(did).toJson()));
        QCOMPARE(did.value("removedScript").toBool(), false);
        QCOMPARE(did.value("removed").toBool(), true);
        QVERIFY(QFileInfo::exists(path + "/main.js"));
        QVERIFY(!QFileInfo::exists(bench.dir.filePath("escaped.txt")));
        QVERIFY(QDir(bench.dir.filePath("outside")).isEmpty());
    }

    void aBrokenScriptSaysWhereAndTakesNothingDown() {
        Bench bench;
        bench.plugin("shaky", "var fine = 1;\nvar alsoFine = 2;\nvar broken = ;\n");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        // The reason is beside the plugin's folder, with the file and line.
        QVERIFY2(bench.error("shaky").startsWith("main.js:3: SyntaxError"), qPrintable(bench.error("shaky")));
        QVERIFY(!plugins.plugin("shaky"));
        QCOMPARE(plugins.list().size(), 1);
        QVERIFY(plugins.list().first().problem.startsWith("main.js:3"));
        QVERIFY(!plugins.list().first().loaded);

        // Fixed, it loads and the error file goes away.
        bench.plugin("shaky", "omabook.metadata.register({name: \"Shaky\", fields: [\"title\"], search: function () { return []; }});\n");
        plugins.reload();
        QVERIFY(bench.error("shaky").isEmpty());
        QVERIFY(!QFileInfo::exists(bench.folder + "/shaky.error"));
        QVERIFY(plugins.plugin("shaky"));
        QCOMPARE(plugins.list().first().sources, QStringList{"Shaky"});

        // Broken again, the version that worked keeps running, and it says so.
        bench.plugin("shaky", "omabook.metadata.register({name: \"Shaky\"});\n// and a longer file, so it is seen to have changed\n");
        plugins.reload();
        QVERIFY2(bench.error("shaky").contains("main.js:1: TypeError: The source's search should be a function."),
                 qPrintable(bench.error("shaky")));
        QVERIFY(bench.error("shaky").contains("last version that worked"));
        QVERIFY(plugins.plugin("shaky"));
        QCOMPARE(plugins.metadataSources(this).size(), 1);

        // Something thrown later, in a hook, is reported the same way and
        // the plugin goes on.
        bench.plugin("shaky", "omabook.on(\"book.opened\", function (event) {\n    return event.book.title.length;\n});\n"
                              "omabook.on(\"book.closed\", function () { omabook.say(\"still here\"); });\n");
        plugins.reload();
        QVERIFY(bench.error("shaky").isEmpty());
        QSignalSpy said(&plugins, &ScriptPlugins::said);
        plugins.emitEvent("book.opened", {});
        QVERIFY2(bench.error("shaky").startsWith("main.js:2: TypeError"), qPrintable(bench.error("shaky")));
        plugins.emitEvent("book.closed", {});
        QCOMPARE(texts(said), QStringList{"still here"});

        // A plugin.json that can't be read is a problem too, not a crash.
        Fixtures::write(bench.folder + "/shaky/plugin.json", "{\"script\": \"main.js\", ");
        QVERIFY(ScriptTools::failed(ScriptTools::check(bench.context(), "shaky")));
        QVERIFY(ScriptTools::check(bench.context(), "shaky").value("error").toString().contains("plugin.json isn't valid JSON"));
    }

    void aChangedScriptIsLoadedAgainByItself() {
        Bench bench;
        bench.plugin("live", "omabook.commands.register({id: \"one\", title: \"One\", run: function () {}});");
        ScriptPlugins plugins(bench.services(), bench.folder);
        QCOMPARE(plugins.commands().size(), 1);
        QCOMPARE(plugins.commands().first().toMap().value("id").toString(), QString("library.live:one"));
        QSignalSpy changed(&plugins, &ScriptPlugins::changed);

        // Saved again, as an editor or an agent would: no restart, no call.
        bench.plugin("live", "omabook.commands.register({id: \"one\", title: \"One\", run: function () {}});\n"
                             "omabook.commands.register({id: \"two\", title: \"Two\", place: \"reader\", run: function () {}});");
        QTRY_COMPARE(plugins.commands().size(), 2);
        QVERIFY(changed.size() > 0);
        // A plugin dropped into the folder starts; one taken out stops.
        bench.plugin("late", "omabook.on(\"app.started\", function () {});");
        QTRY_VERIFY(plugins.plugin("late"));
        QVERIFY(QDir(bench.folder + "/live").removeRecursively());
        QTRY_VERIFY(!plugins.plugin("live"));
        QCOMPARE(plugins.commands().size(), 0);

        // Switched off in the settings, it stops; switched on, it starts.
        bench.settings.setPluginEnabled("folder:late", false);
        QTRY_VERIFY(!plugins.plugin("late"));
        QCOMPARE(plugins.list().first().enabled, false);
        bench.settings.setPluginEnabled("folder:late", true);
        QTRY_VERIFY(plugins.plugin("late"));
    }

    void aRunawayScriptIsStopped() {
        Bench bench;
        bench.plugin("spinner", R"js(
            function spin() { while (true) {} }
            function fine() { return "still answering"; }
            function later() { return new Promise(function (resolve) { omabook.after(10, resolve); }).then(function () { while (true) {} }); }
            omabook.on("book.opened", function () { for (;;) {} });
        )js");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        plugins.setTimeLimit(std::chrono::milliseconds(150));
        ScriptPlugin *plugin = plugins.plugin("spinner");
        QVERIFY(plugin);

        QElapsedTimer spent;
        spent.start();
        QString problem;
        plugin->callNow("spin", {}, &problem);
        QVERIFY2(problem.contains("without finishing and was stopped"), qPrintable(problem));
        QVERIFY(spent.elapsed() < 3000);
        // It was stopped, not broken: the next call answers.
        problem.clear();
        QCOMPARE(plugin->callNow("fine", {}, &problem).toString(), QString("still answering"));
        QVERIFY(problem.isEmpty());

        // A hook that never comes back is stopped and reported.
        plugins.emitEvent("book.opened", {});
        QVERIFY2(bench.error("spinner").contains("was stopped"), qPrintable(bench.error("spinner")));
        QFile::remove(bench.folder + "/spinner.error");

        // So is a function the script gave a promise, which nobody is
        // waiting on when it runs.
        plugin->callFunction("later", {}, [](const QJsonValue &, const QString &) {});
        QTRY_VERIFY2_WITH_TIMEOUT(bench.error("spinner").contains("was stopped"), qPrintable(bench.error("spinner")), 5000);
        QCOMPARE(plugin->callNow("fine", {}, &problem).toString(), QString("still answering"));

        // And a script whose top level never ends doesn't load.
        bench.plugin("stuck", "while (true) {}");
        const QJsonObject stuck = [&] {
            ScriptPlugin fresh(ScriptManifest::read(bench.folder + "/stuck"), bench.services());
            fresh.setTimeLimit(std::chrono::milliseconds(150));
            QString why;
            const bool loaded = fresh.load(&why);
            return QJsonObject{{"loaded", loaded}, {"why", why}};
        }();
        QCOMPARE(stuck.value("loaded").toBool(), false);
        QVERIFY2(stuck.value("why").toString().contains("was stopped"), qPrintable(stuck.value("why").toString()));
    }

    void aScriptIsASourceOfDetails() {
        Bench bench;
        const qint64 id = bench.addBook("Whale Book");
        // One answers at once, one with a promise, one fails; a hook hears
        // when all have answered.
        bench.plugin("catalogs", R"js(
            omabook.metadata.register({
                name: "At Once", fields: ["title", "series"],
                search: function (book) { return [{title: book.title + " (found)", series: "Sea Tales", series_index: 2}]; }
            });
            omabook.metadata.register({
                name: "In A While", fields: ["title", "authors", "published"],
                search: function (book) {
                    return new Promise(function (resolve) {
                        omabook.after(20, function () { resolve([{title: "Later " + book.title, authors: ["Ann Sailor"], published: 1851}, {}]); });
                    });
                }
            });
            omabook.metadata.register({
                name: "Out Of Order", fields: ["title"],
                search: function () { return Promise.reject(new Error("the catalog is closed")); }
            });
            omabook.on("lookup.finished", function (event) { omabook.say(event.candidates.length + " found for " + event.id); });
        )js");
        NoPicker picker;
        Backend backend(&bench.settings, &picker, nullptr, [](QString *) -> std::unique_ptr<AiProvider> { return nullptr; }, {},
                        bench.folder, "-");
        // Only the script's sources: nothing here goes to the network.
        for (const PluginInfo &plugin : backend.pluginRegistry()->known())
            if (plugin.kind == "metadata" && plugin.switchable)
                bench.settings.setPluginEnabled(plugin.id, false);
        backend.useLibrary(bench.library.root());
        QCOMPARE(backend.libraryPath(), bench.library.root());

        backend.lookUp(id);
        QCOMPARE(backend.lookUpSources().size(), 3);
        QTRY_VERIFY(!backend.searching());
        QStringList titles, sources;
        for (const QVariant &candidate : backend.candidates()) {
            titles << candidate.toMap().value("title").toString();
            sources << candidate.toMap().value("source").toString();
        }
        // A candidate without a title is no candidate.
        QCOMPARE(titles, (QStringList{"Whale Book (found)", "Later Whale Book"}));
        QCOMPARE(sources, (QStringList{"At Once", "In A While"}));
        QVERIFY2(backend.searchProblem().contains("Out Of Order") && backend.searchProblem().contains("the catalog is closed"),
                 qPrintable(backend.searchProblem()));
        QTRY_COMPARE(backend.message(), QString("2 found for %1").arg(id));
        backend.stopLookUp();

        // The plugin is listed with the others, as a script.
        bool listed = false;
        for (const QVariant &plugin : backend.plugins()) {
            if (plugin.toMap().value("id").toString() == "folder:catalogs") {
                listed = true;
                QCOMPARE(plugin.toMap().value("kind").toString(), QString("script"));
                QCOMPARE(plugin.toMap().value("switchable").toBool(), true);
            }
        }
        QVERIFY(listed);
    }

    void aCommandGetsAKeyAndRuns() {
        Bench bench;
        const qint64 id = bench.addBook("Key Book");
        bench.plugin("marks", R"js(
            omabook.commands.register({
                id: "finished", title: "Mark as read", place: "library", key: "Ctrl+R",
                run: function (context) {
                    omabook.library.setReading(context.book, {shelf: "read", rating: 4, finished: "2026-11-14"});
                    omabook.say("Marked " + omabook.library.get(context.book).title + " in the " + context.place);
                }
            });
            // "A" adds books on the shelf, so this one starts without a key.
            omabook.commands.register({id: "taken", title: "Wants A", place: "library", key: "A", run: function () {}});
            omabook.commands.register({id: "anywhere", title: "Everywhere", place: "global", key: "Ctrl+J", run: function () { omabook.say("anywhere"); }});
        )js");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        Customization custom(bench.dir.filePath("custom"));
        custom.setPlugins(plugins.commands(), plugins.panels());

        QCOMPARE(custom.pluginCommands().size(), 3);
        QCOMPARE(custom.chordKeys().value("library.marks:finished").toStringList(), QStringList{"Ctrl+R"});
        QCOMPARE(custom.plainKeys().value("library.marks:taken").toStringList(), QStringList());
        QCOMPARE(custom.chordKeys().value("global.marks:anywhere").toStringList(), QStringList{"Ctrl+J"});
        // The built-in command kept its key.
        QCOMPARE(custom.plainKeys().value("library.add").toStringList(), QStringList{"A"});

        // keys.json binds a plugin's command like any other.
        QVERIFY(QDir().mkpath(custom.folder()));
        QVERIFY(Fixtures::write(custom.folder() + "/keys.json", "{\"library\": {\"F5\": \"marks:taken\", \"Ctrl+R\": \"\"}}"));
        QCOMPARE(custom.check(), QString());
        QCOMPARE(custom.chordKeys().value("library.marks:taken").toStringList(), QStringList{"F5"});
        QCOMPARE(custom.chordKeys().value("library.marks:finished").toStringList(), QStringList());

        QSignalSpy said(&plugins, &ScriptPlugins::said);
        plugins.run("library.marks:finished", id);
        QTRY_COMPARE(texts(said), QStringList{"Marked Key Book in the library"});
        const Book book = *bench.library.book(id);
        QCOMPARE(book.shelf, QString("read"));
        QCOMPARE(book.rating, 4);
        QCOMPARE(book.finished, QString("2026-11-14"));

        // A command that throws is reported, with what it was given.
        plugins.run("library.marks:finished", 999);
        QVERIFY2(bench.error("marks").contains("There is no book 999"), qPrintable(bench.error("marks")));
    }

    void hooksHearWhatHappensToTheLibrary() {
        Bench bench;
        bench.plugin("ears", R"js(
            omabook.on("app.started", function () { omabook.say("started"); });
            omabook.on("book.added", function (event) { omabook.say("added " + event.book.title); });
            omabook.on("book.changed", function (event) { omabook.say("changed " + event.before.title + " to " + event.book.title); });
            omabook.on("book.removed", function (event) { omabook.say("removed " + event.book.title + " (" + event.id + ")"); });
            omabook.on("library.switched", function (event) { omabook.say("switched to " + event.name); });
            omabook.on("book.opened", function (event) { omabook.say("opened " + event.book.title + ", reading " + omabook.reader.current().id); });
            omabook.on("book.closed", function (event) { omabook.say("closed " + event.id + ", reading " + omabook.reader.current()); });
        )js");
        bench.settings.setLibraryPath(bench.library.root());
        NoPicker picker;
        Backend backend(&bench.settings, &picker, nullptr, [](QString *) -> std::unique_ptr<AiProvider> { return nullptr; }, {},
                        bench.folder, "-");
        QSignalSpy said(backend.scripts(), &ScriptPlugins::said);
        QTRY_COMPARE(texts(said), QStringList{"started"});
        said.clear();

        // Added behind the window's back, as the command line would.
        const qint64 id = bench.addBook("First Book");
        backend.reload();
        QTRY_COMPARE(texts(said), QStringList{"added First Book"});
        said.clear();

        BookMeta meta = bench.library.book(id)->meta;
        meta.title = "Renamed Book";
        QVERIFY(bench.library.setMeta(id, meta));
        backend.reload();
        QCOMPARE(texts(said), QStringList{"changed First Book to Renamed Book"});
        said.clear();

        // Starting a book changes how it stands; reading on changes nothing.
        bench.library.setProgress(id, {0, 0.5, 20});
        backend.reload();
        QCOMPARE(texts(said), QStringList{"changed Renamed Book to Renamed Book"});
        said.clear();
        bench.library.setProgress(id, {1, 0.5, 60});
        backend.reload();
        QCOMPARE(texts(said), QStringList());

        Reader reader(&backend, {});
        reader.openBook(id);
        QTRY_COMPARE(texts(said), QStringList{QString("opened Renamed Book, reading %1").arg(id)});
        said.clear();
        reader.close();
        QCOMPARE(texts(said), QStringList{QString("closed %1, reading null").arg(id)});
        said.clear();

        backend.remove(id);
        QCOMPARE(texts(said), QStringList{QString("removed Renamed Book (%1)").arg(id)});
        said.clear();

        QVERIFY(QDir().mkpath(bench.dir.filePath("Other")));
        backend.useLibrary(bench.dir.filePath("Other"));
        QCOMPARE(texts(said), QStringList{"switched to Other"});
        // What a script says is said in the window.
        QCOMPARE(backend.message(), QString("switched to Other"));
    }

    void aBookAddedWhileAwayIsStillAnnounced() {
        Bench bench;
        bench.plugin("ears", "omabook.on(\"book.added\", function (event) { omabook.say(\"added \" + event.book.title); });");
        bench.addBook("Already Here");
        {
            ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
            QSignalSpy said(&plugins, &ScriptPlugins::said);
            // The first look at a library announces nothing: it is how things are.
            plugins.libraryChanged(bench.library.books(), bench.library.root());
            QCOMPARE(texts(said), QStringList());
        }
        // Omabook isn't running; a book comes in from the command line.
        bench.addBook("Came In Later");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        QSignalSpy said(&plugins, &ScriptPlugins::said);
        plugins.libraryChanged(bench.library.books(), bench.library.root());
        QTRY_COMPARE(texts(said), QStringList{"added Came In Later"});
        // Once.
        plugins.libraryChanged(bench.library.books(), bench.library.root());
        QVERIFY(plugins.wait(2000));
        QCOMPARE(said.size(), 1);
    }

    void anImporterHasItsSayInEveryNewBook() {
        Bench bench;
        bench.plugin("tagger", R"js(
            omabook.importer.register({
                name: "Inbox",
                added: function (book) { return {tags: book.tags.concat(["inbox"]), publisher: "Seen by " + omabook.plugin.name}; }
            });
            omabook.on("book.added", function (event) { omabook.say("added"); });
        )js");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        QSignalSpy touched(&plugins, &ScriptPlugins::touchedLibrary);
        plugins.libraryChanged(bench.library.books(), bench.library.root());
        const qint64 id = bench.addBook("Fresh Book");
        plugins.libraryChanged(bench.library.books(), bench.library.root());
        QTRY_COMPARE(touched.size(), 1);
        const Book book = *bench.library.book(id);
        QVERIFY2(book.meta.tags.contains("inbox"), qPrintable(book.meta.tags.join(',')));
        QCOMPARE(book.meta.publisher, QString("Seen by tagger"));
        // What the file said of itself is kept.
        QCOMPARE(book.meta.title, QString("Fresh Book"));
    }

    void aScriptWritesACatalogAndAddsAPanel() {
        Bench bench;
        bench.addBook("Alpha");
        bench.addBook("Beta");
        const QString path = bench.plugin("extras", R"js(
            omabook.catalog.register({
                format: "titles", description: "One title a line.",
                make: function (books) { return books.map(function (book) { return book.title; }).sort().join("\n") + "\n"; }
            });
            omabook.ui.panel({place: "reader.side", title: "Extras", width: 2000, source: "Panel.qml"});
            omabook.ui.set("count", omabook.library.list().length);
            function double(n) { omabook.ui.set("count", n * 2); return n * 2; }
        )js");
        Fixtures::write(path + "/Panel.qml", "import QtQuick\nItem {}\n");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);

        QCOMPARE(plugins.catalogFormats(), QStringList{"titles"});
        QCOMPARE(plugins.describeCatalog("titles"), QString("One title a line."));
        QString problem;
        QCOMPARE(plugins.makeCatalog("titles", bench.library.books(), &problem), QByteArray("Alpha\nBeta\n"));
        QVERIFY(problem.isEmpty());
        QVERIFY(plugins.makeCatalog("nothing", bench.library.books(), &problem).isNull());
        QVERIFY(!problem.isEmpty());

        // The panel is a mod like the user's own, so the same slot shows it.
        Customization custom(bench.dir.filePath("custom"));
        QSignalSpy mods(&custom, &Customization::modsChanged);
        custom.setPlugins(plugins.commands(), plugins.panels());
        QCOMPARE(mods.size(), 1);
        QCOMPARE(custom.mods().size(), 1);
        const QVariantMap panel = custom.mods().first().toMap();
        QCOMPARE(panel.value("name").toString(), QString("extras"));
        QCOMPARE(panel.value("place").toString(), QString("reader.side"));
        QCOMPARE(panel.value("width").toInt(), 900);
        QCOMPARE(panel.value("url").toUrl().toLocalFile(), path + "/Panel.qml");
        // What the script set is there for the panel, and the panel can call back.
        QCOMPARE(plugins.data().value("extras").toMap().value("count").toInt(), 2);
        QSignalSpy data(&plugins, &ScriptPlugins::dataChanged);
        QCOMPARE(plugins.call("extras", "double", {21}).toInt(), 42);
        QCOMPARE(data.size(), 1);
        QCOMPARE(plugins.data().value("extras").toMap().value("count").toInt(), 42);

        // A panel that isn't there is the script's mistake, said at once.
        bench.plugin("lost", "omabook.ui.panel({source: \"Missing.qml\"});");
        QVERIFY(ScriptTools::check(bench.context(), "lost").value("error").toString().contains("Missing.qml"));
    }

    void theLibraryIsThereToReadAndChange() {
        Bench bench;
        const qint64 id = bench.addBook("Sea Stories");
        bench.plugin("librarian", R"js(
            function work(id) {
                var before = omabook.library.get(id);
                var after = omabook.library.setMeta(id, {series: "Tides", series_index: 3, tags: ["sea", "salt"]});
                omabook.library.setIdentifier(id, "shelfmark", "S-12");
                omabook.library.setNotes(id, "librarian", "Shelved by the window.");
                var paper = omabook.library.addEntry({title: "A Paper Book", authors: ["Pat Page"]}, true);
                var refused;
                try { omabook.library.setMeta(id, {title: ""}); } catch (error) { refused = error.message; }
                return {
                    title: before.title, files: Object.keys(before.files), kept: after.title, series: after.series,
                    index: after.series_index, tags: after.tags, found: omabook.library.search("tag:salt").length,
                    all: omabook.library.list().length, paper: omabook.library.get(paper).held,
                    notes: omabook.library.notes(id, "librarian").text, missing: omabook.library.get(4711),
                    sections: omabook.library.toc(id).sections, text: omabook.library.text(id, 0).length > 0,
                    progress: omabook.library.progress(id).percent, refused: refused,
                    option: omabook.settings.option("font_size"), set: omabook.settings.setOption("font_size", 24),
                    unfit: omabook.settings.setOption("font_size", 4000)
                };
            }
        )js");
        const QJsonObject answer = ScriptTools::run(bench.context(), "librarian", "work", QJsonArray{id});
        QVERIFY2(!ScriptTools::failed(answer), qPrintable(answer.value("error").toString()));
        const QJsonObject did = answer.value("result").toObject();
        QCOMPARE(did.value("title").toString(), QString("Sea Stories"));
        QCOMPARE(did.value("files").toArray(), QJsonArray{"epub"});
        QCOMPARE(did.value("kept").toString(), QString("Sea Stories"));
        QCOMPARE(did.value("series").toString(), QString("Tides"));
        QCOMPARE(did.value("index").toDouble(), 3.0);
        QCOMPARE(did.value("tags").toArray(), (QJsonArray{"sea", "salt"}));
        QCOMPARE(did.value("found").toInt(), 1);
        QCOMPARE(did.value("all").toInt(), 2);
        QCOMPARE(did.value("paper").toString(), QString("paper"));
        QCOMPARE(did.value("notes").toString(), QString("Shelved by the window."));
        QVERIFY(did.value("missing").isNull());
        QCOMPARE(did.value("sections").toInt(), 2);
        QCOMPARE(did.value("text").toBool(), true);
        QCOMPARE(did.value("refused").toString(), QString("A book needs a title."));
        QCOMPARE(did.value("option").toInt(), 19);
        QCOMPARE(did.value("set").toBool(), true);
        QCOMPARE(did.value("unfit").toBool(), false);
        QCOMPARE(bench.settings.fontSize(), 24);
        // It is the library itself that changed.
        const Book book = *bench.library.book(id);
        QCOMPARE(book.meta.series, QString("Tides"));
        QCOMPARE(book.identifiers.value("shelfmark").toString(), QString("S-12"));
    }

    void theNetworkProgramsAndTheModelAnswerInPromises() {
        Bench bench;
        FakeSite site;
        bench.plugin("reaches", R"js(
            function fetch(url) {
                return omabook.http.get(url, {headers: {"X-Shelf": "seven"}}).then(function (response) {
                    return {status: response.status, ok: response.ok, answer: response.json().answer, type: response.headers["content-type"]};
                });
            }
            function send(url) { return omabook.http.post(url, {title: "Posted"}).then(function (response) { return response.status; }); }
            // (Put together from pieces: moc reads two slashes in a raw string as a comment.)
            function file() { return omabook.http.get(["file:", "", "", "etc", "hostname"].join("/")).then(function () { return "fetched"; }, function (why) { return "refused: " + why; }); }
            function echo() { return omabook.run("sh", ["-c", "cat; echo err >&2; exit 3"], {input: "from the script"}); }
            function nothing() { return omabook.run("omabook-no-such-program", []).then(function () { return "ran"; }, function (why) { return "failed: " + why; }); }
            function ask() { return omabook.ai.ask("Who is the narrator?").then(function (answer) { return answer + (omabook.ai.available() ? "!" : "?"); }); }
            function kept() {
                omabook.settings.set("seen", (omabook.settings.get("seen", 0)) + 1);
                return [omabook.settings.get("seen"), omabook.settings.get("unseen", "fallback"), Object.keys(omabook.settings.all())];
            }
            function split() { omabook.include("more.js"); return more(); }
        )js", {"http", "run"});
        Fixtures::write(bench.folder + "/reaches/more.js", "function more() { return \"from another file\"; }\n");

        QJsonObject answer = ScriptTools::run(bench.context(), "reaches", "fetch", QJsonArray{site.url()});
        QVERIFY2(!ScriptTools::failed(answer), qPrintable(answer.value("error").toString()));
        QCOMPARE(answer.value("result").toObject(), (QJsonObject{{"status", 200}, {"ok", true}, {"answer", 42}, {"type", "application/json"}}));
        QVERIFY(site.received.contains("X-Shelf: seven"));
        // Every plugin is the same client, and nothing about the reader goes along.
        QVERIFY2(site.received.contains("User-Agent: omabook-plugin/"), site.received.constData());

        site.received.clear();
        answer = ScriptTools::run(bench.context(), "reaches", "send", QJsonArray{site.url()});
        QCOMPARE(answer.value("result").toInt(), 200);
        QVERIFY(site.received.startsWith("POST /books"));
        QVERIFY(site.received.toLower().contains("content-type: application/json"));
        QVERIFY(site.received.endsWith("{\"title\":\"Posted\"}"));

        // http is for the web: a file is not an address.
        QVERIFY(ScriptTools::run(bench.context(), "reaches", "file").value("result").toString().startsWith("refused: "));

        answer = ScriptTools::run(bench.context(), "reaches", "echo");
        QCOMPARE(answer.value("result").toObject(), (QJsonObject{{"code", 3}, {"stdout", "from the script"}, {"stderr", "err\n"}}));
        QVERIFY(ScriptTools::run(bench.context(), "reaches", "nothing").value("result").toString().startsWith("failed: omabook-no-such-program"));

        // The model is whichever the settings choose; here the fake one...
        ScriptPlugin plugin(ScriptManifest::read(bench.folder + "/reaches"), bench.services());
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        QString answered;
        plugin.callFunction("ask", {}, [&](const QJsonValue &value, const QString &) { answered = value.toString(); });
        QTRY_COMPARE(answered, QString("You asked: Who is the narrator?!"));
        // ...and with none set up, the promise is rejected with the reason.
        ScriptServices none = bench.services();
        none.ai = [](QString *why) -> std::unique_ptr<AiProvider> {
            if (why)
                *why = "AI is switched off in the settings.";
            return nullptr;
        };
        ScriptPlugin quiet(ScriptManifest::read(bench.folder + "/reaches"), none);
        QVERIFY(quiet.load(&problem));
        quiet.callFunction("ask", {}, [&](const QJsonValue &, const QString &why) { problem = why; });
        QTRY_COMPARE(problem, QString("AI is switched off in the settings."));

        // A plugin's own settings outlast the engine they were set in.
        QCOMPARE(ScriptTools::run(bench.context(), "reaches", "kept").value("result").toArray(), (QJsonArray{1, "fallback", QJsonArray{"seen"}}));
        QCOMPARE(ScriptTools::run(bench.context(), "reaches", "kept").value("result").toArray().at(0).toInt(), 2);
        QVERIFY(QFileInfo::exists(bench.folder + "/reaches/data/settings.json"));
        QCOMPARE(ScriptTools::run(bench.context(), "reaches", "split").value("result").toString(), QString("from another file"));
    }

    void everyTemplateIsAPluginThatWorks() {
        Bench bench;
        const qint64 id = bench.addBook("Template Book");
        QCOMPARE(ScriptApi::templates(), (QStringList{"catalog", "command", "format", "hook", "metadata", "panel"}));
        for (const QString &name : ScriptApi::templates()) {
            const QJsonObject made = ScriptTools::create(bench.context(), "my-" + name, name);
            QVERIFY2(!ScriptTools::failed(made), qPrintable(made.value("error").toString()));
            const QJsonObject checked = ScriptTools::check(bench.context(), "my-" + name);
            QVERIFY2(checked.value("ok").toBool(), qPrintable(name + ": " + QJsonDocument(checked).toJson()));
            // Its own name is filled in wherever the template had a blank.
            for (const QJsonValue &file : made.value("files").toArray()) {
                QFile written(made.value("folder").toString() + "/" + file.toString());
                QVERIFY(written.open(QIODevice::ReadOnly));
                QVERIFY2(!written.readAll().contains("{{"), qPrintable(name + "/" + file.toString()));
            }
        }
        // No plugin is made over another, and none from a template there isn't.
        QVERIFY(ScriptTools::failed(ScriptTools::create(bench.context(), "my-hook", "hook")));
        QVERIFY(ScriptTools::failed(ScriptTools::create(bench.context(), "other", "sonnet")));
        QVERIFY(ScriptTools::failed(ScriptTools::create(bench.context(), "../escape", "hook")));

        // Each does what its comments say.
        QJsonObject registered = ScriptTools::check(bench.context(), "my-metadata").value("registered").toObject();
        QCOMPARE(registered.value("metadata").toArray().first().toObject().value("name").toString(), QString("Gutenberg"));
        const QJsonObject found{{"title", "Moby Dick"}, {"authors", QJsonArray{QJsonObject{{"name", "Melville, Herman"}}}},
                                {"languages", QJsonArray{"en"}}, {"formats", QJsonObject{{"image/jpeg", "https://example.org/cover.jpg"}}}};
        const QJsonObject candidate = ScriptTools::run(bench.context(), "my-metadata", "candidate", QJsonArray{found}).value("result").toObject();
        QCOMPARE(candidate.value("authors").toArray(), QJsonArray{"Herman Melville"});
        QCOMPARE(candidate.value("cover_url").toString(), QString("https://example.org/cover.jpg"));

        QJsonObject ran = ScriptTools::run(bench.context(), "my-command", "command:finished", QJsonObject{{"book", id}});
        QCOMPARE(ran.value("said").toArray(), QJsonArray{"Marked Template Book as read."});
        QCOMPARE(bench.library.book(id)->shelf, QString("read"));
        ran = ScriptTools::run(bench.context(), "my-command", "command:finished");
        QCOMPARE(ran.value("said").toArray(), QJsonArray{"Select a book first."});

        ran = ScriptTools::run(bench.context(), "my-hook", "event:book.closed", QJsonObject{{"id", id}, {"percent", 100}});
        QVERIFY2(!ScriptTools::failed(ran), qPrintable(ran.value("error").toString()));
        QCOMPARE(ran.value("said").toArray(), QJsonArray{"Finished Template Book. That makes 1."});
        QCOMPARE(ScriptTools::run(bench.context(), "my-hook", "finished").value("result").toArray().size(), 1);
        // Closing it again doesn't count it twice.
        QCOMPARE(ScriptTools::run(bench.context(), "my-hook", "event:book.closed", QJsonObject{{"id", id}, {"percent", 100}}).value("said").toArray().size(), 0);

        ran = ScriptTools::run(bench.context(), "my-catalog", "catalog:reading-list");
        QVERIFY2(ran.value("result").toString().contains("- [x] Template Book"), qPrintable(QJsonDocument(ran).toJson()));

        // The format template reads a folder of text chapters with an index,
        // through the script alone and through Omabook's own way of opening books.
        QVERIFY(writeFolder(bench.dir.filePath("The Long Way"),
                            {{"index.txt", "title: The Long Way\nauthor: Ann Author\nlanguage: en\n\n01-start.txt   Setting Out\n02-road.txt On the Road\nmissing.txt Not There\n"},
                             {"01-start.txt", "We left at dawn.\n\nNobody waved.\n"},
                             {"02-road.txt", "The road went on.\n"}}));
        registered = ScriptTools::check(bench.context(), "my-format").value("registered").toObject();
        QCOMPARE(registered.value("formats").toArray().first().toObject().value("name").toString(), QString("Text book"));
        ran = ScriptTools::run(bench.context(), "my-format", "format:Text book", bench.dir.filePath("The Long Way"));
        QVERIFY2(!ScriptTools::failed(ran), qPrintable(ran.value("error").toString()));
        QCOMPARE(ran.value("result").toObject().value("title").toString(), QString("The Long Way"));
        QCOMPARE(ran.value("result").toObject().value("chapters").toArray().size(), 2);
        {
            ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
            plugins.provideFormats();
            const auto book = Formats::open(bench.dir.filePath("The Long Way"));
            QVERIFY(book);
            QCOMPARE(book->meta().title, QString("The Long Way"));
            QCOMPARE(book->meta().authors, QStringList{"Ann Author"});
            QCOMPARE(book->count(), 2);
            QCOMPARE(book->toc().at(1).title, QString("On the Road"));
            QVERIFY(book->text(0).contains("Nobody waved."));
            // A folder without an index is not one of its books, and says why when forced.
            QVERIFY(QDir().mkpath(bench.dir.filePath("Loose")));
            QVERIFY(Fixtures::write(bench.dir.filePath("Loose/a.txt"), "words"));
            QVERIFY(!Formats::providerFor(bench.dir.filePath("Loose")));
            QVERIFY(ScriptTools::run(bench.context(), "my-format", "format:Text book", bench.dir.filePath("Loose"))
                        .value("error").toString().contains("There is no index.txt in Loose."));
        }

        registered = ScriptTools::check(bench.context(), "my-panel").value("registered").toObject();
        QCOMPARE(registered.value("panels").toArray().first().toObject().value("source").toString(), QString("Panel.qml"));
        QCOMPARE(registered.value("events").toArray().size(), 5);
    }

    void theCommandLineAndTheToolsDriveAPlugin() {
        Bench bench;
        const QStringList at{"--folder", bench.folder};
        auto omabook = [&](QStringList words) { return runCli(QStringList{"omabook", "plugin"} + words + at); };
        QVERIFY(isCliCommand("plugin"));
        QVERIFY(cliSummary().contains("plugin new|check|run|list|api"));

        QCOMPARE(omabook({"new", "streak", "--template", "command"}), 0);
        QVERIFY(QFileInfo::exists(bench.folder + "/streak/main.js"));
        QCOMPARE(omabook({"new", "streak"}), 1);
        QCOMPARE(omabook({"new", "Bad Name"}), 1);
        QCOMPARE(omabook({"check", "streak"}), 0);
        QCOMPARE(omabook({"check", "streak", "--json"}), 0);
        QCOMPARE(omabook({"check", "nothing-there"}), 1);
        QCOMPARE(omabook({"list"}), 0);
        QCOMPARE(omabook({"list", "--json"}), 0);
        QCOMPARE(omabook({"api"}), 0);
        QCOMPARE(omabook({"api", "--json"}), 0);
        QCOMPARE(omabook({"run", "streak", "today"}), 0);
        QCOMPARE(omabook({"run", "streak", "no_such_function"}), 1);
        QCOMPARE(omabook({"run", "streak", "today", "{not json"}), 1);
        QCOMPARE(omabook({"run", "streak"}), 1);
        QCOMPARE(omabook({"frobnicate"}), 1);
        QCOMPARE(omabook({}), 1);
        QCOMPARE(omabook({"--help"}), 0);

        // Switching is the settings' business: the user's own, which here is
        // the scratch home the tests run in.
        QCOMPARE(omabook({"disable", "streak"}), 0);
        QCOMPARE(Settings().pluginEnabled("folder:streak", true), false);
        QCOMPARE(omabook({"enable", "streak"}), 0);
        QCOMPARE(Settings().pluginEnabled("folder:streak", false), true);
        QCOMPARE(omabook({"enable", "nothing-there"}), 1);

        // A mistake comes back with its file and line, and fails the command.
        Fixtures::write(bench.folder + "/streak/main.js", "var a = 1;\n\nomabook.library.fetchEverything();\n");
        QCOMPARE(omabook({"check", "streak"}), 1);
        const QJsonObject checked = ScriptTools::check(bench.context(), "streak");
        QCOMPARE(checked.value("ok").toBool(), false);
        const QJsonObject error = checked.value("errors").toArray().first().toObject();
        QCOMPARE(error.value("file").toString(), QString("main.js"));
        QCOMPARE(error.value("line").toInt(), 3);
        QVERIFY2(error.value("message").toString().contains("fetchEverything"), qPrintable(error.value("message").toString()));

        // Writing a plugin file by file, as an agent does over MCP: the
        // script alone isn't a plugin yet; with plugin.json it is checked.
        QJsonObject wrote = ScriptTools::write(bench.context(), "by-hand", "main.js", "omabook.log(\"loaded\", {n: 1});\nfunction twice(n) { return n * 2; }\n");
        QVERIFY2(!ScriptTools::failed(wrote), qPrintable(wrote.value("error").toString()));
        QVERIFY(!wrote.contains("check"));
        wrote = ScriptTools::write(bench.context(), "by-hand", "plugin.json", "{\"name\": \"By hand\", \"script\": \"main.js\"}");
        QCOMPARE(wrote.value("check").toObject().value("ok").toBool(), true);
        QCOMPARE(wrote.value("check").toObject().value("log").toArray(), QJsonArray{"loaded {\"n\":1}"});
        QCOMPARE(ScriptTools::run(bench.context(), "by-hand", "twice", QJsonArray{21}).value("result").toInt(), 42);
        QCOMPARE(ScriptTools::run(bench.context(), "by-hand", "twice", 4).value("result").toInt(), 8);
        QVERIFY(ScriptTools::failed(ScriptTools::write(bench.context(), "by-hand", "../other/main.js", "x")));
        QVERIFY(ScriptTools::failed(ScriptTools::write(bench.context(), "by-hand", "", "x")));
        QVERIFY(ScriptTools::failed(ScriptTools::run(bench.context(), "by-hand", "event:book.exploded")));

        const QJsonArray listed = ScriptTools::list(bench.context()).value("plugins").toArray();
        QCOMPARE(listed.size(), 2);
        QCOMPARE(listed.at(0).toObject().value("name").toString(), QString("by-hand"));
        QCOMPARE(listed.at(0).toObject().value("running").toBool(), true);
        QCOMPARE(listed.at(1).toObject().value("running").toBool(), false);
        QVERIFY(listed.at(1).toObject().value("problem").toString().startsWith("main.js:3"));

        // The same six are tools of the MCP server.
        QStringList tools;
        for (const Mcp::Tool &tool : Mcp::tools())
            if (tool.name.startsWith("plugin_"))
                tools << tool.name;
        for (const char *tool : {"plugin_api", "plugin_list", "plugin_write", "plugin_check", "plugin_run", "plugin_enable"})
            QVERIFY2(tools.contains(tool), tool);
    }

    void theToolsWorkOverTheProtocol() {
        // The tools work on the user's plugin folder, which here is the
        // scratch home: written, checked, run and switched by name.
        auto call = [](const QString &tool, const QJsonObject &arguments) {
            const QJsonObject request{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                      {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}};
            return QJsonDocument::fromJson(Mcp::handle(QJsonDocument(request).toJson(QJsonDocument::Compact))).object().value("result").toObject();
        };
        auto text = [](const QJsonObject &result) { return result.value("content").toArray().first().toObject().value("text").toString(); };

        QJsonObject result = call("plugin_api", {});
        QVERIFY2(!result.value("isError").toBool(), qPrintable(text(result)));
        QVERIFY(text(result).contains("omabook.metadata.register"));

        result = call("plugin_write", {{"name", "over-mcp"}, {"template", "catalog"}});
        QVERIFY2(!result.value("isError").toBool(), qPrintable(text(result)));
        QVERIFY(QFileInfo::exists(ScriptPlugins::defaultFolder() + "/over-mcp/main.js"));
        QVERIFY(ScriptPlugins::defaultFolder().startsWith(m_home.path()));
        result = call("plugin_write", {{"name", "over-mcp"}, {"file", "main.js"}, {"contents", "function hello(name) { return \"hello \" + name; }\nvar oops = nope;\n"}});
        QVERIFY2(text(result).contains("main.js") && text(result).contains("nope"), qPrintable(text(result)));
        // A check that finds mistakes has answered: the mistakes are its answer.
        result = call("plugin_check", {{"name", "over-mcp"}});
        QVERIFY(!result.value("isError").toBool());
        QVERIFY2(text(result).contains("\"line\": 2") || text(result).contains("\"line\":2"), qPrintable(text(result)));
        result = call("plugin_write", {{"name", "over-mcp"}, {"file", "main.js"}, {"contents", "function hello(name) { return \"hello \" + name; }\n"}});
        result = call("plugin_run", {{"name", "over-mcp"}, {"what", "hello"}, {"arguments", QJsonArray{"agent"}}});
        QVERIFY2(text(result).contains("hello agent"), qPrintable(text(result)));
        result = call("plugin_list", {});
        QVERIFY2(text(result).contains("over-mcp"), qPrintable(text(result)));
        result = call("plugin_enable", {{"name", "over-mcp"}, {"enabled", false}});
        QVERIFY2(!result.value("isError").toBool(), qPrintable(text(result)));
        QCOMPARE(Settings().pluginEnabled("folder:over-mcp", true), false);
        result = call("plugin_run", {{"name", "nothing-there"}, {"what", "hello"}});
        QVERIFY(result.value("isError").toBool());
    }

    void aScriptAddsABookFormat() {
        Bench bench;
        bench.plugin("parts", partsFormat);
        const QString folder = bench.dir.filePath("Three Parts"), zipped = bench.dir.filePath("zipped.zip"),
                      file = bench.dir.filePath("Filed.parts");
        QVERIFY(writeFolder(folder, partsBook()));
        QVERIFY(Fixtures::zip(zipped, partsBook("Zipped Parts")));
        QVERIFY(Fixtures::zip(file, partsBook("Filed Parts")));
        QVERIFY(!Formats::isBook(file));
        QVERIFY(!Formats::providerFor(folder));

        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        // A set of plugins keeps its formats to itself until it is the one
        // the program runs with.
        QVERIFY(!Formats::isBook(file));
        plugins.provideFormats();
        QVERIFY(Formats::isBook(file));
        QVERIFY(Formats::extensions().contains("parts"));
        QCOMPARE(Formats::name("parts"), QString("Parts"));
        QCOMPARE(plugins.list().first().formats, QStringList{"Parts (.parts)"});
        // Its own extension by name; a folder and a plain zip by what is in them.
        QCOMPARE(Formats::providerFor(file)->id, QString("plugin:parts/Parts"));
        QCOMPARE(Formats::providerFor(folder)->id, QString("plugin:parts/Parts"));
        QCOMPARE(Formats::providerFor(zipped)->id, QString("plugin:parts/Parts"));
        QVERIFY(Fixtures::zip(bench.dir.filePath("other.zip"), {{"notes.txt", "Not a book of parts."}}));
        QVERIFY(!Formats::providerFor(bench.dir.filePath("other.zip")));

        // The same book from a folder, a zip and a file of its own.
        for (const auto &[path, title] : {std::pair{folder, "Three Parts"}, std::pair{zipped, "Zipped Parts"}, std::pair{file, "Filed Parts"}}) {
            QString error;
            const auto book = Formats::open(path, &error);
            QVERIFY2(book, qPrintable(path + ": " + error));
            QCOMPARE(book->kind(), Document::Flow);
            QCOMPARE(book->count(), 3);
            const BookMeta meta = book->meta();
            QCOMPARE(meta.title, QString(title));
            QCOMPARE(meta.authors, QStringList{"Pat Parts"});
            QCOMPARE(meta.published, QString("1999"));
            QCOMPARE(meta.cover.size(), QSize(60, 90));
            QCOMPARE(book->toc().size(), 3);
            QCOMPARE(book->toc().at(1).title, QString("Two"));
            QCOMPARE(book->toc().at(1).level, 1);

            // HTML as given: its link to another chapter's file leads to
            // that chapter, and its picture is found among the book's files.
            const QString first = book->html(0, 600);
            QVERIFY2(first.contains("First words") && first.contains("omabook:1"), qPrintable(first));
            QVERIFY2(first.contains("image://book/"), qPrintable(first));
            QCOMPARE(QImage::fromData(book->resource("img/pic.png")).size(), QSize(40, 20));
            QVERIFY(book->resource("../" + QFileInfo(path).fileName()).isNull());
            // Markdown is made into HTML by Omabook, pictures relative to its own file.
            const QString second = book->html(1, 600);
            QVERIFY2(second.contains("emphasis") && !second.contains("*emphasis*"), qPrintable(second));
            QVERIFY2(second.contains("image://book/"), qPrintable(second));
            QVERIFY(book->text(1).contains("Some emphasis here."));
            // Plain text is paragraphs, and what looks like markup in it is words.
            QVERIFY2(book->text(2).contains("Plain <b>words</b>"), qPrintable(book->text(2)));
            QVERIFY(book->text(2).contains("Another paragraph."));
            QVERIFY2(!book->html(2, 600).contains("<b>words</b>"), qPrintable(book->html(2, 600)));
        }
        QCOMPARE(Formats::readMeta(file).title, QString("Filed Parts"));

        // A folder or a zip the format claims goes into a library as one
        // book, packed under the format's own extension.
        ImportSummary summary = importBooks(bench.library, {folder});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.added, 1);
        const QList<Book> books = bench.library.books("title:Three");
        QCOMPARE(books.size(), 1);
        QCOMPARE(books.first().formats, QStringList{"parts"});
        QCOMPARE(books.first().meta.authors, QStringList{"Pat Parts"});
        QVERIFY(books.first().hasCover);
        const auto kept = Formats::open(bench.library.formatPath(books.first().id));
        QVERIFY(kept);
        QCOMPARE(kept->count(), 3);
        QVERIFY(QFileInfo::exists(folder + "/sub/two.md"));
        QCOMPARE(importBooks(bench.library, {folder}).duplicates, 1);
        QCOMPARE(importBooks(bench.library, {zipped, file}).added, 2);
        // And a script reads its text like any other book's.
        bench.plugin("reads", "function words(id) { return omabook.library.text(id, 2) + \" / \" + omabook.library.toc(id).sections; }");
        const QJsonObject read = ScriptTools::run(bench.context(), "reads", "words", QJsonArray{books.first().id});
        QVERIFY2(read.value("result").toString().contains("Another paragraph.") && read.value("result").toString().endsWith("/ 3"),
                 qPrintable(QJsonDocument(read).toJson()));
    }

    void aSourceIsOneBookAndOnlyWhileItIsOpen() {
        Bench bench;
        bench.plugin("peeks", R"js(
            var kept = null;
            omabook.format.register({
                name: "Peek", extensions: ["peek"],
                open: function (source) {
                    kept = source;
                    var seen = {
                        name: source.name, folder: source.isFolder, entries: source.entries(),
                        has: [source.contains("a.txt"), source.contains("b.txt")],
                        outside: source.text("../secret.txt"), absolute: source.text("/etc/hostname"), missing: source.bytes("nothing.png"),
                        bytes: source.bytes("pic.png"), up: source.resolve("deep/in/a.md", "../../pic.png"),
                        out: source.resolve("a.md", "../../etc/passwd"), html: omabook.markdown.toHtml("Some *slanted* words.")
                    };
                    return {title: JSON.stringify(seen), chapters: [{title: "Only", text: source.text("a.txt")}]};
                }
            });
            function later() { return kept.text("a.txt"); }
        )js");
        const QString folder = bench.dir.filePath("Peeked");
        QVERIFY(writeFolder(folder, {{"a.txt", "Inside."}, {"pic.png", Fixtures::picture(4, 4, Qt::green)}}));
        QVERIFY(Fixtures::write(bench.dir.filePath("secret.txt"), "Not the book's."));

        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        ScriptPlugin *plugin = plugins.plugin("peeks");
        QVERIFY(plugin);
        QString problem;
        const QJsonObject book = plugin->openBook("Peek", BookSource(folder), &problem);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        const QJsonObject seen = QJsonDocument::fromJson(book.value("title").toString().toUtf8()).object();
        QCOMPARE(seen.value("name").toString(), QString("Peeked"));
        QCOMPARE(seen.value("folder").toBool(), true);
        QCOMPARE(seen.value("entries").toArray(), (QJsonArray{"a.txt", "pic.png"}));
        QCOMPARE(seen.value("has").toArray(), (QJsonArray{true, false}));
        // Nothing beside the book, above it, or elsewhere on the disk.
        QVERIFY(seen.value("outside").isNull());
        QVERIFY(seen.value("absolute").isNull());
        QVERIFY(seen.value("missing").isNull());
        QVERIFY(QByteArray::fromBase64(seen.value("bytes").toString().toLatin1()).startsWith("\x89PNG"));
        QCOMPARE(seen.value("up").toString(), QString("pic.png"));
        QCOMPARE(seen.value("out").toString(), QString());
        QVERIFY2(seen.value("html").toString().contains("slanted") && !seen.value("html").toString().contains("*slanted*"),
                 qPrintable(seen.value("html").toString()));
        // A handle the script kept is good for nothing once open has answered.
        plugin->callNow("later", {}, &problem);
        QVERIFY2(problem.contains("no longer open"), qPrintable(problem));
    }

    void aFormatThatGoesWrongSaysWhereAndOpensNothing() {
        Bench bench;
        bench.plugin("wrong", R"js(
omabook.format.register({
    name: "Throws", extensions: ["throws"],
    open: function (source) {
        return source.nothing.here;
    }
});
omabook.format.register({name: "Refuses", extensions: ["refuses"], open: function (source) { throw new Error("This book is sealed."); }});
omabook.format.register({name: "Promises", extensions: ["promises"], open: function () { return Promise.resolve({title: "Later", chapters: []}); }});
omabook.format.register({name: "Empty", extensions: ["empty"], open: function () { return {title: "Nothing", chapters: []}; }});
omabook.format.register({name: "Shapeless", extensions: ["shapeless"], open: function () { return "a book"; }});
omabook.format.register({name: "Spins", extensions: ["spins"], claims: function () { for (;;) {} }, open: function () { while (true) {} }});
        )js");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        plugins.setTimeLimit(std::chrono::milliseconds(150));
        plugins.reload();
        plugins.provideFormats();
        auto opening = [&](const QString &extension) {
            const QString file = bench.dir.filePath("book." + extension);
            Fixtures::write(file, "words");
            QString error;
            const auto book = Formats::open(file, &error);
            return book ? QString("opened") : error;
        };
        // A mistake in the script is the reason the book can't be opened,
        // with where it is, and nothing falls over.
        QString error = opening("throws");
        QVERIFY2(error.startsWith("Throws (the plugin wrong) couldn't open book.throws: main.js:5: TypeError"), qPrintable(error));
        // What the script throws on purpose is said in its own words.
        QVERIFY2(opening("refuses").contains("This book is sealed."), qPrintable(opening("refuses")));
        QVERIFY2(opening("promises").contains("not a promise"), qPrintable(opening("promises")));
        QVERIFY2(opening("empty").contains("no chapters"), qPrintable(opening("empty")));
        QVERIFY2(opening("shapeless").contains("open should answer"), qPrintable(opening("shapeless")));

        // An open that never comes back is stopped, and so is a claims.
        QElapsedTimer spent;
        spent.start();
        error = opening("spins");
        QVERIFY2(error.contains("without finishing and was stopped"), qPrintable(error));
        QVERIFY(QDir().mkpath(bench.dir.filePath("A Folder")));
        QVERIFY(!Formats::providerFor(bench.dir.filePath("A Folder")));
        QVERIFY(spent.elapsed() < 3000);
        QVERIFY2(bench.error("wrong").contains("was stopped"), qPrintable(bench.error("wrong")));
        // The plugin is as good as before.
        QVERIFY2(opening("refuses").contains("This book is sealed."), qPrintable(opening("refuses")));
    }

    void aFormatGoesWhenItsPluginDoes() {
        Bench bench;
        bench.plugin("parts", partsFormat);
        const QString file = bench.dir.filePath("Filed.parts");
        QVERIFY(Fixtures::zip(file, partsBook()));
        {
            ScriptPlugins plugins(bench.services(), bench.folder);
            plugins.provideFormats();
            QVERIFY(Formats::open(file));

            // Switched off, its format is no longer one Omabook knows; on, it is again.
            bench.settings.setPluginEnabled("folder:parts", false);
            QTRY_VERIFY(!Formats::isBook(file));
            QString error;
            QVERIFY(!Formats::open(file, &error));
            QVERIFY2(error.contains("doesn't know the format"), qPrintable(error));
            bench.settings.setPluginEnabled("folder:parts", true);
            QTRY_VERIFY(Formats::isBook(file));

            // Saved with another extension, the old one is gone and the new one there.
            bench.plugin("parts", QByteArray(partsFormat).replace("[\"parts\"]", "[\"pieces\", \"pcs\"]"));
            QTRY_VERIFY(Formats::extensions().contains("pieces"));
            QVERIFY(Formats::extensions().contains("pcs"));
            QVERIFY(!Formats::isBook(file));
            // Broken, the version that worked goes on reading its books.
            bench.plugin("parts", "omabook.format.register({name: \"Parts\"});\n// longer, so it is seen to have changed\n");
            QTRY_VERIFY(!bench.error("parts").isEmpty());
            QVERIFY(Formats::extensions().contains("pieces"));
            QVERIFY(QFile::copy(file, bench.dir.filePath("Filed.pieces")));
            QVERIFY(Formats::open(bench.dir.filePath("Filed.pieces")));
            // Removed, so is its format.
            QVERIFY(QDir(bench.folder + "/parts").removeRecursively());
            QTRY_VERIFY(!Formats::extensions().contains("pieces"));

            bench.plugin("parts", partsFormat);
            QTRY_VERIFY(Formats::isBook(file));
        }
        // And when the plugins themselves go, nothing of theirs is left behind.
        QVERIFY(!Formats::isBook(file));
        for (const FormatProvider &provider : Formats::providers())
            QVERIFY2(!provider.id.startsWith("plugin:"), qPrintable(provider.id));
    }

    void aBookIsOpenedFromAnyThread() {
        Bench bench;
        bench.plugin("parts", partsFormat);
        bench.plugin("slow", "omabook.format.register({name: \"Slow\", extensions: [\"slow\"], open: function () { for (;;) {} }});");
        const QString file = bench.dir.filePath("Filed.parts"), slow = bench.dir.filePath("book.slow");
        QVERIFY(Fixtures::zip(file, partsBook()));
        QVERIFY(Fixtures::write(slow, "words"));
        auto plugins = std::make_unique<ScriptPlugins>(bench.services(), bench.folder, nullptr, false);
        plugins->setTimeLimit(std::chrono::milliseconds(200));
        plugins->reload();
        plugins->provideFormats();
        // What a thread makes of opening a book: its chapters, or why not.
        auto open = [](const QString &path) {
            QString error;
            const auto book = Formats::open(path, &error);
            return book ? QString::number(book->count()) + " chapters, " + book->meta().title : error;
        };

        // On the thread the script lives on, it is simply called.
        QCOMPARE(open(file), QString("3 chapters, Three Parts"));

        // From another thread, as the reader opens books: the script runs
        // on its own thread while this one turns its loop, and the answer
        // is plain data over there. Several at once take their turn.
        QList<QFuture<QString>> opened;
        for (int i = 0; i < 6; ++i)
            opened << QtConcurrent::run(open, file);
        for (QFuture<QString> &future : opened) {
            QTRY_VERIFY(future.isFinished());
            QCOMPARE(future.result(), QString("3 chapters, Three Parts"));
        }
        // So does asking whether a folder is a book.
        QVERIFY(writeFolder(bench.dir.filePath("A Folder"), partsBook()));
        QFuture<bool> claimed = QtConcurrent::run([&] { return Formats::providerFor(bench.dir.filePath("A Folder")).has_value(); });
        QTRY_VERIFY(claimed.isFinished());
        QVERIFY(claimed.result());

        // The script's thread waiting for the very thread that waits for
        // it, as a window closing on a book still being opened does: nobody
        // waits for ever. The opener is let go with the reason.
        QElapsedTimer spent;
        spent.start();
        QString stuck;
        std::thread opener([&] { stuck = open(file); });
        opener.join();
        QVERIFY2(stuck.contains("didn't answer in time"), qPrintable(stuck));
        QVERIFY(spent.elapsed() < 5000);
        // What was asked and given up on is not run later, and the next one is answered.
        QFuture<QString> after = QtConcurrent::run(open, file);
        QTRY_VERIFY(after.isFinished());
        QCOMPARE(after.result(), QString("3 chapters, Three Parts"));

        // A runaway open is stopped though it was asked for from elsewhere.
        QFuture<QString> runaway = QtConcurrent::run(open, slow);
        QTRY_VERIFY(runaway.isFinished());
        QVERIFY2(runaway.result().contains("was stopped"), qPrintable(runaway.result()));

        // The plugins going while a thread waits for one lets it go at once.
        spent.restart();
        QString orphan;
        std::thread left([&] { orphan = open(file); });
        QThread::msleep(50);
        plugins.reset();
        left.join();
        QVERIFY2(orphan.contains("switched off or changed"), qPrintable(orphan));
        QVERIFY(spent.elapsed() < 2000);
    }

    void aCommandLoadsThePluginsOnlyWhenAFormatIsAskedFor() {
        // What every command does first. The plugin notes each time it is
        // loaded, in its own settings.
        const QString plugin = ScriptPlugins::defaultFolder() + "/parts";
        QVERIFY(QDir().mkpath(plugin));
        QVERIFY(Fixtures::write(plugin + "/plugin.json", "{\"name\": \"Parts\", \"script\": \"main.js\"}"));
        QVERIFY(Fixtures::write(plugin + "/main.js", partsFormat + "omabook.settings.set(\"loaded\", omabook.settings.get(\"loaded\", 0) + 1);\n"));
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("one.epub")));
        QVERIFY(Fixtures::zip(dir.filePath("Filed.parts"), partsBook()));
        ScriptFormats::loadOnDemand();

        // A format Omabook reads itself is opened without a script being run.
        QVERIFY(Formats::isBook(dir.filePath("one.epub")));
        QVERIFY(Formats::open(dir.filePath("one.epub")));
        QVERIFY(!QFileInfo::exists(plugin + "/data/settings.json"));
        // One it doesn't know brings the plugins in, once.
        QVERIFY(Formats::isBook(dir.filePath("Filed.parts")));
        const auto book = Formats::open(dir.filePath("Filed.parts"));
        QVERIFY(book);
        QCOMPARE(book->count(), 3);
        QVERIFY(Formats::extensions().contains("parts"));
        QFile kept(plugin + "/data/settings.json");
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(kept.readAll()).object().value("loaded").toInt(), 1);

        // A command that runs for long, as the MCP server does, has no
        // event loop to hear of a changed file: the folder is looked at
        // again when a format is next asked about.
        QVERIFY(Fixtures::write(plugin + "/main.js", QByteArray(partsFormat).replace("[\"parts\"]", "[\"pieces\"]")));
        QThread::msleep(1100);
        QVERIFY(Formats::extensions().contains("pieces"));
        QVERIFY(!Formats::isBook(dir.filePath("Filed.parts")));

        ScriptFormats::unload();
        QVERIFY(!Formats::extensions().contains("pieces"));
        QVERIFY(QDir(plugin).removeRecursively());
    }

    void everyCommandReadsAScriptsFormat() {
        // The real binary, with no window and the scratch home: the plugin
        // is in the user's folder, and no command is told to load it.
        const QString omabook = QCoreApplication::applicationDirPath() + "/omabook";
        QVERIFY2(QFileInfo(omabook).isExecutable(), qPrintable(omabook + " should be built beside the tests"));
        const QString plugin = ScriptPlugins::defaultFolder() + "/parts";
        QVERIFY(plugin.startsWith(m_home.path()));
        QVERIFY(QDir().mkpath(plugin));
        QVERIFY(Fixtures::write(plugin + "/plugin.json", "{\"name\": \"Parts\", \"script\": \"main.js\"}"));
        QVERIFY(Fixtures::write(plugin + "/main.js", partsFormat));
        QTemporaryDir dir;
        const QString folder = dir.filePath("Three Parts"), file = dir.filePath("Filed.parts"), library = dir.filePath("Library");
        QVERIFY(writeFolder(folder, partsBook()));
        QVERIFY(Fixtures::zip(file, partsBook("Filed Parts")));
        struct Ran {
            int code;
            QString out, err;
        };
        auto run = [&](const QStringList &arguments) {
            QProcess process;
            process.start(omabook, arguments);
            const bool finished = process.waitForFinished(30000);
            return Ran{finished ? process.exitCode() : -1, QString::fromUtf8(process.readAllStandardOutput()),
                       QString::fromUtf8(process.readAllStandardError())};
        };

        Ran ran = run({"toc", file, "--json"});
        QVERIFY2(ran.code == 0 && ran.out.contains("\"sections\": 3") && ran.out.contains("\"Two\""), qPrintable(ran.out + ran.err));
        ran = run({"text", file, "--section", "2"});
        QVERIFY2(ran.code == 0 && ran.out.contains("Another paragraph."), qPrintable(ran.out + ran.err));
        ran = run({"find", file, "emphasis"});
        QVERIFY2(ran.code == 0 && ran.out.startsWith("1\t"), qPrintable(ran.out + ran.err));
        ran = run({"info", file, "--json"});
        QVERIFY2(ran.code == 0 && ran.out.contains("Filed Parts") && ran.out.contains("\"format\": \"Parts\""), qPrintable(ran.out + ran.err));
        ran = run({"add", folder, file, "--library", library, "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.out + ran.err));
        ran = run({"list", "--library", library, "--json"});
        QVERIFY2(ran.out.contains("Three Parts") && ran.out.contains("Filed Parts") && ran.out.contains("\"parts\""), qPrintable(ran.out + ran.err));
        ran = run({"text", "1", "--library", library, "--section", "0"});
        QVERIFY2(ran.code == 0 && ran.out.contains("First words"), qPrintable(ran.out + ran.err));
        ran = run({"plugin", "list", "--json"});
        QVERIFY2(ran.out.contains("Parts (.parts)"), qPrintable(ran.out + ran.err));

        // Switched off, the commands no longer know the format, and say so.
        QCOMPARE(run({"plugin", "disable", "parts"}).code, 0);
        ran = run({"toc", file});
        QVERIFY2(ran.code == 1 && ran.err.contains("doesn't know the format"), qPrintable(ran.out + ran.err));
        QVERIFY(QDir(plugin).removeRecursively());
    }

    void aPluginMakesBooksAndRewritesOnlyItsOwn() {
        Bench bench;
        bench.plugin("teller", teller, {"files"});
        bench.plugin("rival", teller, {"files"});
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        // Calls a function of a plugin's: what it answered, or what it threw.
        auto call = [&](const QString &plugin, const QString &function, const QJsonArray &arguments) {
            QString problem;
            const QJsonValue answer = plugins.plugin(plugin)->callNow(function, arguments, &problem);
            return problem.isEmpty() ? answer : QJsonValue("threw: " + problem);
        };
        auto words = [&](qint64 id) {
            QFile file(bench.library.formatPath(id));
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };

        // A book of the plugin's own: in the library with the details it
        // gave, its file copied in, and marked as made by it.
        QSignalSpy touched(&plugins, &ScriptPlugins::touchedLibrary);
        const QJsonObject made = call("teller", "make", {"The Tale", 2}).toObject();
        const qint64 tale = made.value("id").toInteger();
        QVERIFY2(tale > 0, qPrintable(QJsonDocument(made).toJson()));
        QCOMPARE(made.value("generated").toString(), QString("teller"));
        QCOMPARE(made.value("identifiers").toObject().value("generated").toString(), QString("teller"));
        QCOMPARE(made.value("formats").toArray(), QJsonArray{"txt"});
        QVERIFY(made.value("files").toObject().value("txt").toString().startsWith(bench.library.root()));
        QCOMPARE(touched.size(), 1);
        Book book = *bench.library.book(tale);
        QCOMPARE(book.meta.title, QString("The Tale"));
        QCOMPARE(book.meta.authors, QStringList{"Tess Teller"});
        QCOMPARE(book.meta.tags, QStringList{"told"});
        QCOMPARE(book.meta.series, QString("Tales"));
        QCOMPARE(book.meta.seriesIndex, 2.0);
        QCOMPARE(book.identifiers.value("generated").toString(), QString("teller"));
        QCOMPARE(Formats::open(bench.library.formatPath(tale))->count(), 2);
        QCOMPARE(call("teller", "mine", {}).toArray(), QJsonArray{tale});
        // What every plugin sees of a book says who made it.
        QCOMPARE(call("rival", "mine", {}).toArray(), QJsonArray());

        // It grows: the same book, the same one file, more in it. Whoever
        // has it open is told to read it anew.
        QSignalSpy reloads(&plugins, &ScriptPlugins::reloadBookRequested);
        const QJsonObject grown = call("teller", "grow", {tale, 5}).toObject();
        QCOMPARE(grown.value("id").toInteger(), tale);
        QCOMPARE(Formats::open(bench.library.formatPath(tale))->count(), 5);
        QCOMPARE(bench.library.formatFiles(tale).size(), 1);
        QCOMPARE(bench.library.books().size(), 1);
        QCOMPARE(reloads.size(), 1);
        QCOMPARE(reloads.first().first().toLongLong(), tale);

        // What it may not touch: a book a person added, a book on paper,
        // and another plugin's. None of the three ways in works, and each
        // says why.
        const qint64 human = bench.addBook("By A Person");
        BookMeta paper;
        paper.title = "On Paper";
        const qint64 shelved = bench.library.addEntry(paper, true).id;
        const qint64 theirs = call("rival", "make", {"Their Tale", 1}).toObject().value("id").toInteger();
        QVERIFY(human > 0 && shelved > 0 && theirs > 0);
        const QString before = Library::hashFile(bench.library.formatPath(human)), rivalBefore = words(theirs);
        reloads.clear();
        for (const qint64 id : {human, shelved, theirs}) {
            const QJsonArray refused = call("teller", "trespass", {id}).toArray();
            QCOMPARE(refused.size(), 3);
            QVERIFY2(refused.at(0).toString().contains("was not made by this plugin") && refused.at(0).toString().contains("rewritten"),
                     qPrintable(refused.at(0).toString()));
            QVERIFY2(refused.at(1).toString().contains("was not made by this plugin") && refused.at(1).toString().contains("removed"),
                     qPrintable(refused.at(1).toString()));
            QVERIFY2(refused.at(2).toString().contains("omabook.library.create alone"), qPrintable(refused.at(2).toString()));
        }
        QVERIFY(call("teller", "trespass", {theirs}).toArray().at(0).toString().contains("but by rival"));
        QCOMPARE(Library::hashFile(bench.library.formatPath(human)), before);
        QCOMPARE(words(theirs), rivalBefore);
        QVERIFY(bench.library.book(human) && bench.library.book(shelved) && bench.library.book(theirs));
        QVERIFY(bench.library.book(human)->identifiers.isEmpty());
        QVERIFY(bench.library.book(shelved)->formats.isEmpty());
        QCOMPARE(bench.library.book(theirs)->identifiers.value("generated").toString(), QString("rival"));
        QCOMPARE(reloads.size(), 0);
        // A book that isn't there is said to be not there.
        QVERIFY(call("teller", "grow", {4711, 2}).toString().contains("There is no book 4711"));

        // The very file of a person's book, offered as the plugin's own, is
        // not taken for it: the book stays the person's.
        QVERIFY(QDir().mkpath(bench.folder + "/teller/data"));
        QFile::remove(bench.folder + "/teller/data/copy.epub");
        QVERIFY(QFile::copy(bench.library.formatPath(human), bench.folder + "/teller/data/copy.epub"));
        bench.plugin("teller", teller + "function steal() { return omabook.library.create({title: \"Mine Now\"}, \"data/copy.epub\"); }\n"
                               "function from(file) { return omabook.library.create({title: \"From Elsewhere\"}, file); }\n"
                               "function other(id) { omabook.files.write(\"data/story.md\", \"# One\"); return omabook.library.replaceFile(id, \"data/story.md\"); }\n",
                     {"files"});
        plugins.reload();
        QVERIFY2(call("teller", "steal", {}).toString().contains("already in the library") , qPrintable(call("teller", "steal", {}).toString()));
        QVERIFY(bench.library.book(human)->identifiers.isEmpty());
        QCOMPARE(bench.library.book(human)->meta.title, QString("By A Person"));
        // A book comes from the plugin's data folder and nowhere else, and
        // a new version is of the format the book has.
        for (const char *elsewhere : {"main.js", "../rival/data/story.txt", "/etc/hostname", "data/nothing.txt", "data"})
            QVERIFY2(call("teller", "from", {elsewhere}).toString().contains("not a file under the plugin's data/ folder"), elsewhere);
        QVERIFY2(call("teller", "other", {tale}).toString().contains("has no .md file to replace"), qPrintable(call("teller", "other", {tale}).toString()));
        QCOMPARE(bench.library.books().size(), 4);

        // Its own it may remove; the rest stay.
        QCOMPARE(call("teller", "drop", {tale}).toBool(), true);
        QVERIFY(!bench.library.book(tale));
        QCOMPARE(bench.library.books().size(), 3);

        // Without the files permission there is nothing to make a book from.
        bench.plugin("plain", "function has() { return [typeof omabook.library.create, typeof omabook.library.replaceFile, typeof omabook.library.remove]; }");
        QCOMPARE(ScriptTools::run(bench.context(), "plain", "has").value("result").toArray(), (QJsonArray{"undefined", "undefined", "function"}));
    }

    void thePluginKnowsWhereTheReaderIsAndTheReaderFollowsAGrownBook() {
        Bench bench;
        bench.plugin("teller", teller + R"js(
            function where(event) {
                return [event.id, event.generated, event.identifiers.generated, event.section, event.sections,
                        Math.round(event.fraction * 100), Math.round(event.percent)].join(" ");
            }
            omabook.on("book.opened", function (event) { omabook.say("opened " + where(event) + " " + event.book.title); });
            omabook.on("page.turned", function (event) { omabook.say("turned " + where(event) + " = " + where(omabook.reader.current())); });
            omabook.on("book.closed", function (event) { omabook.say("closed " + where(event)); });
            function current() { return omabook.reader.current(); }
            function again() { omabook.reader.reload(); return "asked"; }
        )js", {"files"});
        bench.settings.setLibraryPath(bench.library.root());
        NoPicker picker;
        Backend backend(&bench.settings, &picker, nullptr, [](QString *) -> std::unique_ptr<AiProvider> { return nullptr; }, {},
                        bench.folder, "-");
        Reader reader(&backend, {});
        ScriptPlugins *plugins = backend.scripts();
        QSignalSpy said(plugins, &ScriptPlugins::said);
        const qint64 human = bench.addBook("By A Person");

        // With nothing open there is nowhere the reader is, and nothing to read anew.
        QVERIFY(plugins->call("teller", "current").isNull());
        QCOMPARE(plugins->call("teller", "again").toString(), QString("asked"));
        QVERIFY(!reader.isOpen());

        const qint64 tale = plugins->call("teller", "make", {"The Tale", 3}).toMap().value("id").toLongLong();
        QVERIFY(tale > 0);
        reader.openBook(tale);
        QTRY_VERIFY(reader.isOpen());
        QCOMPARE(reader.count(), 3);
        // The book, whose it is, the section of how many, and how far.
        QTRY_COMPARE(texts(said), QStringList{QString("opened %1 teller teller 0 3 0 0 The Tale").arg(tale)});
        said.clear();
        reader.goToPlace(1, 0.5);
        QTRY_COMPARE(texts(said), QStringList{QString("turned %1 teller teller 1 3 50 50 = %1 teller teller 1 3 50 50").arg(tale)});
        said.clear();
        const QVariantMap current = plugins->call("teller", "current").toMap();
        QCOMPARE(current.value("id").toLongLong(), tale);
        QCOMPARE(current.value("sections").toInt(), 3);
        QCOMPARE(current.value("section").toInt(), 1);

        // The book grows while it is open. Nobody asks the reader to
        // follow: replacing the file does. It stays where it was, with
        // more ahead of it, and the plugin hears of the new length.
        QSignalSpy changed(&reader, &Reader::bookChanged);
        plugins->call("teller", "grow", {tale, 6});
        QTRY_COMPARE(reader.count(), 6);
        QCOMPARE(changed.size(), 1);
        QVERIFY(reader.isOpen());
        QCOMPARE(reader.bookId(), tale);
        QCOMPARE(reader.section(), 1);
        QCOMPARE(reader.fraction(), 0.5);
        QCOMPARE(reader.toc().size(), 6);
        QVERIFY(reader.html().contains("chapter 2"));
        QTRY_COMPARE(texts(said), QStringList{QString("turned %1 teller teller 1 6 50 25 = %1 teller teller 1 6 50 25").arg(tale)});
        QCOMPARE(bench.library.progress(tale).section, 1);
        said.clear();

        // Asked for by hand, the same; a book that got shorter leaves the
        // reader on its last chapter rather than past its end.
        reader.goToPlace(5, 0.25);
        QTRY_COMPARE(said.size(), 1);
        said.clear();
        QFile file(bench.library.formatPath(tale));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("CHAPTER 1\n\nOnly this is left.\n\nCHAPTER 2\n\nAnd this.\n");
        file.close();
        plugins->call("teller", "again");
        QTRY_COMPARE(reader.count(), 1);
        QCOMPARE(reader.section(), 0);
        QVERIFY(reader.html().contains("And this."));
        QTRY_COMPARE(said.size(), 1);
        said.clear();

        // Two versions in a row are both followed, the second not lost.
        plugins->call("teller", "grow", {tale, 3});
        plugins->call("teller", "grow", {tale, 4});
        QTRY_COMPARE(reader.count(), 4);

        // Another book growing is none of the open one's business.
        const qint64 second = plugins->call("teller", "make", {"Another Tale", 1}).toMap().value("id").toLongLong();
        changed.clear();
        plugins->call("teller", "grow", {second, 2});
        QTest::qWait(150);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(reader.count(), 4);

        said.clear();
        reader.close();
        QCOMPARE(texts(said).last(), QString("closed %1 teller teller 0 4 25 6").arg(tale));
        said.clear();
        // A person's book is told of the same way, with nobody's name on it.
        reader.openBook(human);
        QTRY_COMPARE(texts(said), QStringList{QString("opened %1   0 2 0 0 By A Person").arg(human)});
        reader.close();
    }

    void aPluginAsksTheReaderForWords() {
        Bench bench;
        bench.plugin("asker", R"js(
            function ask(title) { return omabook.ui.ask({title: title, placeholder: "A wish", text: "north"}); }
            function plainly() { return omabook.ui.ask("Just a title"); }
        )js");
        // With no window to ask in, a command say, the answer is no answer.
        QJsonObject ran = ScriptTools::run(bench.context(), "asker", "ask", QJsonArray{"Where to?"});
        QVERIFY2(!ScriptTools::failed(ran), qPrintable(ran.value("error").toString()));
        QVERIFY(ran.value("result").isNull());

        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        ScriptPlugin *plugin = plugins.plugin("asker");
        QStringList answers;
        const ScriptPlugin::Done heard = [&](const QJsonValue &value, const QString &) {
            answers << (value.isNull() ? QString("(nothing)") : value.toString());
        };
        plugin->callFunction("ask", {"Unattended"}, heard);
        QTRY_COMPARE(answers, QStringList{"(nothing)"});
        QVERIFY(plugins.question().isEmpty());
        answers.clear();

        // A window that is there is asked: one question at a time, each
        // answered or left once.
        plugins.attend(true);
        QSignalSpy asked(&plugins, &ScriptPlugins::questionChanged);
        plugin->callFunction("ask", {"Where to?"}, heard);
        plugin->callFunction("plainly", {}, heard);
        QCOMPARE(asked.size(), 2);
        QVariantMap question = plugins.question();
        QCOMPARE(question.value("title").toString(), QString("Where to?"));
        QCOMPARE(question.value("placeholder").toString(), QString("A wish"));
        QCOMPARE(question.value("text").toString(), QString("north"));
        QCOMPARE(question.value("plugin").toString(), QString("asker"));
        QVERIFY(!plugin->idle());
        plugins.answer(question.value("id").toInt(), "Go south, past the mill.");
        QTRY_COMPARE(answers, QStringList{"Go south, past the mill."});
        question = plugins.question();
        QCOMPARE(question.value("title").toString(), QString("Just a title"));
        QCOMPARE(question.value("text").toString(), QString());
        plugins.dismiss(question.value("id").toInt());
        QTRY_COMPARE(answers, (QStringList{"Go south, past the mill.", "(nothing)"}));
        QVERIFY(plugins.question().isEmpty());
        // Answered twice, or an answer to nothing, is nothing.
        plugins.answer(question.value("id").toInt(), "again");
        plugins.dismiss(4711);
        QTest::qWait(20);
        QCOMPARE(answers.size(), 2);
        // An empty answer is an answer, and not the same as none.
        plugin->callFunction("plainly", {}, heard);
        plugins.answer(plugins.question().value("id").toInt(), "");
        QTRY_COMPARE(answers.size(), 3);
        QCOMPARE(answers.last(), QString());

        // The window going leaves what was asked unanswered, and so does
        // the plugin: its question isn't left on show.
        plugin->callFunction("ask", {"Left hanging"}, heard);
        plugins.attend(false);
        QTRY_COMPARE(answers.size(), 4);
        QCOMPARE(answers.last(), QString("(nothing)"));
        plugins.attend(true);
        plugin->callFunction("ask", {"From a plugin about to go"}, heard);
        QVERIFY(!plugins.question().isEmpty());
        QVERIFY(QDir(bench.folder + "/asker").removeRecursively());
        plugins.reload();
        QVERIFY(plugins.question().isEmpty());
    }

    void aPluginChoosesTheModelItAsks() {
        // What a plugin writes is for a person, so the main model answers
        // unless the plugin asks for the quick one, or for a model by name.
        struct TierAi : AiProvider {
            QString name() const override { return "Tier"; }
            void ask(const AiRequest &request) override {
                const QString heard = QString(request.tier == AiRequest::Quick ? "quick" : "main") + "/" + modelFor(request);
                QTimer::singleShot(0, this, [this, heard] {
                    emit delta(heard);
                    emit finished();
                });
            }
            void cancel() override {}
        };
        Bench bench;
        bench.plugin("picker", R"js(
            function plain() { return omabook.ai.ask("One."); }
            function quick() { return omabook.ai.ask("Two.", {model: "quick"}); }
            function named() { return omabook.ai.ask("Three.", {model: "opus", system: "Be brief."}); }
        )js");
        ScriptServices services = bench.services();
        services.ai = [](QString *) -> std::unique_ptr<AiProvider> {
            auto provider = std::make_unique<TierAi>();
            provider->setModels({"big", "small"});
            return provider;
        };
        ScriptPlugin plugin(ScriptManifest::read(bench.folder + "/picker"), services);
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        QStringList heard;
        for (const char *function : {"plain", "quick", "named"})
            plugin.callFunction(function, {}, [&](const QJsonValue &value, const QString &) { heard << value.toString(); });
        QTRY_COMPARE(heard.size(), 3);
        heard.sort();
        QCOMPARE(heard, QStringList({"main/big", "main/opus", "quick/small"}));
        // The one table of the API says so.
        QVERIFY(ScriptApi::markdown().contains("ai.ask(prompt, {system, onText, model})"));
    }

    void aLongAnswerFromTheModelArrivesWhole() {
        Bench bench;
        bench.plugin("writer", R"js(
            var pieces = 0, words = "";
            function chapter() {
                return omabook.ai.ask("Write the next chapter.", {
                    system: "You are a storyteller.",
                    onText: function (piece) { pieces += 1; words += piece; }
                }).then(function (answer) { return {pieces: pieces, same: words === answer, length: answer.length, end: answer.slice(-30)}; });
            }
            function oldWay() { return omabook.ai.ask("Write.", "As before."); }
            function careless() { return omabook.ai.ask("Write.", {onText: function (piece) { return piece.nothing.here; }}); }
        )js");
        // The answer takes six times as long as any call into the script
        // may run: what is timed is the script, not the waiting.
        ScriptServices services = bench.services();
        services.ai = [](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<SlowAi>(40, 25); };
        ScriptPlugin plugin(ScriptManifest::read(bench.folder + "/writer"), services);
        plugin.setTimeLimit(std::chrono::milliseconds(150));
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        QJsonObject wrote;
        QElapsedTimer spent;
        spent.start();
        plugin.callFunction("chapter", {}, [&](const QJsonValue &value, const QString &why) {
            wrote = value.toObject();
            problem = why;
        });
        QTRY_VERIFY_WITH_TIMEOUT(!wrote.isEmpty() || !problem.isEmpty(), 10000);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QVERIFY(spent.elapsed() >= 900);
        QCOMPARE(wrote.value("pieces").toInt(), 40);
        QCOMPARE(wrote.value("same").toBool(), true);
        QVERIFY2(wrote.value("end").toString().endsWith("Paragraph 40 of the chapter. "), qPrintable(wrote.value("end").toString()));
        QCOMPARE(SlowAi::system, QString("You are a storyteller."));
        // The system prompt alone, as it was before there were options.
        QString answer;
        plugin.callFunction("oldWay", {}, [&](const QJsonValue &value, const QString &) { answer = value.toString(); });
        QTRY_VERIFY_WITH_TIMEOUT(!answer.isEmpty(), 10000);
        QCOMPARE(SlowAi::system, QString("As before."));
        // An onText that throws is the script's mistake to hear of, and
        // the answer still arrives.
        QSignalSpy failed(&plugin, &ScriptPlugin::failed);
        answer.clear();
        plugin.callFunction("careless", {}, [&](const QJsonValue &value, const QString &) { answer = value.toString(); });
        QTRY_VERIFY_WITH_TIMEOUT(!answer.isEmpty(), 10000);
        QVERIFY(failed.size() >= 1);
        QVERIFY2(failed.first().first().toString().startsWith("main.js:"), qPrintable(failed.first().first().toString()));
    }

    void aPluginRunFromAnyFolderHasItsOwnFormatsForTheRun() {
        // The bench's plugin folder is not the user's, and nothing has the
        // plugins in it loaded: a run does that for the one it runs.
        Bench bench;
        bench.plugin("story", partsFormat + R"js(
            function make() {
                omabook.files.write("data/a.parts", "not a zip, but a file of the format by its name");
                return omabook.library.create({title: "Made In A Run"}, "data/a.parts");
            }
            function known() { return omabook.library.list().map(function (book) { return book.formats.join(); }); }
        )js", {"files"});
        QVERIFY(!Formats::extensions().contains("parts"));
        const QJsonObject ran = ScriptTools::run(bench.context(), "story", "make");
        QVERIFY2(!ScriptTools::failed(ran), qPrintable(ran.value("error").toString()));
        QCOMPARE(ran.value("result").toObject().value("formats").toArray(), QJsonArray{"parts"});
        QCOMPARE(ran.value("result").toObject().value("generated").toString(), QString("story"));
        QCOMPARE(bench.library.books().size(), 1);
        // And only for the run.
        QVERIFY(!Formats::extensions().contains("parts"));
        for (const FormatProvider &provider : Formats::providers())
            QVERIFY2(!provider.id.startsWith("plugin:"), qPrintable(provider.id));
        QVERIFY(ScriptTools::check(bench.context(), "story").value("ok").toBool());
        QVERIFY(!Formats::extensions().contains("parts"));

        // Where the plugins are loaded for good, a run leaves their
        // formats as it found them.
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        plugins.provideFormats();
        QVERIFY(Fixtures::zip(bench.dir.filePath("Filed.parts"), partsBook()));
        QVERIFY(Formats::open(bench.dir.filePath("Filed.parts")));
        QCOMPARE(ScriptTools::run(bench.context(), "story", "known").value("result").toArray(), QJsonArray{"parts"});
        QVERIFY(Formats::open(bench.dir.filePath("Filed.parts")));
    }

    void aRunEndsWhenItsAnswerIsInAndWaitsOnlyAsLongAsItIsTold() {
        Bench bench;
        bench.plugin("timed", R"js(
            function quick() { omabook.after(4000, function () { omabook.settings.set("late", true); }); return "answered"; }
            function held() {
                omabook.wait(new Promise(function (resolve) {
                    omabook.after(300, function () { omabook.settings.set("kept", true); resolve(); });
                }));
                return "answered";
            }
            function never() { return new Promise(function () {}); }
            function kept() { return omabook.settings.all(); }
        )js");
        // A timer the script set is not what the command was asked for.
        QElapsedTimer spent;
        spent.start();
        QCOMPARE(ScriptTools::run(bench.context(), "timed", "quick").value("result").toString(), QString("answered"));
        QVERIFY2(spent.elapsed() < 1500, qPrintable(QString::number(spent.elapsed())));
        // What the script asks to be waited for, is.
        spent.restart();
        QCOMPARE(ScriptTools::run(bench.context(), "timed", "held").value("result").toString(), QString("answered"));
        QVERIFY(spent.elapsed() >= 300);
        QCOMPARE(ScriptTools::run(bench.context(), "timed", "kept").value("result").toObject(), (QJsonObject{{"kept", true}}));

        // How long it waits is the caller's to say, and when it gives up it
        // says what became of the work and how to go on.
        ScriptTools::Context impatient = bench.context();
        impatient.waitMinutes = 0.005;
        spent.restart();
        const QJsonObject gaveUp = ScriptTools::run(impatient, "timed", "never");
        QVERIFY(spent.elapsed() >= 300 && spent.elapsed() < 3000);
        const QString why = gaveUp.value("error").toString();
        QVERIFY2(why.contains("stopped waiting") && why.contains("--wait <minutes>") && why.contains("Run it again"), qPrintable(why));
        // On the command line it is --wait.
        const QStringList at{"--folder", bench.folder};
        QCOMPARE(runCli(QStringList{"omabook", "plugin", "run", "timed", "quick", "--wait", "2"} + at), 0);
        QCOMPARE(runCli(QStringList{"omabook", "plugin", "run", "timed", "never", "--wait", "0.005"} + at), 1);
    }

    void commandsAndPanelsApplyWhereThePluginSaysTheyDo() {
        Bench bench;
        const QString path = bench.plugin("teller", teller + R"js(
            omabook.commands.register({id: "steer", title: "Steer the story", place: "reader", key: "Ctrl+R", when: "generated", run: function () { omabook.say("steered"); }});
            omabook.commands.register({id: "new", title: "A new story", place: "library", key: "Ctrl+J", run: function () { omabook.say("new"); }});
            omabook.ui.panel({place: "reader.side", title: "Story", source: "Story.qml", when: "generated"});
            omabook.ui.panel({place: "library.side", title: "Stories", source: "Shelf.qml"});
            function enable(id, on) { omabook.commands.setEnabled(id, on); }
            function show(source, on) { omabook.ui.setVisible(source, on); }
            function wrong() { return [tryTo(function () { omabook.commands.setEnabled("nothing", true); }), tryTo(function () { omabook.ui.setVisible("Nothing.qml", true); }),
                                       tryTo(function () { omabook.commands.register({id: "x", title: "X", when: "sometimes", run: function () {}}); })]; }
        )js", {"files"});
        Fixtures::write(path + "/Story.qml", "import QtQuick\nItem {}\n");
        Fixtures::write(path + "/Shelf.qml", "import QtQuick\nItem {}\n");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        ScriptPlugin *plugin = plugins.plugin("teller");
        QVERIFY(plugin);
        Customization custom(bench.dir.filePath("custom"));
        const auto follow = [&] { custom.setPlugins(plugins.commands(), plugins.panels()); };
        connect(&plugins, &ScriptPlugins::changed, &custom, follow);
        follow();
        auto offered = [&] {
            QStringList ids;
            for (const QVariant &command : custom.pluginCommands())
                ids << command.toMap().value("id").toString();
            return ids;
        };
        auto panels = [&] {
            QStringList titles;
            for (const QVariant &panel : custom.mods())
                titles << panel.toMap().value("title").toString();
            return titles;
        };
        QString problem;
        const qint64 own = plugin->callNow("make", {"The Tale", 2}, &problem).toObject().value("id").toInteger();
        const qint64 human = bench.addBook("By A Person");
        QVERIFY2(own > 0 && human > 0, qPrintable(problem));

        // With no book in front of the reader, what is only for the
        // plugin's own books is not there.
        QCOMPARE(offered(), QStringList{"library.teller:new"});
        QCOMPARE(panels(), QStringList{"Stories"});
        // It keeps its key, though, so keys.json may still name it.
        QCOMPARE(custom.chordKeys().value("reader.teller:steer").toStringList(), QStringList{"Ctrl+R"});
        QVERIFY(QDir().mkpath(custom.folder()));
        QVERIFY(Fixtures::write(custom.folder() + "/keys.json", "{\"reader\": {\"F6\": \"teller:steer\"}}"));
        QCOMPARE(custom.check(), QString());

        // Its own book selected, or open: there. A person's: not.
        plugins.setSelectedBook(own);
        QCOMPARE(offered(), (QStringList{"reader.teller:steer", "library.teller:new"}));
        QCOMPARE(panels(), (QStringList{"Story", "Stories"}));
        plugins.setSelectedBook(human);
        QCOMPARE(offered(), QStringList{"library.teller:new"});
        QCOMPARE(panels(), QStringList{"Stories"});
        // The open book counts before the selected one.
        plugins.emitEvent("book.opened", {{"id", own}, {"book", QJsonObject{{"id", own}}}, {"section", 0}});
        QCOMPARE(offered().size(), 2);
        QCOMPARE(panels().size(), 2);
        plugins.emitEvent("book.closed", {{"id", own}});
        QCOMPARE(offered().size(), 1);
        // A book that becomes the plugin's while it is selected is seen to.
        plugins.setSelectedBook(0);
        const qint64 second = plugin->callNow("make", {"Another Tale", 3}, &problem).toObject().value("id").toInteger();
        plugins.setSelectedBook(second);
        QCOMPARE(panels().size(), 2);

        // And the script itself can take a command or a panel away, and
        // bring it back, whatever book is there.
        plugin->callNow("enable", {"new", false}, &problem);
        plugin->callNow("show", {"Shelf.qml", false}, &problem);
        QTRY_COMPARE(offered(), QStringList{"reader.teller:steer"});
        QCOMPARE(panels(), QStringList{"Story"});
        plugin->callNow("enable", {"new", true}, &problem);
        plugin->callNow("show", {"Shelf.qml", true}, &problem);
        QTRY_COMPARE(offered().size(), 2);
        QCOMPARE(panels().size(), 2);
        const QJsonArray wrong = plugin->callNow("wrong", {}, &problem).toArray();
        QVERIFY2(wrong.at(0).toString().contains("no command") && wrong.at(1).toString().contains("no panel")
                     && wrong.at(2).toString().contains("always, generated"), qPrintable(QJsonDocument(wrong).toJson()));
    }

    void commandsAreRunByNameAndFunctionsAnswerLater() {
        Bench bench;
        for (const char *name : {"north", "south"})
            bench.plugin(name, R"js(
                omabook.commands.register({id: "go", title: "Go", run: function (context) { omabook.say(omabook.plugin.id + " went for " + context.book); }});
                function later(words, wait) { return new Promise(function (resolve) { omabook.after(wait, function () { resolve({heard: words, by: omabook.plugin.id}); }); }); }
                function atOnce(n) { return n * 2; }
                function broken() { return nothing.here; }
                function refuses() { return Promise.reject(new Error("not today")); }
            )js");
        bench.plugin("lone", "omabook.commands.register({id: \"only\", title: \"Only\", place: \"global\", run: function () { omabook.say(\"the only one\"); }});"
                             "omabook.ui.showGraph({scope: \"world:lone\"});\n"
                             "function graph(what) { omabook.ui.showGraph(what); }");
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        QSignalSpy said(&plugins, &ScriptPlugins::said);

        // A command is one plugin's: named whole, or by plugin and id. The
        // id alone is enough only when nobody else has it.
        plugins.run("library.north:go", 7);
        plugins.run("south:go", 8);
        plugins.run("only", 9);
        QTRY_COMPARE(texts(said), (QStringList{"north went for 7", "south went for 8", "the only one"}));
        said.clear();
        plugins.run("go", 1);
        plugins.run("nothing", 1);
        QTest::qWait(30);
        QCOMPARE(said.size(), 1);
        QVERIFY2(texts(said).first().contains("Several plugins have a command \"go\"") && texts(said).first().contains("north:go"),
                 qPrintable(texts(said).first()));

        // A panel asks and is answered later, a promise waited for.
        QSignalSpy answered(&plugins, &ScriptPlugins::answered);
        const int slow = plugins.request("north", "later", {"hello", 60});
        const int fast = plugins.request("south", "atOnce", {21});
        QVERIFY(slow > 0 && fast > 0 && slow != fast);
        // Never before the caller has its number.
        QCOMPARE(answered.size(), 0);
        QTRY_COMPARE(answered.size(), 2);
        QCOMPARE(answered.at(0).at(0).toInt(), fast);
        QCOMPARE(answered.at(0).at(1).toInt(), 42);
        QCOMPARE(answered.at(0).at(2).toString(), QString());
        QCOMPARE(answered.at(1).at(0).toInt(), slow);
        QCOMPARE(answered.at(1).at(1).toMap(), (QVariantMap{{"heard", "hello"}, {"by", "north"}}));
        answered.clear();
        // What went wrong comes the same way, with where.
        const int broken = plugins.request("north", "broken"), refuses = plugins.request("north", "refuses"),
                  nobody = plugins.request("nobody", "later"), nothing = plugins.request("north", "noSuchFunction");
        QTRY_COMPARE(answered.size(), 4);
        QHash<int, QString> errors;
        for (const QList<QVariant> &call : answered)
            errors.insert(call.at(0).toInt(), call.at(2).toString());
        QVERIFY2(errors.value(broken).startsWith("main.js:5: ReferenceError"), qPrintable(errors.value(broken)));
        QVERIFY2(errors.value(refuses).contains("not today"), qPrintable(errors.value(refuses)));
        QVERIFY2(errors.value(nobody).contains("no plugin nobody"), qPrintable(errors.value(nobody)));
        QVERIFY2(errors.value(nothing).contains("no function noSuchFunction"), qPrintable(errors.value(nothing)));

        // The graph is asked for by scope or by book; with nobody to show
        // it (at load, in a command) nothing happens.
        QSignalSpy graph(&plugins, &ScriptPlugins::graphRequested);
        QString problem;
        plugins.plugin("lone")->callNow("graph", {QJsonObject{{"scope", "world:lone"}}}, &problem);
        plugins.plugin("lone")->callNow("graph", {QJsonObject{{"book", 12}}}, &problem);
        plugins.plugin("lone")->callNow("graph", {}, &problem);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(graph.size(), 3);
        QCOMPARE(graph.at(0).at(0).toString(), QString("world:lone"));
        QCOMPARE(graph.at(0).at(1).toLongLong(), 0);
        QCOMPARE(graph.at(1).at(1).toLongLong(), 12);
        QCOMPARE(graph.at(2).at(0).toString(), QString());
        QVERIFY(ScriptTools::check(bench.context(), "lone").value("ok").toBool());
    }

    void aBookOpensWhereAPluginSaysAndItsOwnIsMovedThere() {
        Bench bench;
        bench.plugin("teller", teller + R"js(
            function openAt(id, section, fraction) { omabook.reader.open(id, {section: section, fraction: fraction}); }
            function open(id) { omabook.reader.open(id); }
            function move(id, section, fraction) { return omabook.library.setProgress(id, {section: section, fraction: fraction}); }
            function moveOthers(id) { return tryTo(function () { omabook.library.setProgress(id, {section: 1, fraction: 0.5}); }); }
        )js", {"files"});
        bench.settings.setLibraryPath(bench.library.root());
        NoPicker picker;
        Backend backend(&bench.settings, &picker, nullptr, [](QString *) -> std::unique_ptr<AiProvider> { return nullptr; }, {},
                        bench.folder, "-");
        Reader reader(&backend, {});
        ScriptPlugins *plugins = backend.scripts();
        const qint64 tale = plugins->call("teller", "make", {"The Tale", 4}).toMap().value("id").toLongLong();
        const qint64 human = bench.addBook("By A Person");
        QVERIFY(tale > 0 && human > 0);

        // Where its own book has been read to is the plugin's to say: a
        // story that branched opens at the branch.
        const QVariantMap moved = plugins->call("teller", "move", {tale, 2, 0.5}).toMap();
        QCOMPARE(moved.value("section").toInt(), 2);
        QCOMPARE(moved.value("percent").toDouble(), 62.5);
        QCOMPARE(bench.library.progress(tale).section, 2);
        QCOMPARE(bench.library.progress(tale).fraction, 0.5);
        // Past the end is the last chapter.
        QCOMPARE(plugins->call("teller", "move", {tale, 40, 0}).toMap().value("section").toInt(), 3);
        // A person's book is where the person left it.
        bench.library.setProgress(human, {1, 0.25, 62});
        QVERIFY(plugins->call("teller", "moveOthers", {human}).toString().contains("was not made by this plugin"));
        QCOMPARE(bench.library.progress(human).fraction, 0.25);

        // Opened plainly, a book opens where it was read to; opened at a
        // place, there, whoever's it is.
        plugins->call("teller", "move", {tale, 1, 0.75});
        plugins->call("teller", "open", {tale});
        QTRY_VERIFY(reader.isOpen());
        QCOMPARE(reader.section(), 1);
        QCOMPARE(reader.fraction(), 0.75);
        plugins->call("teller", "openAt", {tale, 3, 0.25});
        QTRY_VERIFY(reader.isOpen() && reader.section() == 3);
        QCOMPARE(reader.fraction(), 0.25);
        plugins->call("teller", "openAt", {human, 1, 0.5});
        QTRY_VERIFY(reader.isOpen() && reader.bookId() == human);
        QCOMPARE(reader.section(), 1);
        QCOMPARE(reader.fraction(), 0.5);
        // Past the end is the last chapter here too.
        plugins->call("teller", "openAt", {tale, 99, 0});
        QTRY_VERIFY(reader.isOpen() && reader.bookId() == tale);
        QCOMPARE(reader.section(), 3);
        reader.close();
    }

    void whatTheEngineLacksIsGivenWhereItIsCheap() {
        Bench bench;
        bench.plugin("modern", R"js(
            function given() {
                var seen = [];
                for (var key in [1, 2]) seen.push(key);
                return {
                    flat: [1, [2, [3, [4]]]].flat(), deep: [1, [2, [3, [4]]]].flat(Infinity), holes: [1, , [2]].flat().length,
                    flatMap: [1, 2].flatMap(function (n) { return [n, n * 10]; }),
                    at: [[1, 2, 3].at(-1), [1, 2, 3].at(0), "abc".at(-1)],
                    findLast: [1, 2, 3, 4].findLast(function (n) { return n % 2 === 1; }),
                    replaceAll: ["a.b.c".replaceAll(".", "-"), "aXbXc".replaceAll(/x/gi, "$&$&"), "1+1".replaceAll("1", function (found, at) { return found + at; }),
                                 "a$b".replaceAll("$", "$$")],
                    notGlobal: tryTo(function () { "a".replaceAll(/a/, "b"); }),
                    trims: ["  a ".trimStart(), " a  ".trimEnd()],
                    fromEntries: Object.fromEntries([["a", 1], ["b", 2]]), fromMap: Object.fromEntries(new Map([["k", "v"]])),
                    global: globalThis.omabook === omabook, enumerated: seen
                };
            }
            function tryTo(what) { try { what(); return "done"; } catch (error) { return error.name; } }
            function promised() {
                var order = [];
                return Promise.allSettled([Promise.resolve(1), Promise.reject(new Error("no")), 3]).then(function (all) {
                    return Promise.resolve("kept").finally(function () { order.push("finally"); }).then(function (value) {
                        return Promise.reject(new Error("thrown")).finally(function () { order.push("again"); }).then(null, function (error) {
                            return {statuses: all.map(function (one) { return one.status; }), value: all[2].value, reason: all[1].reason.message,
                                    kept: value, order: order, rethrown: error.message};
                        });
                    });
                });
            }
        )js");
        const QJsonObject given = ScriptTools::run(bench.context(), "modern", "given").value("result").toObject();
        QCOMPARE(given.value("flat").toArray(), (QJsonArray{1, 2, QJsonArray{3, QJsonArray{4}}}));
        QCOMPARE(given.value("deep").toArray(), (QJsonArray{1, 2, 3, 4}));
        QCOMPARE(given.value("holes").toInt(), 2);
        QCOMPARE(given.value("flatMap").toArray(), (QJsonArray{1, 10, 2, 20}));
        QCOMPARE(given.value("at").toArray(), (QJsonArray{3, 1, "c"}));
        QCOMPARE(given.value("findLast").toInt(), 3);
        QCOMPARE(given.value("replaceAll").toArray(), (QJsonArray{"a-b-c", "aXXbXXc", "10+12", "a$b"}));
        QCOMPARE(given.value("notGlobal").toString(), QString("TypeError"));
        QCOMPARE(given.value("trims").toArray(), (QJsonArray{"a ", " a"}));
        QCOMPARE(given.value("fromEntries").toObject(), (QJsonObject{{"a", 1}, {"b", 2}}));
        QCOMPARE(given.value("fromMap").toObject(), (QJsonObject{{"k", "v"}}));
        QCOMPARE(given.value("global").toBool(), true);
        // None of it shows up when an array is walked by its keys.
        QCOMPARE(given.value("enumerated").toArray(), (QJsonArray{"0", "1"}));
        const QJsonObject promised = ScriptTools::run(bench.context(), "modern", "promised").value("result").toObject();
        QCOMPARE(promised.value("statuses").toArray(), (QJsonArray{"fulfilled", "rejected", "fulfilled"}));
        QCOMPARE(promised.value("value").toInt(), 3);
        QCOMPARE(promised.value("reason").toString(), QString("no"));
        QCOMPARE(promised.value("kept").toString(), QString("kept"));
        QCOMPARE(promised.value("order").toArray(), (QJsonArray{"finally", "again"}));
        QCOMPARE(promised.value("rethrown").toString(), QString("thrown"));

        // What can't be given is what the guide says it is: each of these
        // is a mistake said with its line, and the two that fail silently
        // do fail.
        const QHash<QString, QString> lacking{
            {"async function f() {}", "SyntaxError"}, {"var {a, ...rest} = {a: 1};", "SyntaxError"}, {"var o = {...{a: 1}};", "SyntaxError"},
            {"class A { x = 1 }", "SyntaxError"}, {"try { f(); } catch { }", "SyntaxError"}, {"var a; a ?" "?= 1;", "SyntaxError"},
            {"var r = /a.b/s;", "SyntaxError"}};
        for (auto it = lacking.begin(); it != lacking.end(); ++it) {
            bench.plugin("lacks", ("var fine = 1;\n" + it.key()).toUtf8());
            const QString error = ScriptTools::check(bench.context(), "lacks").value("error").toString();
            QVERIFY2(error.startsWith("main.js:2: " + it.value()), qPrintable(it.key() + " gave: " + error));
        }
        bench.plugin("silent", "function quiet() { return [/\\p{L}/u.test(\"a\"), /(?<=a)b/.test(\"ab\"), typeof \"\".matchAll, typeof BigInt, typeof Intl,"
                               " typeof structuredClone, typeof setTimeout, /a(?=b)/.test(\"ab\")]; }");
        QCOMPARE(ScriptTools::run(bench.context(), "silent", "quiet").value("result").toArray(),
                 (QJsonArray{false, false, "undefined", "undefined", "undefined", "undefined", "undefined", true}));
    }

    void theInstructionsAreMadeFromTheTable() {
        const QString guide = ScriptApi::guide();
        for (const ScriptApi::Entry &entry : ScriptApi::entries())
            QVERIFY2(guide.contains("`omabook." + entry.signature + "`"), qPrintable(entry.signature));
        for (const ScriptApi::Event &event : ScriptApi::events())
            QVERIFY2(guide.contains("`" + event.name + "`"), qPrintable(event.name));
        for (const ScriptApi::Permission &permission : ScriptApi::permissions())
            QVERIFY2(guide.contains("| `" + permission.name + "` |"), qPrintable(permission.name));
        // The worked examples are the templates, whole.
        QVERIFY(guide.contains(QString::fromUtf8(ScriptApi::templateFile("hook", "main.js")).replace("{{name}}", "finished")));
        QVERIFY(guide.contains("omabook plugin check"));
        QVERIFY(guide.contains("plugin_write"));
        QVERIFY(!guide.contains("{{"));
        QVERIFY(!guide.contains("%1") && !guide.contains("%2") && !guide.contains("%3") && !guide.contains("%4") && !guide.contains("%5"));
        QVERIFY(guide.contains("### Adding a format"));
        QVERIFY(guide.contains("### A plugin that makes books"));
        // A plugin has one name, its folder's; `folder:` is only how the
        // list of every kind of plugin writes it.
        QVERIFY(guide.contains("The folder's name is the plugin's id"));
        QVERIFY(!guide.contains("its id is\n`folder:<name>`"));
        QVERIFY(guide.contains("`omabook plugins`, and its `enable`/`disable`"));
        for (const char *lack : {"object rest and spread", "lookbehind", "never match", "`String.prototype.replaceAll`", "--wait <minutes>"})
            QVERIFY2(guide.contains(lack), lack);
        QVERIFY(guide.contains(QString::fromUtf8(ScriptApi::templateFile("format", "main.js")).replace("{{name}}", "textbook")));

        const QJsonObject json = ScriptApi::json();
        QCOMPARE(json.value("functions").toArray().size(), ScriptApi::entries().size());
        QCOMPARE(json.value("events").toArray().size(), ScriptApi::events().size());
        QCOMPARE(json.value("permissions").toArray().size(), ScriptApi::permissions().size());

        // It is in the guide Omabook writes beside the interface's files.
        QTemporaryDir dir;
        const Customization custom(dir.filePath("custom"));
        QFile written(custom.writeGuide());
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QString agents = QString::fromUtf8(written.readAll());
        QVERIFY(agents.contains("## Writing a plugin"));
        QVERIFY(agents.contains("`omabook.library.setMeta(id, details)`"));
        QVERIFY(agents.contains("## keys.json"));
    }

    void aFormIsCheckedAndAnsweredInTheShapeItAsked() {
        // What a form may be, and what it is filled in to start with.
        QString error;
        const QJsonObject form = ScriptForm::checked(QJsonDocument::fromJson(R"({
            "title": " Start a story ", "intro": "A few words.", "accept": "Start",
            "fields": [
                {"id": "wish", "type": "textarea", "label": "What should it be?", "required": true},
                {"id": "where", "type": "choice", "label": "Where", "choices": ["new", {"value": "world", "label": "In a world", "help": "Shared"}]},
                {"id": "story", "type": "choice", "label": "Which", "choices": ["a", "b"], "when": {"where": "world"}},
                {"id": "skills", "type": "multi", "label": "Skills", "choices": ["lush", "wry", "spare"], "value": ["wry"]},
                {"id": "words", "type": "number", "label": "Words", "min": 150, "max": 6000, "step": 50, "value": 1200},
                {"id": "ahead", "type": "toggle", "label": "Write ahead", "value": true},
                {"id": "name", "label": "Pen name"}
            ]})").object(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(form.value("title").toString(), QString("Start a story"));
        const QJsonArray fields = form.value("fields").toArray();
        QCOMPARE(fields.size(), 7);
        QCOMPARE(fields.at(1).toObject().value("value").toString(), QString("new"));
        QCOMPARE(fields.at(1).toObject().value("choices").toArray().at(1).toObject().value("help").toString(), QString("Shared"));
        QCOMPARE(fields.at(6).toObject().value("type").toString(), QString("text"));

        // Answered with a value of its type for every field, within bounds;
        // what doesn't fit keeps where it started.
        QJsonObject values = ScriptForm::values(form, QJsonDocument::fromJson(R"({
            "wish": "a ship", "where": "elsewhere", "skills": ["spare", "made-up", "lush", "spare"], "words": 99999,
            "ahead": "yes", "name": "  Pen  ", "unknown": 1})").object());
        QCOMPARE(values.value("wish").toString(), QString("a ship"));
        QCOMPARE(values.value("where").toString(), QString("new"));
        QCOMPARE(values.value("story").toString(), QString("a"));
        QCOMPARE(values.value("skills").toArray(), (QJsonArray{"lush", "spare"}));
        QCOMPARE(values.value("words").toDouble(), 6000.0);
        QCOMPARE(values.value("ahead").toBool(), true);
        QCOMPARE(values.value("name").toString(), QString("Pen"));
        QVERIFY(!values.contains("unknown"));
        QCOMPARE(ScriptForm::values(form, {{"words", 1234}}).value("words").toDouble(), 1250.0);
        // Shown only while what it depends on says so; required only when shown.
        QVERIFY(!ScriptForm::shown(fields.at(2).toObject(), values));
        QVERIFY(ScriptForm::shown(fields.at(2).toObject(), ScriptForm::values(form, {{"where", "world"}})));
        QCOMPARE(ScriptForm::missing(form, ScriptForm::values(form, {})), QString("What should it be?"));
        QCOMPARE(ScriptForm::missing(form, values), QString());

        // What can't be drawn says why.
        const QList<QPair<QByteArray, QString>> wrong{
            {R"({"fields": []})", "needs a title"},
            {R"({"title": "T", "fields": {}})", "should be a list"},
            {R"({"title": "T", "fields": [{"id": "a"}, {"id": "a"}]})", "Two fields are called \"a\""},
            {R"({"title": "T", "fields": [{"id": "9a"}]})", "A field's id"},
            {R"({"title": "T", "fields": [{"id": "a", "type": "slider"}]})", "of the type \"slider\""},
            {R"({"title": "T", "fields": [{"id": "a", "type": "choice", "choices": []}]})", "no choices"},
            {R"({"title": "T", "fields": [{"id": "a", "type": "choice", "choices": ["x", "x"]}]})", "offers \"x\" twice"},
            {R"({"title": "T", "fields": [{"id": "a", "type": "number", "min": 5, "max": 1}]})", "min is above its max"},
            {R"({"title": "T", "fields": [{"id": "a", "type": "choice", "choices": ["x"], "value": "y"}]})", "starts at a value"},
            {R"({"title": "T", "fields": [{"id": "a", "when": {"b": "x"}}, {"id": "b"}]})", "not a field above it"},
        };
        for (const auto &[json, why] : wrong) {
            QString problem;
            QVERIFY(ScriptForm::checked(QJsonDocument::fromJson(json).object(), &problem).isEmpty());
            QVERIFY2(problem.contains(why), qPrintable(problem + " / " + why));
        }
        // No fields at all is a confirmation.
        QString none;
        QVERIFY(!ScriptForm::checked(QJsonObject{{"title", "Remove it?"}}, &none).isEmpty() && none.isEmpty());
    }

    void aPluginPutsAFormToTheReader() {
        Bench bench;
        bench.plugin("former", R"js(
            function start() {
                return omabook.ui.form({title: "Start a story", accept: "Start", fields: [
                    {id: "wish", type: "text", label: "Wish", required: true},
                    {id: "chapters", type: "number", label: "Chapters", min: 2, max: 60, value: 12},
                    {id: "lush", type: "toggle", label: "Lush"}]});
            }
            function broken() { return omabook.ui.form({title: "T", fields: [{id: "a"}, {id: "a"}]}); }
        )js");
        // No window, no answer; a form that can't be drawn throws at once.
        QJsonObject ran = ScriptTools::run(bench.context(), "former", "start", QJsonArray());
        QVERIFY2(!ScriptTools::failed(ran), qPrintable(ran.value("error").toString()));
        QVERIFY(ran.value("result").isNull());
        ran = ScriptTools::run(bench.context(), "former", "broken", QJsonArray());
        QVERIFY(ran.value("error").toString().contains("Two fields are called \"a\""));

        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        ScriptPlugin *plugin = plugins.plugin("former");
        QList<QJsonValue> answers;
        const ScriptPlugin::Done heard = [&](const QJsonValue &value, const QString &) { answers << value; };
        plugins.attend(true);
        plugin->callFunction("start", {}, heard);
        QVariantMap question = plugins.question();
        QCOMPARE(question.value("title").toString(), QString("Start a story"));
        const QVariantMap form = question.value("form").toMap();
        QCOMPARE(form.value("accept").toString(), QString("Start"));
        QCOMPARE(form.value("fields").toList().size(), 3);
        // Not sent while what must be filled in is empty; then answered whole.
        QVERIFY(!plugins.fill(question.value("id").toInt(), {{"wish", "  "}}));
        QVERIFY(!plugins.question().isEmpty());
        QCOMPARE(plugins.formMissing(form, plugins.formValues(form, {})), QString("Wish"));
        QVERIFY(plugins.fill(question.value("id").toInt(), {{"wish", "a ship"}, {"chapters", 99}}));
        QTRY_COMPARE(answers.size(), 1);
        QCOMPARE(answers.first().toObject(), (QJsonObject{{"wish", "a ship"}, {"chapters", 60}, {"lush", false}}));
        // Left (Escape), it is no answer; words are no answer to a form.
        plugin->callFunction("start", {}, heard);
        plugins.answer(plugins.question().value("id").toInt(), "words");
        QTRY_COMPARE(answers.size(), 2);
        QVERIFY(answers.last().isNull());
        plugin->callFunction("start", {}, heard);
        plugins.dismiss(plugins.question().value("id").toInt());
        QTRY_COMPARE(answers.size(), 3);
        QVERIFY(answers.last().isNull());
        // The window going leaves it unanswered.
        plugin->callFunction("start", {}, heard);
        plugins.attend(false);
        QTRY_COMPARE(answers.size(), 4);
        QVERIFY(answers.last().isNull());
    }

    void aLauncherIsOnTheShelfAndNowhereElse() {
        Bench bench;
        const qint64 real = bench.addBook("A Real Book");
        Fixtures::write(bench.plugin("teller", R"js(
            var opened = 0;
            omabook.ui.launcher({id: "new", title: "Write a new story", author: "Teller", description: "Starts one.",
                                 open: function (context) { opened += 1; omabook.ui.set("opened", context.launcher + opened); },
                                 removed: function () { omabook.settings.set("hidden", true); }});
            omabook.ui.launcher({id: "pic", title: "With a picture", cover: "cover.png", open: function () {}});
            function broken(which) {
                if (which === "cover") omabook.ui.launcher({id: "x", title: "X", cover: "nope.png", open: function () {}});
                if (which === "id") omabook.ui.launcher({id: "Not Ok", title: "X", open: function () {}});
                if (which === "open") omabook.ui.launcher({id: "x", title: "X"});
            }
        )js", {"files"}) + "/cover.png", Fixtures::picture(20, 30, Qt::green));
        ScriptPlugins plugins(bench.services(), bench.folder, nullptr, false);
        QVariantList launchers = plugins.launchers();
        QCOMPARE(launchers.size(), 2);
        const QVariantMap launcher = launchers.first().toMap();
        const qint64 number = launcher.value("bookId").toLongLong();
        QVERIFY(number < 0);
        QCOMPARE(launcher.value("title").toString(), QString("Write a new story"));
        QCOMPARE(launcher.value("authors").toString(), QString("Teller"));
        QVERIFY(launchers.at(1).toMap().value("cover").toUrl().toLocalFile().endsWith("/teller/cover.png"));
        QCOMPARE(plugins.launchers().first().toMap().value("bookId").toLongLong(), number);
        for (const char *which : {"cover", "id", "open"}) {
            QString problem;
            plugins.plugin("teller")->callNow("broken", {which}, &problem);
            QVERIFY2(!problem.isEmpty(), which);
        }

        // On the shelf, first, found by its words, and in no count.
        BookListModel shelf;
        shelf.setBooks(bench.library.books(), bench.library.root());
        shelf.setLaunchers(plugins.launchers());
        QCOMPARE(shelf.count(), 3);
        QCOMPARE(shelf.total(), 1);
        QCOMPARE(shelf.shownBooks(), 1);
        QCOMPARE(shelf.idAt(0), number);
        QCOMPARE(shelf.get(0).value("launcher").toBool(), true);
        QCOMPARE(shelf.get(0).value("held").toString(), QString("launcher"));
        for (const QString &sort : BookListModel::sorts()) {
            shelf.setSort(sort);
            QCOMPARE(shelf.idAt(0), number);
        }
        shelf.setQuery("new story");
        QCOMPARE(shelf.count(), 1);
        shelf.setQuery("is:wishlist");
        QCOMPARE(shelf.rowOf(number), -1);
        shelf.setQuery("");
        shelf.setFilter("status", "unread");
        QCOMPARE(shelf.rowOf(number), -1);
        QCOMPARE(shelf.rowOf(real), 0);
        shelf.clearFilters();
        for (const QVariant &choice : shelf.choices("author"))
            QVERIFY(choice.toMap().value("value").toString() != "Teller");
        QCOMPARE(shelf.groups("series").first().toMap().value("launcher").toLongLong(), number);
        // Nothing of it is in the library: not its books, so not in what
        // is counted, suggested, graphed, tidied, synced or exported.
        QCOMPARE(bench.library.books().size(), 1);
        QVERIFY(!bench.library.book(number));

        // Opened, its plugin is called; taken off, its plugin is told.
        QVERIFY(plugins.launch(number));
        QTRY_COMPARE(plugins.data().value("teller").toMap().value("opened").toString(), QString("new1"));
        QVERIFY(plugins.removeLauncher(number));
        QTRY_COMPARE(plugins.launchers().size(), 1);
        QTRY_VERIFY(QFile::exists(bench.folder + "/teller/data/settings.json"));
        QVERIFY(!plugins.launch(number));
        // With the plugin switched off, it is gone; and with it removed.
        bench.settings.setPluginEnabled("folder:teller", false);
        plugins.reload();
        QVERIFY(plugins.launchers().isEmpty());
        shelf.setLaunchers(plugins.launchers());
        QCOMPARE(shelf.count(), 1);
    }

    void checkAndRunKnowALauncher() {
        Bench bench;
        bench.plugin("teller", R"js(
            omabook.ui.launcher({id: "new", title: "Write a new story", open: function () { return omabook.ui.form({title: "Start"}); }});
        )js");
        const QJsonObject checked = ScriptTools::check(bench.context(), "teller");
        QCOMPARE(checked.value("registered").toObject().value("launchers").toArray().first().toObject().value("title").toString(),
                 QString("Write a new story"));
        const QJsonObject ran = ScriptTools::run(bench.context(), "teller", "launcher:new", QJsonObject());
        QVERIFY2(!ScriptTools::failed(ran), qPrintable(ran.value("error").toString()));
        QVERIFY(ran.value("result").isNull());
        QVERIFY(ScriptTools::run(bench.context(), "teller", "launcher:none", QJsonObject()).value("error").toString().contains("no launcher"));
    }
};

QTEST_MAIN(ScriptingTests)
#include "scripting_tests.moc"
