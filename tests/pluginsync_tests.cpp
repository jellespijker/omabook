// Books in a plugin's format on another computer: the plugin's own there too
// (#28), through the change log and with an Omabook from before it.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "fixtures.h"
#include "library.h"
#include "pluginclaims.h"
#include "remote.h"
#include "scriptplugins.h"
#include "sync.h"

namespace {

// A plugin that reads stories (.story, a JSON file) and hears which books arrive.
constexpr const char *teller = R"(
omabook.format.register({
    name: "Told story",
    extensions: ["story"],
    open: function (source) {
        const story = JSON.parse(source.text(source.entries()[0]));
        return {title: story.title, authors: ["Tess Teller"], chapters: [{title: "One", text: story.text}]};
    }
});
var arrived = [];
omabook.on("book.added", function (event) { arrived.push(event.book.title + ": " + event.book.generated); });
function heard() { return arrived; }
)";

// A plugin that says the EPUB is its format, beside one of its own.
constexpr const char *grabby = R"(
omabook.format.register({name: "Grabbed", extensions: ["epub", "grab"], open: function () { return {title: "Mine", chapters: []}; }});
)";

void writePlugin(const QString &folder, const QString &name, const QByteArray &script) {
    QVERIFY(QDir().mkpath(folder + "/" + name));
    const QJsonObject manifest{{"name", name}, {"version", "1.0"}, {"description", "A test plugin."}, {"script", "main.js"}};
    QVERIFY(Fixtures::write(folder + "/" + name + "/plugin.json", QJsonDocument(manifest).toJson()));
    QVERIFY(Fixtures::write(folder + "/" + name + "/main.js", script));
}

QString story(const QTemporaryDir &dir, const QString &title) {
    const QString file = dir.filePath(title + ".story");
    Fixtures::write(file, QJsonDocument(QJsonObject{{"title", title}, {"text", "Once upon a time."}}).toJson());
    return file;
}

// The other computer behind a stand-in ssh; as an `older` Omabook it refuses
// the options one from before the log doesn't know (exit code 1).
Remote otherComputer(const QTemporaryDir &dir, const QString &library, bool older) {
    const QByteArray standIn = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY).toUtf8();
    const QByteArray refuse = older ? "case \"$*\" in\n"
                                      "  *\"'--log'\"*) echo \"Unknown option 'log'.\" >&2; exit 1 ;;\n"
                                      "  *\"'--replicate'\"*) echo \"Unknown option 'replicate'.\" >&2; exit 1 ;;\n"
                                      "esac\n"
                                    : "";
    return Remote("laptop:" + library,
                  {Fixtures::script(dir.filePath(older ? "ssh-older" : "ssh-current"), refuse + "exec '" + standIn + "' \"$@\"\n")});
}

ScriptServices servicesOf(Library &library) {
    ScriptServices services;
    services.library = [&library] { return &library; };
    return services;
}

} // namespace

class PluginSyncTests : public QObject {
    Q_OBJECT

private slots:
    // A story the plugin made on one computer comes to the other without the
    // plugin's mark, by the log and from an older Omabook alike: a sync never
    // carries it. The plugin there calls it its own again when the book
    // arrives (what book.added says of it says so), or when it loads.
    void aSyncedStoryIsItsPluginsOwnAgain() {
        for (const bool older : {false, true}) {
            QTemporaryDir dir;
            const QString folder = dir.filePath("plugins");
            writePlugin(folder, "teller", teller);
            Library here(dir.filePath("Here"));
            QVERIFY(here.open());
            ScriptPlugins plugins(servicesOf(here), folder);
            plugins.provideFormats();
            plugins.libraryChanged(here.books(), here.root());
            QVERIFY(plugins.wait());
            {
                Library there(dir.filePath("There"));
                QVERIFY(there.open());
                const Library::Added made = there.add(story(dir, "The Eaten Word"));
                QVERIFY2(made.error.isEmpty(), qPrintable(made.error));
                QVERIFY(there.setIdentifier(made.id, "generated", "teller"));
                QVERIFY(there.setIdentifier(made.id, "teller", "s1"));
            }

            const SyncSummary summary = syncLibrary(here, otherComputer(dir, dir.filePath("There"), older));
            QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(), qPrintable(summary.error + summary.problems.join('\n')));
            const QList<Book> arrived = here.books("eaten word");
            QCOMPARE(arrived.size(), 1);
            QCOMPARE(arrived.first().identifiers.value("teller").toString(), QString("s1"));
            QVERIFY(!arrived.first().identifiers.contains("generated"));

            if (older) {
                // As the plugin loads: Omabook started after the sync.
                const ScriptPlugins loaded(servicesOf(here), folder);
                QCOMPARE(here.book(arrived.first().id)->identifiers.value("generated").toString(), QString("teller"));
                continue;
            }
            // As the book arrives, and what book.added says of it says so.
            plugins.libraryChanged(here.books(), here.root());
            QVERIFY(plugins.wait());
            QCOMPARE(here.book(arrived.first().id)->identifiers.value("generated").toString(), QString("teller"));
            QString problem;
            QCOMPARE(plugins.plugin("teller")->callNow("heard", {}, &problem).toArray(), QJsonArray{"The Eaten Word: teller"});
        }
    }

    // A person's EPUB is no plugin's, though a plugin says EPUB is its format;
    // nor is a book with a file in another format, nor one two plugins read.
    void aPluginNeverClaimsAPersonsBook() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("plugins");
        writePlugin(folder, "grabby", grabby);
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("person.epub"), "By A Person"));
        const qint64 person = library.add(dir.filePath("person.epub")).id;
        QVERIFY(person > 0);
        ScriptPlugins plugins(servicesOf(library), folder);
        plugins.provideFormats();
        plugins.libraryChanged(library.books(), library.root());
        QVERIFY(!library.book(person)->identifiers.contains("generated"));
        // One that arrives later is refused too.
        QVERIFY(Fixtures::epub(dir.filePath("later.epub"), "Later By A Person"));
        const qint64 later = library.add(dir.filePath("later.epub")).id;
        plugins.libraryChanged(library.books(), library.root());
        QVERIFY(!library.book(later)->identifiers.contains("generated"));

        const PluginClaims::Registered registered{{"grabby", {"epub", "grab"}}, {"teller", {"story"}}};
        QCOMPARE(PluginClaims::owner({"grab"}, registered), QString("grabby"));
        QCOMPARE(PluginClaims::owner({"story"}, registered), QString("teller"));
        QCOMPARE(PluginClaims::owner({"epub"}, registered), QString());
        QCOMPARE(PluginClaims::owner({"grab", "epub"}, registered), QString());
        QCOMPARE(PluginClaims::owner({"story", "grab"}, registered), QString());
        QCOMPARE(PluginClaims::owner({}, registered), QString());
        QCOMPARE(PluginClaims::owner({"story"}, {{"teller", {"story"}}, {"rival", {"story"}}}), QString());
    }
};

QTEST_MAIN(PluginSyncTests)
#include "pluginsync_tests.moc"
