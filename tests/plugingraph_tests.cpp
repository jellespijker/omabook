// What a plugin finds and keeps of its own books (#136): a plugin's book
// found by its words in the search, the book's own node in the graph, the
// graph searched in the tenant the plugin names, and a sighting it made
// taken away, here and on one's other computer. And a book's generated id
// in one shape (#128). The stand-in is a jotter, whose jottings are books
// of the format "jot", their words what its format's open says they hold.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>

#include "bookwords.h"
#include "changelog.h"
#include "fixtures.h"
#include "graph.h"
#include "knowledgesearch.h"
#include "library.h"
#include "remote.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "spacereducer.h"
#include "spacereplay.h"
#include "spaces.h"
#include "sync.h"
#include "tenants.h"

namespace {

const QByteArray manifest = R"({"name": "Jotter", "version": "1.0", "script": "main.js", "permissions": ["files"]})";

const QByteArray script = R"js(
omabook.format.register({
    name: "Jotting",
    extensions: ["jot"],
    open: function (source) { return {title: "A jotting", chapters: [{title: "Page", text: source.text(source.entries()[0])}]}; }
});
function jot(title, words) {
    omabook.files.write("data/" + title + ".jot", words);
    return omabook.library.create({title: title}, "data/" + title + ".jot").id;
}
function rewrite(id, title, words) {
    omabook.files.write("data/" + title + ".jot", words);
    return omabook.library.replaceFile(id, "data/" + title + ".jot").id;
}
function search(query) { return omabook.library.search(query).map(function (book) { return book.title; }); }
function book(id) { return omabook.library.get(id); }
function bookNode(id) { return omabook.graph.bookNode(id); }
function find(words, options) { return omabook.graph.find(words, options).map(function (node) { return node.name; }).sort(); }
function meet(name, id, section) { return omabook.graph.upsert({kind: "character", name: name, book: id, section: section, fraction: 0.5}); }
function unsee(id, book) { return omabook.graph.unsee(id, book); }
function met(id) { return omabook.graph.ofBook(id).nodes.filter(function (node) { return node.kind === "character"; }).map(function (node) { return node.name; }); }
function relate(source, target, relation) { return omabook.graph.link(source, target, relation); }
)js";

ScriptServices servicesOf(Library &library) {
    ScriptServices services;
    services.library = [&library] { return &library; };
    return services;
}

// The jotter, installed in `plugins`, running on `library`.
struct Jotter {
    Jotter(const QString &plugins, Library &library, bool follow = false) {
        QVERIFY(QDir().mkpath(plugins + "/jotter"));
        QVERIFY(Fixtures::write(plugins + "/jotter/plugin.json", manifest));
        QVERIFY(Fixtures::write(plugins + "/jotter/main.js", script));
        set = std::make_unique<ScriptPlugins>(servicesOf(library), plugins, nullptr, follow);
        set->provideFormats();
        QVERIFY(set->plugin("jotter") != nullptr);
    }

    QJsonValue call(const QString &function, const QJsonArray &arguments = {}) {
        QString problem;
        const QJsonValue answer = set->plugin("jotter")->callNow(function, arguments, &problem);
        last = problem;
        return answer;
    }
    QStringList strings(const QString &function, const QJsonArray &arguments = {}) {
        return call(function, arguments).toVariant().toStringList();
    }

    std::unique_ptr<ScriptPlugins> set;
    QString last; // why the last call failed
};

// The titles the shelf's search finds (KnowledgeSearch, as the window and `omabook search` ask it).
[[nodiscard]] QStringList found(const Library &library, const QString &query) {
    QStringList titles;
    for (const KnowledgeSearch::Found &one : KnowledgeSearch::search(library, library.books(), query))
        titles << one.book.meta.title;
    titles.sort();
    return titles;
}

// What the log, all of it merged, says of where a thing was met: by book (global id), the section.
[[nodiscard]] QMap<QString, int> sightingsInTheLog(const Library &library, qint64 node, bool backwards = false) {
    QList<SpaceOperation> ops = SpaceReplay::operations(library);
    if (backwards)
        std::ranges::reverse(ops);
    QMap<QString, int> sections;
    const ReducedNode reduced = SpaceReducer::reduce(ops).nodes.value(Spaces::nodeId(library, node));
    for (auto seen = reduced.seen.cbegin(); seen != reduced.seen.cend(); ++seen)
        sections.insert(seen.key(), seen->place.section);
    return sections;
}

} // namespace

