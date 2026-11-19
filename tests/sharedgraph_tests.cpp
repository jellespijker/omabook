// A graph someone shared follows the receiver's own reading place (#5): of a
// subscribed space's graph, a book shows what was met or learned no further
// than where its receiver is, everywhere the receiver's own graph is shown or
// told: the graph's window, the marks on the page and their cards, a search
// by what is known, the command line, the MCP server and what the AI is told.
// An unbegun book shows nothing of it; reading on shows more; unsubscribing
// takes it away. It stays apart from the receiver's own graph, and says whose
// it is.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

#include "backend.h"
#include "filepicker.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcontext.h"
#include "graphmodel.h"
#include "graphtools.h"
#include "knowledgesearch.h"
#include "library.h"
#include "person.h"
#include "publishedspace.h"
#include "reader.h"
#include "readingmarks.h"
#include "settings.h"
#include "subscriptions.h"

namespace {

// Of ten chapters: the receiver at the third is a little under a third in.
[[nodiscard]] bool tenChapters(const QString &path) {
    QList<Fixtures::Chapter> chapters;
    for (int at = 1; at <= 10; ++at)
        chapters << Fixtures::Chapter{QString("Chapter %1").arg(at), QString("<p>The ferry crossed again, the %1th time.</p>").arg(at)};
    return Fixtures::chapteredEpub(path, "The Long Tide", chapters);
}

[[nodiscard]] SpaceSnapshot::Owner ada() {
    static const auto person = Person::generate();
    if (!person)
        qFatal("%s", qPrintable(person.error()));
    return {.name = "Ada Fixture", .person = person->id()};
}

// Someone who read The Long Tide to its end and shares it, graph and all:
// Mara and Glass Harbour early on, Wick and who Mara really is near the end.
struct Owner {
    Owner() : library(dir.filePath("Owner")) {
        QVERIFY(library.open());
        QVERIFY(tenChapters(file()));
        book = library.add(file()).id;
        BookMeta hers;
        hers.title = "Only Hers";
        hers.authors = {"Ann Other"};
        unowned = library.addEntry(hers, false).id;
        QVERIFY(book > 0 && unowned > 0);
        library.setProgress(book, {9, 1.0, 100});
        library.setProgress(unowned, {9, 1.0, 100});
        const auto made = library.addCollection("Tides");
        QVERIFY(made.has_value());
        shelf = *made;
        QVERIFY(library.putInCollection(shelf, book, true) && library.putInCollection(shelf, unowned, true));
        Graph graph(library);
        mara = graph.upsert({}, "character", "Mara", "A ferry pilot.", {}, GraphPlace{book, 0, 0.1}, "Mara");
        const qint64 harbour = graph.upsert({}, "place", "Glass Harbour", "Where the ferries berth.", {}, GraphPlace{book, 1, 0.5});
        const qint64 wick = graph.upsert({}, "character", "Wick", "A lamplighter turned informer.", {}, GraphPlace{book, 7, 0.5});
        graph.learn(mara, "summary", "Secretly the queen in exile.", GraphMoment{{book, 8, 0.5}, "the queen"});
        graph.learn(mara, "alias", "The Queen", GraphMoment{{book, 8, 0.5}, "the queen"});
        graph.link(mara, harbour, "lives in", {}, GraphPlace{book, 1, 0.5});
        graph.link(wick, mara, "betrays", {}, GraphPlace{book, 9, 0.9});
        graph.upsert({}, "character", "Hers Only", "Of the book the receiver lacks.", {}, GraphPlace{unowned, 0, 0.1});
    }

    [[nodiscard]] QString file() const { return dir.filePath("The Long Tide.epub"); }

    [[nodiscard]] SpaceSnapshot snapshot() const {
        const auto taken = PublishedSpace::snapshot(library, shelf, {.graph = true}, ada());
        if (!taken)
            qFatal("%s", qPrintable(taken.error()));
        return *taken;
    }

