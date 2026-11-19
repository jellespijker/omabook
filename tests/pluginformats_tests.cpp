// A book in the format of a plugin that isn't installed: kept, and saying
// which plugin it needs, on its way to another computer as well (#25).

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "booklistmodel.h"
#include "fixtures.h"
#include "formatprovider.h"
#include "formats.h"
#include "graph.h"
#include "library.h"
#include "pluginformats.h"
#include "remote.h"
#include "sync.h"

namespace {

// A story as Storyteller writes it: one JSON file.
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

// Storyteller's format, as it registers it where it is installed.
FormatProvider storyteller() {
    return {
        .id = "plugin:storyteller/Storyteller story", .name = "Storyteller story", .extensions = {"story"}, .claims = {}, .describe = {}};
}

// A story made in `library` on a computer with Storyteller, with a thing of
// its world met in it.
qint64 storyWithItsWorld(const QTemporaryDir &dir, Library &library) {
    Formats::addProvider(storyteller());
    const Library::Added made = library.add(story(dir, "The Eaten Word"));
    Formats::removeProvider(storyteller().id);
    if (!made.error.isEmpty())
        return 0;
    library.setIdentifier(made.id, "storyteller", "s1");
    Graph(library).upsert("world:s1", "character", "Mara Venn", "Eats words.", {}, GraphPlace{made.id, 0, 0.5}, "Mara");
    return made.id;
}

// Whether the story is in `library` as it was made, and with `withWorld` its
// world with it, what is said of its people included.
void keptWithItsWorld(Library &library, bool withWorld) {
    const QList<Book> kept = library.books("eaten word");
    QCOMPARE(kept.size(), 1);
    QCOMPARE(kept.first().formats, QStringList{"story"});
    QCOMPARE(kept.first().identifiers.value("storyteller").toString(), QString("s1"));
    QCOMPARE(Formats::neededPlugin(kept.first().formats)->title, QString("Storyteller"));
    const Graph graph(library);
    if (!withWorld)
        return;
    const qint64 mara = graph.named("world:s1", "character", "Mara Venn");
    QVERIFY(mara > 0);
    QCOMPARE(graph.node(mara)->summary, QString("Eats words."));
    const GraphView world = graph.ofBook(kept.first().id, std::nullopt, "world:s1");
    QVERIFY(std::ranges::any_of(world.nodes, [](const GraphNode &node) { return node.name == "Mara Venn"; }));
}

} // namespace

class PluginFormatsTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { PluginFormats::know(); }

    // Where Storyteller isn't installed, a story is a book all the same: it
    // is added, says it needs Storyteller (and where it is installed from),
    // and opening it says how to install it.
    void aStoryIsKeptWhereItsPluginIsNot() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const Library::Added added = library.add(story(dir, "Kept"));
        QVERIFY2(added.error.isEmpty(), qPrintable(added.error));
        const Book book = *library.book(added.id);
        QCOMPARE(Formats::name("story"), QString("Storyteller story"));
        const QVariantMap shown = bookToMap(book, library.root());
        QCOMPARE(shown.value("needsPlugin").toString(), QString("Storyteller"));
        QCOMPARE(shown.value("pluginAddress").toString(), QString("jellespijker/omabook-storyteller"));
        QCOMPARE(shown.value("formatNames").toString(), QString("Storyteller story"));
        QString error;
        QVERIFY(!Formats::open(library.formatPath(added.id), &error));
        QVERIFY2(error.contains("Storyteller plugin") && error.contains("omabook plugin add storyteller"), qPrintable(error));

        // Once the plugin reads it, it needs nothing.
        Formats::addProvider(storyteller());
        QVERIFY(!Formats::neededPlugin({"story"}));
        QCOMPARE(bookToMap(book, library.root()).value("needsPlugin").toString(), QString());
        Formats::removeProvider(storyteller().id);
        // A book with a file Omabook reads needs no plugin; one with none, nothing at all.
        QVERIFY(!Formats::neededPlugin({"story", "epub"}));
        QVERIFY(!Formats::neededPlugin({}));
        // And a registry can't make one of Omabook's own formats a plugin's.
        Formats::setPluginFormats({{.extension = "epub", .name = "Grabbed", .plugin = "grabby", .title = "Grabby", .address = "a/b"}});
        QVERIFY(!Formats::pluginFormat("epub"));
        QVERIFY(!Formats::pluginFormat("story"));
        PluginFormats::know();
        QVERIFY(Formats::pluginFormat("story"));
    }

    // A story syncs to a computer without Storyteller, and from one, by the
    // log and with an Omabook from before it alike: it is kept there instead
    // of failing as a format Omabook doesn't know. Through the log its world
    // (the graph's world scope) goes with it; an Omabook from before the log
    // never sends or takes a world, so by the manifest it stays behind.
    void aStorySyncsWhereItsPluginIsNot() {
        for (const bool older : {false, true}) {
            for (const bool sending : {true, false}) {
                QTemporaryDir dir;
                Library here(dir.filePath("Here"));
                QVERIFY(here.open());
                {
                    Library there(dir.filePath("There"));
                    QVERIFY(there.open());
                    QVERIFY(storyWithItsWorld(dir, sending ? here : there) > 0);
                }
                const SyncSummary summary = syncLibrary(here, otherComputer(dir, dir.filePath("There"), older));
                const QString said = summary.error + summary.problems.join('\n');
                QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(),
                         qPrintable(QString("older %1, sending %2: %3").arg(older).arg(sending).arg(said)));
                QCOMPARE(sending ? summary.sent : summary.received, 1);
                Library there(dir.filePath("There"));
                QVERIFY(there.open());
                keptWithItsWorld(sending ? there : here, !older);
                if (QTest::currentTestFailed())
                    return;
            }
        }
    }
};

QTEST_MAIN(PluginFormatsTests)
#include "pluginformats_tests.moc"