class PluginGraphTests : public QObject {
    Q_OBJECT

private slots:
    // A jotting's words, as its format's open says them, are what the
    // search finds it by: the shelf's, `omabook search` and the plugin's
    // own; read again when the plugin writes the file anew, and never in
    // the log, so they go nowhere the book doesn't.
    void aPluginsBookIsFoundByItsWords() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("person.epub"), "A Person's Book"));
        QVERIFY(library.add(dir.filePath("person.epub")).error.isEmpty());
        Jotter jotter(dir.filePath("plugins"), library);

        const qint64 id = jotter.call("jot", {"Monday", "The lighthouse keeper rowed out at dawn."}).toInteger();
        QVERIFY2(id > 0, qPrintable(jotter.last));
        // Read once the script is done.
        QTRY_COMPARE(found(library, "lighthouse"), QStringList{"Monday"});
        QCOMPARE(found(library, "LIGHT"), QStringList{"Monday"});            // by a word's beginning, case aside
        QCOMPARE(found(library, "\"keeper rowed\""), QStringList{"Monday"}); // words together
        QCOMPARE(found(library, "lighthouse -dawn"), QStringList());
        QCOMPARE(jotter.strings("search", {"rowed"}), QStringList{"Monday"});
        QCOMPARE(found(library, "person"), QStringList{"A Person's Book"});

        QCOMPARE(jotter.call("rewrite", {id, "Monday", "A storm took the harbour wall."}).toInteger(), id);
        QTRY_COMPARE(found(library, "harbour"), QStringList{"Monday"});
        QCOMPARE(found(library, "lighthouse"), QStringList());
        QVERIFY(BookWords::stale(library, {"jot"}).isEmpty());

        const QList<SpaceOperation> ops = SpaceReplay::operations(library);
        QVERIFY(std::ranges::none_of(ops, [](const SpaceOperation &op) { return op.value.contains("harbour"); }));
        // And they go with the book.
        QVERIFY(library.remove(id).has_value());
        QVERIFY(BookWords::books(library, {"harbour"}).isEmpty());
    }

    // A jotting that came without its words (from another computer, or from
    // before) has them read when the plugin loads.
    void theWordsOfABookThatCameAreReadWhenThePluginLoads() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        qint64 id = 0;
        {
            Jotter first(dir.filePath("plugins"), library);
            id = first.call("jot", {"Tuesday", "Gulls over the quay."}).toInteger();
            QVERIFY2(id > 0, qPrintable(first.last));
        }
        QVERIFY(ChangeLog::exec(library, "DELETE FROM book_texts"));
        QVERIFY(found(library, "gulls").isEmpty());
        QCOMPARE(BookWords::stale(library, {"jot"}), QList<qint64>{id});

        Jotter again(dir.filePath("plugins"), library, true);
        again.set->libraryChanged(library.books(), library.root());
        QTRY_COMPARE(found(library, "gulls"), QStringList{"Tuesday"});
    }

    // A book's own node, for a plugin to relate its things to the book.
    void aBookHasANodeOfItsOwn() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "The Ash and the Mists"));
        const qint64 book = library.add(dir.filePath("book.epub")).id;
        Jotter jotter(dir.filePath("plugins"), library);

        const qint64 node = jotter.call("bookNode", {book}).toInteger();
        QVERIFY2(node > 0, qPrintable(jotter.last));
        QCOMPARE(jotter.call("bookNode", {book}).toInteger(), node);
        QCOMPARE(Graph(library).bookNode(book), node);
        QCOMPARE(Graph(library).node(node)->kind, QString("book"));
        const qint64 mara = jotter.call("meet", {"Mara", book, 0}).toInteger();
        QVERIFY(jotter.call("relate", {mara, node, "appears in"}).toInteger() > 0);
        const QList<GraphEdge> edges = Graph(library).around(node).edges;
        QVERIFY(std::ranges::any_of(edges, [&](const GraphEdge &edge) { return edge.source == mara && edge.kind == "appears in"; }));

        jotter.call("bookNode", {9999});
        QVERIFY2(jotter.last.contains("There is no book 9999"), qPrintable(jotter.last));
    }

    // The general graph searched as the readers of a top-level collection
    // see it: the one named, else that of the book named.
    void findKeepsToTheTenantItIsGiven() {
        Split split;
        QTemporaryDir dir;
        Jotter jotter(dir.filePath("plugins"), split.library);

        QCOMPARE(jotter.strings("find", {"Mara", QJsonObject{{"tenant", "Fiction"}}}), QStringList{"Mara"});
        QCOMPARE(jotter.strings("find", {"Mara", QJsonObject{{"tenant", "Study"}}}), QStringList());
        QCOMPARE(jotter.strings("find", {"Viscosity", QJsonObject{{"book", split.waves}}}), QStringList{"Viscosity"});
        QCOMPARE(jotter.strings("find", {"Viscosity", QJsonObject{{"book", split.sea}}}), QStringList());
        // The one named wins over the book's.
        QCOMPARE(jotter.strings("find", {"Viscosity", QJsonObject{{"book", split.sea}, {"tenant", "Study"}}}), QStringList{"Viscosity"});
        QVERIFY(jotter.last.isEmpty());

        jotter.call("find", {"Mara", QJsonObject{{"tenant", "Nowhere"}}});
        QVERIFY2(jotter.last.contains("no collection called Nowhere"), qPrintable(jotter.last));
    }

    // A sighting a plugin made, taken away: the thing stays, a reader of the
    // book no longer meets it there, and the log says so whatever the order
    // its operations are read in. Met there again, it is back.
    void aSightingIsTakenAwayAndComesBack() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "The Ash and the Mists"));
        const qint64 book = library.add(dir.filePath("book.epub")).id;
        const QString bookGid = Spaces::bookId(library, book);
        Jotter jotter(dir.filePath("plugins"), library);

        const qint64 mara = jotter.call("meet", {"Mara", book, 1}).toInteger();
        QVERIFY2(mara > 0, qPrintable(jotter.last));
        QCOMPARE(jotter.strings("met", {book}), QStringList{"Mara"});
        QCOMPARE(sightingsInTheLog(library, mara).value(bookGid, -1), 1);

        QVERIFY(jotter.call("unsee", {mara, book}).toBool());
        QCOMPARE(jotter.strings("met", {book}), QStringList());
        QVERIFY(Graph(library).node(mara));
        QVERIFY(!jotter.call("unsee", {mara, book}).toBool());
        QVERIFY(!sightingsInTheLog(library, mara).contains(bookGid));
        QVERIFY(!sightingsInTheLog(library, mara, true).contains(bookGid));

        // Met again, later in the book: there again, at the new place.
        QCOMPARE(jotter.call("meet", {"Mara", book, 3}).toInteger(), mara);
        QCOMPARE(jotter.strings("met", {book}), QStringList{"Mara"});
        QCOMPARE(sightingsInTheLog(library, mara).value(bookGid, -1), 3);
        QCOMPARE(sightingsInTheLog(library, mara, true).value(bookGid, -1), 3);
    }

    // And on one's other computer: a sighting taken away here is gone there
    // after a sync, and one made again comes back.
    void aSightingTakenAwayGoesToOnesOtherComputer() {
        QTemporaryDir dir;
        Library here(dir.filePath("Here")), there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "The Ash and the Mists")
                && Fixtures::epub(dir.filePath("other.epub"), "Only There"));
        there.add(dir.filePath("other.epub"));
        const qint64 bookHere = here.add(dir.filePath("book.epub")).id, bookThere = there.add(dir.filePath("book.epub")).id;
        const Remote remote("laptop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
        const auto sync = [&] {
            const SyncSummary summary = syncLibrary(here, remote);
            return summary.error.isEmpty() && summary.problems.isEmpty() ? QString() : summary.error + summary.problems.join('\n');
        };
        const auto metThere = [&]() -> std::optional<int> {
            const Graph graph(there);
            const auto at = graph.firstMet(graph.named({}, "character", "Kelsier"), bookThere);
            return at ? std::optional(at->place.section) : std::nullopt;
        };

        Graph graph(here);
        const qint64 kelsier = graph.upsert({}, "character", "Kelsier", "A thief with a plan.", {}, GraphPlace{bookHere, 1, 0.2});
        QCOMPARE(sync(), QString());
        QCOMPARE(metThere(), std::optional(1));

        QVERIFY(Graph::ofReader(here, bookHere).unsee(kelsier, bookHere));
        QCOMPARE(sync(), QString());
        QVERIFY(Graph(there).named({}, "character", "Kelsier") > 0);
        QCOMPARE(metThere(), std::nullopt);
        // Nothing said again at the next sync.
        QCOMPARE(sync(), QString());
        QCOMPARE(metThere(), std::nullopt);
        QVERIFY(!Graph(here).firstMet(kelsier, bookHere));

        QCOMPARE(graph.upsert({}, "character", "Kelsier", {}, {}, GraphPlace{bookHere, 2, 0.5}), kelsier);
        QCOMPARE(sync(), QString());
        QCOMPARE(metThere(), std::optional(2));
    }

    // A book's generated id has one shape (#128): what the command line
    // prints (Book::toJson) and what plugins are told say it alike, at the
    // top and, as before, among the identifiers.
    void aBooksGeneratedIdHasOneShape() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        Jotter jotter(dir.filePath("plugins"), library);
        const qint64 id = jotter.call("jot", {"Wednesday", "Fog."}).toInteger();
        QVERIFY2(id > 0, qPrintable(jotter.last));
        const Book made = *library.book(id);
        for (const QJsonObject &book : {made.toJson(), ScriptPlugins::told(made), jotter.call("book", {id}).toObject()}) {
            QCOMPARE(book.value("generated").toString(), QString("jotter"));
            QCOMPARE(book.value("identifiers").toObject().value("generated").toString(), QString("jotter"));
        }
        QVERIFY(Fixtures::epub(dir.filePath("person.epub"), "A Person's Book"));
        const QJsonObject person = library.book(library.add(dir.filePath("person.epub")).id)->toJson();
        QCOMPARE(person.value("generated"), QJsonValue(""));
        QVERIFY(person.value("identifiers").isObject());
    }
};

QTEST_MAIN(PluginGraphTests)
#include "plugingraph_tests.moc"