    QTemporaryDir dir;
    Library library;
    qint64 book = 0, unowned = 0, shelf = 0, mara = 0;
};

// The receiver: the same book (the same file), and a graph of their own of it.
struct Receiver {
    explicit Receiver(const Owner &owner, const QString &root) : library(root) {
        QVERIFY(library.open());
        QVERIFY(QFile::copy(owner.file(), QFileInfo(root).dir().filePath("tide.epub")));
        book = library.add(QFileInfo(root).dir().filePath("tide.epub")).id;
        QVERIFY(book > 0);
        own = Graph(library).upsert({}, "character", "Mara", "My note: she rows at night.", {}, GraphPlace{book, 0, 0.2});
        const auto subscribed = Subscriptions::subscribe(library, owner.snapshot(), {});
        QVERIFY2(subscribed.has_value(), qPrintable(subscribed ? QString() : subscribed.error()));
        space = subscribed->space.space;
    }

    void readTo(int section, double fraction, double percent) { library.setProgress(book, {section, fraction, percent}); }

    Library library;
    qint64 book = 0, own = 0;
    QString space;
};

[[nodiscard]] QStringList names(const QList<GraphNode> &nodes) {
    QStringList list;
    for (const GraphNode &node : nodes)
        list << node.name;
    list.sort();
    return list;
}

[[nodiscard]] QStringList sharedNames(const Library &library, qint64 book) {
    QList<GraphNode> nodes;
    for (const GraphLayers::Layer &layer : GraphContext::layers(library))
        nodes += layer.ofBook(book).nodes;
    return names(nodes);
}

// `omabook <arguments> --library <root>`, to its end: what it printed.
[[nodiscard]] QString run(const QString &root, QStringList arguments) {
    QProcess process;
    process.start(OMABOOK_BINARY, arguments << "--library" << root);
    process.closeWriteChannel();
    process.waitForFinished(60000);
    return QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
}

// One tool of `omabook mcp` called, as an agent calls it: what it answered.
[[nodiscard]] QJsonObject tool(const QString &root, const QString &name, const QJsonObject &arguments) {
    QProcess server;
    server.start(OMABOOK_BINARY, {"mcp", "--library", root});
    const QJsonObject request{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}};
    server.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
    while (!server.canReadLine())
        if (!server.waitForReadyRead(60000))
            break;
    const QJsonObject answer = QJsonDocument::fromJson(server.readLine()).object();
    server.closeWriteChannel();
    if (!server.waitForFinished(5000))
        server.kill();
    return answer.value("result").toObject().value("structuredContent").toObject();
}

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// The window's side: a backend on the receiver's library, and its reader.
struct Desk {
    QTemporaryDir dir;
    NoPicker picker;
    std::optional<Settings> settings;
    std::optional<Backend> backend;
    std::optional<Reader> reader;

    Desk() {
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        settings.emplace(dir.filePath("config.json"));
        settings->setLibraryPath(dir.filePath("Library"));
        backend.emplace(
            &*settings, &picker, nullptr, [](QString *) { return std::unique_ptr<AiProvider>(); }, Backend::Catalogs{},
            dir.filePath("plugins"), "-");
        reader.emplace(&*backend, QString());
    }
};

} // namespace

class SharedGraphTests : public QObject {
    Q_OBJECT

private slots:
    void aSharedGraphIsCutAtTheReceiversPlace() {
        const Owner owner;
        QTemporaryDir dir;
        Receiver receiver(owner, dir.filePath("Receiver"));
        receiver.readTo(2, 0.5, 30);
        const QList<GraphLayers::Layer> layers = GraphContext::layers(receiver.library);
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers.first().owner, QString("Ada Fixture"));
        const GraphView view = layers.first().ofBook(receiver.book);
        QCOMPARE(names(view.nodes), QStringList({"Glass Harbour", "Mara"}));
        const auto mara = std::ranges::find(view.nodes, QString("Mara"), &GraphNode::name);
        // Known as it was by then: not yet the queen, by name or by what is said.
        QCOMPARE(mara->summary, QString("A ferry pilot."));
        QVERIFY(mara->aliases().isEmpty());
        QVERIFY(mara->id < 0 && mara->scope == receiver.space);
        QCOMPARE(GraphLayers::from(*mara), QString("Ada Fixture's shared graph"));
        QCOMPARE(view.edges.size(), 1);
        QCOMPARE(view.edges.first().kind, QString("lives in"));
        // A place asked for further on gives no more than the receiver read.
        QCOMPARE(names(GraphContext::layers(receiver.library, GraphPlace{receiver.book, 9, 1.0}).first().ofBook(receiver.book).nodes),
                 QStringList({"Glass Harbour", "Mara"}));
        QCOMPARE(names(GraphContext::layers(receiver.library, GraphPlace{receiver.book, 0, 0.5}).first().ofBook(receiver.book).nodes),
                 QStringList({"Mara"}));
        // Never merged into the receiver's own graph, which is as it was.
        const Graph graph(receiver.library);
        QCOMPARE(names(graph.ofBook(receiver.book).nodes), QStringList({"Mara"}));
        QCOMPARE(graph.node(receiver.own)->summary, QString("My note: she rows at night."));
        QVERIFY(graph.find("Wick", {}, std::nullopt).isEmpty());
    }

    void anUnbegunBookOrAWishShowsNothing() {
        const Owner owner;
        QTemporaryDir dir;
        Receiver receiver(owner, dir.filePath("Receiver"));
        QVERIFY(sharedNames(receiver.library, receiver.book).isEmpty());
        // The book the receiver lacks is a wish, of no place: nothing of it either.
        const QList<Book> books = receiver.library.books();
        const auto wish = std::ranges::find(books, QString("Only Hers"), [](const Book &book) { return book.meta.title; });
        QVERIFY(wish != books.cend() && wish->formats.isEmpty());
        QVERIFY(sharedNames(receiver.library, wish->id).isEmpty());
        QVERIFY(KnowledgeIndex::build(receiver.library).find("hers").isEmpty());
        // Marked as read, all of it is known.
        receiver.readTo(9, 1.0, 100);
        QCOMPARE(sharedNames(receiver.library, receiver.book), QStringList({"Glass Harbour", "Mara", "Wick"}));
    }

    void readingOnRevealsMore() {
        const Owner owner;
        QTemporaryDir dir;
        Receiver receiver(owner, dir.filePath("Receiver"));
        receiver.readTo(2, 0.5, 30);
        QCOMPARE(sharedNames(receiver.library, receiver.book), QStringList({"Glass Harbour", "Mara"}));
        receiver.readTo(8, 0.6, 85);
        const GraphView view = GraphContext::layers(receiver.library).first().ofBook(receiver.book);
        QCOMPARE(names(view.nodes), QStringList({"Glass Harbour", "Mara", "Wick"}));
        const auto mara = std::ranges::find(view.nodes, QString("Mara"), &GraphNode::name);
        QCOMPARE(mara->summary, QString("Secretly the queen in exile."));
        QCOMPARE(mara->aliases(), QStringList{"The Queen"});
        // Wick's betrayal is learned at the very end, still ahead.
        QVERIFY(std::ranges::none_of(view.edges, [](const GraphEdge &edge) { return edge.kind == "betrays"; }));
    }

    void searchingByWhatIsKnownFindsOnlyWhatWasRead() {
        const Owner owner;
        QTemporaryDir dir;
        Receiver receiver(owner, dir.filePath("Receiver"));
        receiver.readTo(2, 0.5, 30);
        const KnowledgeIndex index = KnowledgeIndex::build(receiver.library);
        QVERIFY(index.find("harbour").contains(receiver.book));
        QCOMPARE(index.find("harbour").value(receiver.book).from, QString("Ada Fixture's shared graph"));
        QVERIFY(index.find("wick").isEmpty());
        QVERIFY(index.find("queen", true).isEmpty());
        // The receiver's own Mara is their own.
        QVERIFY(index.find("mara").value(receiver.book).from.isEmpty());
    }

    void theAiIsToldNoMoreThanTheReceiverRead() {
        const Owner owner;
        QTemporaryDir dir;
        Receiver receiver(owner, dir.filePath("Receiver"));
        receiver.readTo(2, 0.5, 30);
        const GraphPlace here{receiver.book, 2, 0.5};
        const QString asked = GraphContext::forQuestion(receiver.library, receiver.book, here, "Who is Mara, and what of Wick?");
        QVERIFY2(asked.contains("Ada Fixture's shared graph") && asked.contains("A ferry pilot.") && asked.contains("Glass Harbour"),
                 qPrintable(asked));
        QVERIFY2(asked.contains("My note: she rows at night."), qPrintable(asked));
        QVERIFY2(!asked.contains("Wick") && !asked.contains("queen", Qt::CaseInsensitive), qPrintable(asked));
        const QString cast = GraphContext::digest(receiver.library, receiver.book, here, {"character", "place"});
        QVERIFY2(cast.contains("Glass Harbour") && !cast.contains("Wick") && !cast.contains("queen", Qt::CaseInsensitive),
                 qPrintable(cast));
        const QString library = GraphContext::picture(receiver.library, "harbour wick queen");
        QVERIFY2(library.contains("Glass Harbour") && !library.contains("Wick") && !library.contains("queen", Qt::CaseInsensitive),
                 qPrintable(library));
        // A book not begun tells the AI nothing of the shared graph.
        receiver.readTo(0, 0, 0);
        QVERIFY(!GraphContext::forQuestion(receiver.library, receiver.book, here, "Glass Harbour").contains("shared"));
    }

    void theCommandLineAndTheMcpServerShowNoMore() {
        const Owner owner;
        QTemporaryDir dir;
        const QString root = dir.filePath("Receiver");
        qint64 book = 0;
        {
            Receiver receiver(owner, root);
            receiver.readTo(2, 0.5, 30);
            book = receiver.book;
        }
        const QString id = QString::number(book);
        const QString shown = run(root, {"graph", "show", id, "--upto", "9"});
        QVERIFY2(shown.contains("Ada Fixture's shared graph") && shown.contains("Glass Harbour") && !shown.contains("Wick")
                     && !shown.contains("queen", Qt::CaseInsensitive),
                 qPrintable(shown));
        const QJsonObject answer = QJsonDocument::fromJson(run(root, {"graph", "show", id, "--json"}).toUtf8()).object();
        const QJsonArray layers = answer.value("layers").toArray();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers.first().toObject().value("owner").toString(), QString("Ada Fixture"));
        QCOMPARE(layers.first().toObject().value("nodes").toArray().size(), 2);
        const QJsonObject mcp = tool(root, "graph_book", {{"id", book}});
        const QString told = QJsonDocument(mcp).toJson();
        QVERIFY2(told.contains("Glass Harbour") && !told.contains("Wick") && !told.contains("queen", Qt::CaseInsensitive),
                 qPrintable(told));
        // Unsubscribing takes it away everywhere.
        QVERIFY(run(root, {"collections", "unsubscribe", "Tides"}).contains("Unsubscribed"));
        const QString after = run(root, {"graph", "show", id, "--json"});
        QVERIFY2(!after.contains("layers") && !after.contains("Glass Harbour"), qPrintable(after));
        QVERIFY(!QJsonDocument(tool(root, "graph_book", {{"id", book}})).toJson().contains("Glass Harbour"));
        Library library(root);
        QVERIFY(library.open());
        QVERIFY(GraphContext::layers(library).isEmpty());
        QVERIFY(KnowledgeIndex::build(library).find("harbour").isEmpty());
        QVERIFY(!GraphContext::forQuestion(library, book, {book, 9, 1.0}, "Glass Harbour").contains("shared"));
    }

    void theWindowsGraphAndCardsShowNoMore() {
        const Owner owner;
        Desk desk;
        Receiver receiver(owner, desk.dir.filePath("Receiver"));
        receiver.readTo(2, 0.5, 30);
        GraphBrowser browser;
        browser.setLibrary(&receiver.library);
        browser.showBook(receiver.book);
        QStringList rows;
        qint64 shared = 0;
        for (const QVariant &row : browser.rows()) {
            rows << row.toMap().value("name").toString();
            if (row.toMap().value("from").toString() == "Ada Fixture's shared graph" && row.toMap().value("name") == "Mara")
                shared = row.toMap().value("id").toLongLong();
        }
        rows.sort();
        QCOMPARE(rows, QStringList({"Glass Harbour", "Mara", "Mara"}));
        QVERIFY(shared < 0);
        browser.select(shared);
        const QVariantMap card = browser.details();
        QCOMPARE(card.value("from").toString(), QString("Ada Fixture's shared graph"));
        QCOMPARE(card.value("summary").toString(), QString("A ferry pilot."));
        QCOMPARE(card.value("relations").toList().size(), 1);
        // Shown whole, the book's own graph is all there; the shared one is not.
        browser.showWhole();
        rows.clear();
        for (const QVariant &row : browser.rows())
            rows << row.toMap().value("name").toString();
        QVERIFY(!rows.contains("Wick"));
        // The collection's graph, laid over its books, keeps to the same.
        browser.showCollection("Tides");
        rows.clear();
        for (const QVariant &row : browser.rows())
            rows << row.toMap().value("name").toString();
        QVERIFY(rows.contains("Glass Harbour") && !rows.contains("Wick") && !rows.contains("Hers Only"));
    }

    void theMarksOnThePageShowNoMore() {
        const Owner owner;
        Desk desk;
        Library &library = *desk.backend->library();
        QVERIFY(QFile::copy(owner.file(), desk.dir.filePath("tide.epub")));
        const qint64 book = library.add(desk.dir.filePath("tide.epub")).id;
        QVERIFY(Subscriptions::subscribe(library, owner.snapshot(), {}).has_value());
        desk.backend->reload();
        library.setProgress(book, {2, 0.5, 30});
        desk.reader->openBookAt(book, 2, 0.5);
        QVERIFY(QTest::qWaitFor([&desk] { return desk.reader->isOpen(); }, 5000));
        ReadingMarks *marks = desk.reader->marks();
        QTRY_VERIFY(marks->known() > 0);
        const QString text = "Mara met Wick at Glass Harbour.";
        QStringList marked;
        for (const TextMarks::Mark &mark : marks->marksIn(2, text))
            marked << text.mid(mark.start, mark.length);
        QCOMPARE(marked, QStringList({"Mara", "Glass Harbour"}));
        const QVariantMap card = marks->named("Mara");
        QCOMPARE(card.value("from").toString(), QString("Ada Fixture's shared graph"));
        QCOMPARE(card.value("summary").toString(), QString("A ferry pilot."));
        QVERIFY(marks->named("Wick").isEmpty() && marks->named("The Queen").isEmpty());
        // Read on, and more is marked.
        library.setProgress(book, {8, 0.6, 85});
        desk.reader->goToPlace(8, 0.6);
        QTRY_VERIFY(!marks->named("Wick").isEmpty());
        QCOMPARE(marks->named("Mara").value("summary").toString(), QString("Secretly the queen in exile."));
    }

    void unsubscribingElsewhereRefreshesWhatTheWindowKnows() {
        const Owner owner;
        Desk desk;
        Library &library = *desk.backend->library();
        QVERIFY(QFile::copy(owner.file(), desk.dir.filePath("tide.epub")));
        const qint64 book = library.add(desk.dir.filePath("tide.epub")).id;
        QVERIFY(Subscriptions::subscribe(library, owner.snapshot(), {}).has_value());
        library.setProgress(book, {2, 0.5, 30});
        desk.backend->reload();
        BookListModel *books = desk.backend->books();
        books->setQuery("harbour");
        QTRY_COMPARE(books->count(), 1);
        // Unsubscribed by a command or an agent while the window runs: it
        // counts as a change to the graph, which the window follows.
        Library elsewhere(library.root());
        QVERIFY(elsewhere.open());
        const Library::Changes before = library.changes();
        QVERIFY(Subscriptions::unsubscribe(elsewhere, "Tides").has_value());
        QVERIFY(library.changes().graph > before.graph);
        QCOMPARE(library.changes().books, before.books);
        desk.backend->reloadIfChanged();
        QTRY_COMPARE_WITH_TIMEOUT(books->count(), 0, 10000);
    }
};

QTEST_MAIN(SharedGraphTests)
#include "sharedgraph_tests.moc"
