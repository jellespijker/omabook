#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>

#include "assistant.h"
#include "backend.h"
#include "bookops.h"
#include "booksides.h"
#include "cli.h"
#include "curation.h"
#include "fixtures.h"
#include "graph.h"
#include "graphbatch.h"
#include "graphbuilder.h"
#include "graphcontext.h"
#include "graphlinks.h"
#include "graphtools.h"
#include "knowledgesearch.h"
#include "library.h"
#include "prompts.h"
#include "reader.h"
#include "scripttools.h"
#include "settings.h"

namespace {

// A made-up story in three chapters, as a plain text book. Each chapter
// carries a word of its own, so a test can tell which were sent to the model.
QByteArray story() {
    const QByteArray sea = QByteArray("The gulls wheeled over the grey water and the lamp turned through the night. ").repeated(30);
    return "Chapter One\n\n"
           "Mara Venn kept the lighthouse at Saltmere, as Old Tobin had before her. FIRSTMARK. Tobin had taught her "
           "tide-binding, the craft of holding the sea back with knotted rope and a spoken name.\n\n"
           + sea + "\n\n"
           "Chapter Two\n\n"
           "In the second winter the Tidewardens came to Saltmere. SECONDMARK. Their captain, Corin Ash, wanted the "
           "drowned bell that lay under the harbour, and Mara would not say where it was.\n\n"
           + sea + "\n\n"
           "Chapter Three\n\n"
           "Corin Ash was Old Tobin's son. THIRDMARK. Mara learned it from the bell itself, the night the harbour froze.\n\n"
           + sea + "\n";
}

// What a model would answer for the first chapter.
const char *const firstAnswer = R"json(Here is the graph:
```json
{"nodes": [
  {"kind": "character", "name": "Mara Venn", "aliases": ["Mara"], "summary": "Keeper of the lighthouse at Saltmere.", "section": 0},
  {"kind": "character", "name": "Old Tobin", "aliases": ["Tobin"], "summary": "The keeper before Mara, who taught her.", "section": 0},
  {"kind": "place", "name": "Saltmere", "summary": "A harbour town with a lighthouse.", "section": 0},
  {"kind": "magic system", "name": "Tide-binding", "summary": "Holding the sea back with knotted rope and a spoken name.", "section": 0},
  {"kind": "theme", "name": "Inheritance", "summary": "A craft handed from keeper to keeper.", "section": 0},
  {"kind": "topic", "name": "Lighthouse keeping", "summary": "The book's keeper tends a lamp through the night.", "section": 0}
],
"edges": [
  {"from": "Old Tobin", "to": "Mara Venn", "relation": "Mentor of", "section": 0},
  {"from": "Mara Venn", "to": "Saltmere", "relation": "lives in", "section": 0},
  {"from": "Mara Venn", "to": "Nobody Known", "relation": "fears", "section": 0}
]}
```)json";

const char *const secondAnswer = R"json({"nodes": [
  {"kind": "group", "name": "The Tidewardens", "aliases": ["Tidewardens"], "summary": "Came to Saltmere in the second winter.", "section": 1},
  {"kind": "character", "name": "Corin Ash", "summary": "Captain of the Tidewardens, after the drowned bell.", "section": 1},
  {"kind": "item", "name": "The drowned bell", "summary": "Lies under the harbour.", "section": 1},
  {"kind": "character", "name": "Mara", "summary": "Keeper of the lighthouse, who hides where the bell lies.", "section": 1}
],
"edges": [
  {"from": "Corin Ash", "to": "The Tidewardens", "relation": "captain of", "section": 1},
  {"from": "Corin Ash", "to": "The drowned bell", "relation": "seeks", "section": 1}
]})json";

// What a model would answer for the third chapter: a reveal, quoted from
// the text, and the bell's part in it quoted from further down.
const char *const thirdAnswer = R"json({"nodes": [
  {"kind": "character", "name": "Corin Ash", "aliases": ["Tobin's son"], "summary": "Captain of the Tidewardens, and Old Tobin's son.",
   "section": 2, "quote": "Corin Ash was Old Tobin's son."},
  {"kind": "item", "name": "The drowned bell", "summary": "Told Mara who Corin Ash is.", "section": 2,
   "quote": "Mara learned it from   the bell itself"}
],
"edges": [
  {"from": "Corin Ash", "to": "Old Tobin", "relation": "son of", "section": 2, "quote": "was Old Tobin's son"},
  {"from": "The drowned bell", "to": "Mara Venn", "relation": "speaks to", "section": 2, "quote": "words nobody wrote"}
]})json";

// A library on a temp folder holding the story.
struct Shelf {
    QTemporaryDir dir;
    Library library{dir.filePath("Library")};
    qint64 book = 0;

    Shelf() {
        library.open();
        book = add("The Keeper of Saltmere");
    }
    qint64 add(const QString &title) {
        const QString file = dir.filePath(title + ".txt");
        // Told apart by its last line: the very same file would be the same book.
        Fixtures::write(file, story() + "\nThe end of " + title.toUtf8() + ".\n");
        return library.add(file).id;
    }
    std::unique_ptr<Document> document(qint64 id) {
        QString error;
        return BookOps::open(&library, QString::number(id), &error);
    }
};

// A model for GraphBuild::build: it answers in turn, and keeps what it was asked.
struct Scripted {
    QStringList answers;
    QList<AiRequest> asked;
    bool fails = false;

    GraphBuild::Asker asker() {
        return [this](const AiRequest &request, QString *answer, QString *error) {
            if (fails) {
                *error = "The model is away.";
                return false;
            }
            *answer = answers.value(asked.size(), "{\"nodes\": [], \"edges\": []}");
            asked << request;
            return true;
        };
    }
    QString sent(int index) const { return asked.value(index).messages.value(0).text; }
};

GraphNode named(const Library &library, const QString &kind, const QString &name) {
    const Graph graph(library);
    return graph.node(graph.named({}, kind, name)).value_or(GraphNode());
}

bool has(const GraphView &view, const QString &name) {
    return std::ranges::any_of(view.nodes, [&](const GraphNode &node) { return node.name == name; });
}

// The window's own model: answers every request with what the test set.
struct AiLog {
    QList<AiRequest> requests;
    QString answer = "Mara Venn keeps the lighthouse.";
};

class FakeAi : public AiProvider {
public:
    explicit FakeAi(AiLog *log) : m_log(log) {}
    QString name() const override { return "Testbot"; }
    void ask(const AiRequest &request) override {
        m_log->requests << request;
        const QString answer = m_log->answer;
        QTimer::singleShot(0, this, [this, answer] {
            emit delta(answer.left(5));
            emit delta(answer.mid(5));
            emit finished();
        });
    }
    void cancel() override {}

private:
    AiLog *m_log;
};

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// The reader and its companion on a temp folder, without the window.
struct Desk {
    QTemporaryDir dir;
    NoPicker picker;
    AiLog ai;
    std::optional<Settings> settings;
    std::optional<Backend> backend;
    std::optional<Reader> reader;
    std::optional<Assistant> assistant;
    qint64 book = 0;

    Desk() {
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        settings.emplace(dir.filePath("config.json"));
        settings->setLibraryPath(dir.filePath("Library"));
        auto makeAi = [this](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<FakeAi>(&ai); };
        backend.emplace(&*settings, &picker, nullptr, makeAi, [](QObject *) { return QList<MetadataSource *>(); },
                        dir.filePath("plugins"), "-");
        reader.emplace(&*backend, QString());
        assistant.emplace(&*reader, makeAi);
        assistant->setGraphMode([this] { return settings->graph(); });
        assistant->setGraphWait(0);
        Fixtures::write(dir.filePath("The Keeper of Saltmere.txt"), story());
        book = backend->library()->add(dir.filePath("The Keeper of Saltmere.txt")).id;
        backend->reload();
    }

    bool open() {
        reader->openBook(book);
        return QTest::qWaitFor([this] { return reader->isOpen(); });
    }
    QString lastSent() const { return ai.requests.isEmpty() ? QString() : ai.requests.last().messages.last().text; }
};

// A machine of the test's own for the real binary: a home whose AI is a
// stand-in program, and a library. Nothing of the user's is read or written.
struct Machine {
    QTemporaryDir dir;
    QString library;

    Machine() : library(dir.filePath("Library")) {
        QDir().mkpath(dir.filePath("home/.config/omabook"));
        // The stand-in model keeps the prompt it was given and answers with a file.
        const QString model = dir.filePath("model");
        Fixtures::write(model, "#!/bin/bash\ncat > '" + dir.filePath("prompt").toUtf8() + "'\ncat '"
                                   + dir.filePath("answer").toUtf8() + "'\n");
        QFile::setPermissions(model, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Fixtures::write(dir.filePath("answer"), firstAnswer);
        Fixtures::write(dir.filePath("home/.config/omabook/config.json"),
                        QJsonDocument(QJsonObject{{"ai_provider", "command"}, {"ai_command", model}}).toJson());
        Fixtures::write(dir.filePath("story.txt"), story());
    }

    QProcessEnvironment environment() const {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("HOME", dir.filePath("home"));
        environment.insert("XDG_CONFIG_HOME", dir.filePath("home/.config"));
        environment.insert("XDG_CACHE_HOME", dir.filePath("home/.cache"));
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        for (const char *unset : {"DISPLAY", "WAYLAND_DISPLAY", "ANTHROPIC_API_KEY"})
            environment.remove(unset);
        return environment;
    }

    struct Ran {
        int code = -1;
        QString output, errors;
        QJsonObject json() const { return QJsonDocument::fromJson(output.toUtf8()).object(); }
    };
    // Runs `omabook <arguments> --library <the test's>` to the end.
    Ran omabook(QStringList arguments) const {
        QProcess process;
        process.setProcessEnvironment(environment());
        process.start(OMABOOK_BINARY, arguments << "--library" << library);
        process.closeWriteChannel();
        Ran ran;
        if (!process.waitForFinished(60000))
            return ran;
        ran.code = process.exitCode();
        ran.output = QString::fromUtf8(process.readAllStandardOutput());
        ran.errors = QString::fromUtf8(process.readAllStandardError());
        return ran;
    }
    QString prompt() const {
        QFile file(dir.filePath("prompt"));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }
};

// `omabook mcp`, spoken to a line at a time.
class Server {
public:
    explicit Server(const Machine &machine) {
        m_process.setProcessEnvironment(machine.environment());
        m_process.start(OMABOOK_BINARY, {"mcp", "--library", machine.library});
        m_process.waitForStarted();
    }
    ~Server() {
        m_process.closeWriteChannel();
        if (!m_process.waitForFinished(5000))
            m_process.kill();
    }
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    QJsonObject request(const QString &method, const QJsonObject &params = {}) {
        m_process.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", ++m_id}, {"method", method}, {"params", params}})
                            .toJson(QJsonDocument::Compact) + "\n");
        m_process.waitForBytesWritten();
        while (!m_process.canReadLine())
            if (!m_process.waitForReadyRead(60000))
                return {};
        return QJsonDocument::fromJson(m_process.readLine()).object().value("result").toObject();
    }
    // What a tool answered with, and whether it failed.
    QJsonObject call(const QString &tool, const QJsonObject &arguments, bool *failed = nullptr) {
        const QJsonObject result = request("tools/call", {{"name", tool}, {"arguments", arguments}});
        if (failed)
            *failed = result.value("isError").toBool();
        return result.value("structuredContent").toObject();
    }

private:
    QProcess m_process;
    int m_id = 0;
};

} // namespace

// The knowledge graph above its storage: grown from what is read, used by
// the AI, and reached from the command line, the MCP server and plugins.
class GraphTests : public QObject {
    Q_OBJECT

private slots:
    void aModelsAnswerGoesIntoTheGraph() {
        Shelf shelf;
        const auto document = shelf.document(shelf.book);
        QVERIFY(document);
        QVERIFY(document->count() >= 3);
        const QList<GraphBuild::Passage> first = GraphBuild::passages(*document, std::nullopt, {shelf.book, 0, 1.0});
        QCOMPARE(first.size(), 1);

        const GraphBuild::Applied applied = GraphBuild::apply(shelf.library, shelf.book, first, firstAnswer);
        QVERIFY(applied.understood);
        QCOMPARE(applied.nodes, 6);
        // The edge to a thing nobody named is left out.
        QCOMPARE(applied.edges, 2);

        const Graph graph(shelf.library);
        const GraphNode mara = named(shelf.library, "character", "Mara Venn");
        QCOMPARE(mara.summary, QString("Keeper of the lighthouse at Saltmere."));
        QCOMPARE(mara.aliases(), QStringList{"Mara"});
        // The model's own word for a kind is taken for ours.
        QCOMPARE(named(shelf.library, "concept", "Tide-binding").kind, QString("concept"));
        QVERIFY(named(shelf.library, "theme", "Inheritance").id > 0);
        QVERIFY(named(shelf.library, "topic", "Lighthouse keeping").id > 0);

        // Everything is placed in the chapter it came from, where its name
        // first shows, and tied to its book.
        const QList<GraphPlace> met = graph.seen(mara.id);
        QCOMPARE(met.size(), 1);
        QCOMPARE(met.first().bookId, shelf.book);
        QCOMPARE(met.first().section, 0);
        QVERIFY(met.first().fraction < 0.1);
        const GraphView around = graph.around(mara.id, 1);
        QVERIFY(has(around, "Old Tobin"));
        QVERIFY(has(around, "Saltmere"));
        QVERIFY(has(around, "The Keeper of Saltmere"));
        bool mentor = false;
        for (const GraphEdge &edge : around.edges)
            mentor = mentor || (edge.kind == "mentor of" && edge.target == mara.id && edge.learned.bookId == shelf.book);
        QVERIFY(mentor);
        QVERIFY(has(graph.ofBook(shelf.book), "Mara Venn"));

        // A known thing met under one of its other names is brought up to
        // date, not made twice.
        const QList<GraphBuild::Passage> second = GraphBuild::passages(*document, GraphPlace{shelf.book, 0, 1.0}, {shelf.book, 1, 1.0});
        QVERIFY(GraphBuild::apply(shelf.library, shelf.book, second, secondAnswer).understood);
        QCOMPARE(graph.find("Venn", "character").size(), 1);
        QCOMPARE(named(shelf.library, "character", "Mara Venn").summary, QString("Keeper of the lighthouse, who hides where the bell lies."));
        QCOMPARE(graph.seen(mara.id).first().section, 0);
    }

    void onlyWhatWasReadSinceIsSent() {
        Shelf shelf;
        const auto document = shelf.document(shelf.book);
        Scripted model;
        model.answers = {firstAnswer, secondAnswer};

        GraphBuild::Built built = GraphBuild::build(shelf.library, *document, shelf.book, {shelf.book, 0, 1.0}, model.asker());
        QVERIFY(built.complete);
        QCOMPARE(built.requests, 1);
        QCOMPARE(built.nodes, 6);
        QVERIFY(model.sent(0).contains("FIRSTMARK"));
        QVERIFY(!model.sent(0).contains("SECONDMARK"));
        QVERIFY(model.asked.first().system.contains("knowledge graph"));
        QCOMPARE(GraphBuild::builtTo(shelf.library, shelf.book)->section, 0);

        // The next chapter: only its text, with the names already known so
        // the model builds on them, and what is known of the ones it touches.
        built = GraphBuild::build(shelf.library, *document, shelf.book, {shelf.book, 1, 1.0}, model.asker());
        QCOMPARE(built.requests, 1);
        QVERIFY(model.sent(1).contains("SECONDMARK"));
        QVERIFY(!model.sent(1).contains("FIRSTMARK"));
        QVERIFY(!model.sent(1).contains("THIRDMARK"));
        QVERIFY(model.sent(1).contains("Mara Venn (character; also Mara): Keeper of the lighthouse at Saltmere."));
        QVERIFY(model.sent(1).contains("Old Tobin (character)"));
        QVERIFY(!model.sent(1).contains("The keeper before Mara"));

        // Nothing read since: nothing sent.
        built = GraphBuild::build(shelf.library, *document, shelf.book, {shelf.book, 1, 1.0}, model.asker());
        QVERIFY(built.complete);
        QCOMPARE(built.requests, 0);
        QCOMPARE(model.asked.size(), 2);

        // A long stretch goes in several requests, none over the limit.
        const QList<GraphBuild::Passage> whole = GraphBuild::passages(*document, std::nullopt, {shelf.book, 2, 1.0});
        const auto pieces = GraphBuild::chunks(whole, 1000);
        QVERIFY(pieces.size() > 5);
        QString joined;
        for (const auto &piece : pieces) {
            QVERIFY(GraphBuild::size(piece) <= 1000);
            for (const GraphBuild::Passage &passage : piece) {
                QVERIFY(passage.from <= passage.to);
                joined += passage.text;
            }
        }
        QCOMPARE(joined.size(), GraphBuild::size(whole));
    }

    void theGraphIsNeverAheadOfTheReader() {
        Shelf shelf;
        const auto document = shelf.document(shelf.book);
        Scripted model;
        // The model names the last chapter for something it should not know of.
        model.answers = {R"({"nodes": [{"kind": "character", "name": "Mara Venn", "summary": "Keeps the lighthouse.", "section": 0},
                                       {"kind": "event", "name": "The harbour froze", "summary": "Told ahead.", "section": 2}],
                             "edges": []})"};
        const GraphPlace halfway{shelf.book, 0, 0.5};
        const GraphBuild::Built built = GraphBuild::build(shelf.library, *document, shelf.book, halfway, model.asker());
        QVERIFY(built.complete);
        // Only the half that was read was sent.
        QVERIFY(model.sent(0).contains("FIRSTMARK"));
        QVERIFY(!model.sent(0).contains("SECONDMARK"));
        QVERIFY(model.sent(0).size() < document->text(0).size() * 0.6 + 6000);
        // Whatever section the model names, a fact is placed within what was sent.
        const Graph graph(shelf.library);
        for (const GraphNode &node : graph.ofBook(shelf.book).nodes)
            for (const GraphPlace &place : graph.seen(node.id))
                QVERIFY2(place.notAfter(halfway), qPrintable(node.name));
        QVERIFY(has(graph.ofBook(shelf.book, halfway), "The harbour froze"));
        // And a reader further back doesn't see it.
        QVERIFY(!has(graph.ofBook(shelf.book, GraphPlace{shelf.book, 0, 0.2}), "The harbour froze"));
        QVERIFY(has(graph.ofBook(shelf.book, GraphPlace{shelf.book, 0, 0.2}), "Mara Venn"));
    }

    void anAnswerThatIsNotJsonChangesNothing() {
        Shelf shelf;
        const auto document = shelf.document(shelf.book);
        const QList<GraphBuild::Passage> first = GraphBuild::passages(*document, std::nullopt, {shelf.book, 0, 1.0});
        const Graph graph(shelf.library);
        for (const char *answer : {"I am sorry, I cannot do that.", "{\"nodes\": [{\"kind\": ", "[1, 2, 3]", "{\"something\": \"else\"}", ""}) {
            QVERIFY2(!GraphBuild::apply(shelf.library, shelf.book, first, answer).understood, answer);
            QVERIFY(graph.ofBook(shelf.book).nodes.isEmpty());
        }
        // What is half right is taken for what it is worth.
        const GraphBuild::Applied partly = GraphBuild::apply(shelf.library, shelf.book, first,
            R"({"nodes": [{"kind": "character"}, 7, {"name": "Mara Venn", "aliases": "Mara", "section": "0"}, {"kind": 3, "name": "Saltmere"}],
                "edges": [{"from": "Mara Venn"}, {"from": "Mara Venn", "to": "Saltmere", "relation": ""}, "x"]})");
        QVERIFY(partly.understood);
        QCOMPARE(partly.nodes, 2);
        QCOMPARE(partly.edges, 0);

        // While building, such an answer is said and passed over; a model
        // that can't be reached stops the build where it is.
        Shelf other;
        const auto again = other.document(other.book);
        Scripted model;
        model.answers = {"Certainly! Here are my thoughts on the book."};
        GraphBuild::Built built = GraphBuild::build(other.library, *again, other.book, {other.book, 0, 1.0}, model.asker());
        QVERIFY(built.complete);
        QCOMPARE(built.problems.size(), 1);
        QCOMPARE(built.nodes, 0);
        model.fails = true;
        built = GraphBuild::build(other.library, *again, other.book, {other.book, 1, 1.0}, model.asker());
        QVERIFY(!built.complete);
        QCOMPARE(built.problems, QStringList{"The model is away."});
        QCOMPARE(GraphBuild::builtTo(other.library, other.book)->section, 0);
    }

    void twoBooksPeopleAreNotOnePerson() {
        Shelf shelf;
        const qint64 other = shelf.add("Another Story");
        const auto first = shelf.document(shelf.book), second = shelf.document(other);
        const char *answer = R"({"nodes": [{"kind": "character", "name": "Mara Venn", "summary": "A keeper.", "section": 0},
                                           {"kind": "theme", "name": "Inheritance", "summary": "Handed down.", "section": 0}], "edges": []})";
        GraphBuild::apply(shelf.library, shelf.book, GraphBuild::passages(*first, std::nullopt, {shelf.book, 0, 1.0}), answer);
        GraphBuild::apply(shelf.library, other, GraphBuild::passages(*second, std::nullopt, {other, 0, 1.0}), answer);
        const Graph graph(shelf.library);
        // A character of an unrelated book is another person; a theme is the
        // same theme, met in both.
        QCOMPARE(graph.find("Mara Venn", "character").size(), 2);
        QVERIFY(named(shelf.library, "character", "Mara Venn (Another Story)").id > 0);
        QCOMPARE(graph.find("Inheritance", "theme").size(), 1);
        QCOMPARE(graph.seen(named(shelf.library, "theme", "Inheritance").id).size(), 2);
        // Met again in its own book, the second Mara is found, not made a third time.
        GraphBuild::apply(shelf.library, other, GraphBuild::passages(*second, GraphPlace{other, 0, 1.0}, {other, 1, 1.0}),
                          R"({"nodes": [{"kind": "character", "name": "Mara Venn", "summary": "Older now.", "section": 1}], "edges": []})");
        QCOMPARE(graph.find("Mara Venn", "character").size(), 2);
        QCOMPARE(named(shelf.library, "character", "Mara Venn (Another Story)").summary, QString("Older now."));
        QCOMPARE(named(shelf.library, "character", "Mara Venn").summary, QString("A keeper."));
    }

    void whatWasAskedJoinsTheGraph() {
        Shelf shelf;
        const auto document = shelf.document(shelf.book);
        GraphBuild::apply(shelf.library, shelf.book, GraphBuild::passages(*document, std::nullopt, {shelf.book, 0, 1.0}), firstAnswer);
        const GraphPlace place{shelf.book, 0, 0.8};
        const qint64 id = GraphContext::noteQuestion(Graph::ofReader(shelf.library, shelf.book), shelf.book, place, "Who taught Mara Venn?",
                                                     "Old Tobin did. He kept the lighthouse before her.");
        const Graph graph(shelf.library);
        const GraphNode question = *graph.node(id);
        QCOMPARE(question.kind, QString("question"));
        QCOMPARE(question.name, QString("Who taught Mara Venn?"));
        QCOMPARE(question.summary, QString("Old Tobin did. He kept the lighthouse before her."));
        QCOMPARE(question.data.value("book").toInteger(), shelf.book);
        QVERIFY(!question.data.value("asked").toString().isEmpty());
        // Tied to the book, at the place it was asked, and to who it names.
        const GraphView around = graph.around(id, 1);
        QVERIFY(has(around, "The Keeper of Saltmere"));
        QVERIFY(has(around, "Mara Venn"));
        QVERIFY(has(around, "Old Tobin"));
        QVERIFY(!has(around, "Saltmere"));
        QCOMPARE(graph.seen(id).first().section, 0);
        QVERIFY(has(graph.ofBook(shelf.book, GraphPlace{shelf.book, 0, 0.9}), "Who taught Mara Venn?"));
        QVERIFY(!has(graph.ofBook(shelf.book, GraphPlace{shelf.book, 0, 0.5}), "Who taught Mara Venn?"));

        // A question of the whole library names the books its answer does.
        const qint64 asked = GraphContext::noteQuestion(Graph::ofLibrary(shelf.library), 0, std::nullopt, "Which book is about inheritance?",
                                                        "The Keeper of Saltmere is: a craft is handed from keeper to keeper.");
        QCOMPARE(graph.node(asked)->data.value("of").toString(), QString("library"));
        QVERIFY(has(graph.around(asked, 1), "The Keeper of Saltmere"));
        QVERIFY(has(graph.around(asked, 1), "Inheritance"));
        QCOMPARE(graph.find({}, "question").size(), 2);
    }

    void theCompanionUsesTheGraphAndFeedsIt() {
        Desk desk;
        QVERIFY(desk.open());
        Library &library = *desk.backend->library();
        const Graph graph(library);

        // Finishing a chapter grows the graph with what was read, no more.
        desk.ai.answer = firstAnswer;
        desk.reader->setSection(1);
        QVERIFY(QTest::qWaitFor([&] { return has(graph.ofBook(desk.book), "Mara Venn") && !desk.assistant->graphBusy(); }));
        QCOMPARE(desk.ai.requests.size(), 1);
        QVERIFY(desk.lastSent().contains("FIRSTMARK"));
        QVERIFY(!desk.lastSent().contains("SECONDMARK"));
        QCOMPARE(GraphBuild::builtTo(library, desk.book)->section, 1);

        // A question is sent with what the graph holds on the things it
        // names and their neighbours, not with the whole graph.
        desk.ai.answer = "Old Tobin taught her tide-binding.";
        desk.assistant->ask("Who is Mara Venn?");
        QVERIFY(QTest::qWaitFor([&] { return !desk.assistant->busy(); }));
        QVERIFY(desk.lastSent().contains("<known>"));
        QVERIFY(desk.lastSent().contains("Mara Venn (character; also Mara)"));
        QVERIFY(desk.lastSent().contains("Old Tobin — mentor of → Mara Venn"));
        QVERIFY(!desk.lastSent().contains("Lighthouse keeping"));
        // And it joins the graph, with the gist of its answer.
        const GraphNode question = named(library, "question", "Who is Mara Venn?");
        QCOMPARE(question.summary, QString("Old Tobin taught her tide-binding."));
        QVERIFY(has(graph.around(question.id, 1), "Mara Venn"));

        // The first who-is-who builds on the graph: what it holds goes in
        // place of the chapter already read.
        desk.reader->goToPlace(1, 0.9);
        desk.ai.answer = "Mara Venn: keeper of the lighthouse.";
        desk.assistant->whoIsWho();
        QVERIFY(QTest::qWaitFor([&] { return !desk.assistant->busy() && !library.notes(desk.book, "cast").text.isEmpty(); }));
        const auto cast = std::ranges::find_if(desk.ai.requests, [](const AiRequest &request) {
            return request.messages.last().text.contains("List who is who");
        });
        QVERIFY(cast != desk.ai.requests.end());
        QVERIFY(cast->messages.last().text.contains("Old Tobin (character; also Tobin)"));
        QVERIFY(cast->messages.last().text.contains("SECONDMARK"));
        QVERIFY(!cast->messages.last().text.contains("FIRSTMARK"));
        // It also brought the graph up to where the reader is.
        QVERIFY(QTest::qWaitFor([&] { return !desk.assistant->graphBusy() && GraphBuild::builtTo(library, desk.book)->fraction > 0.8; }));

        // With the graph off, nothing is read from it or added to it.
        desk.settings->setGraph("off");
        const int before = desk.ai.requests.size();
        desk.assistant->ask("Where is Saltmere?");
        QVERIFY(QTest::qWaitFor([&] { return !desk.assistant->busy(); }));
        QVERIFY(!desk.lastSent().contains("<known>"));
        QCOMPARE(named(library, "question", "Where is Saltmere?").id, qint64(0));
        desk.reader->setSection(2);
        QTest::qWait(50);
        QCOMPARE(desk.ai.requests.size(), before + 1);
        // Only when asked: a chapter finished sends nothing, asking does.
        desk.settings->setGraph("ask");
        desk.reader->goToPlace(2, 0.5);
        QTest::qWait(50);
        QCOMPARE(desk.ai.requests.size(), before + 1);
        desk.assistant->growGraph();
        QVERIFY(QTest::qWaitFor([&] { return desk.ai.requests.size() == before + 2 && !desk.assistant->graphBusy(); }));
        QVERIFY(desk.lastSent().contains("THIRDMARK"));
    }

    void readingIsLoggedAsProgressIsSaved() {
        Desk desk;
        QVERIFY(desk.open());
        Library &library = *desk.backend->library();
        const Graph graph(library);
        QVERIFY(graph.readingLog(desk.book).isEmpty());
        desk.reader->goToPlace(1, 0.5);
        const double percent = desk.reader->percent();
        desk.reader->close();
        const QList<ReadingDay> days = graph.readingLog(desk.book);
        QCOMPARE(days.size(), 1);
        QCOMPARE(days.first().day, QDate::currentDate().toString(Qt::ISODate));
        QCOMPARE(days.first().from, 0.0);
        QCOMPARE(days.first().to, percent);
        // The book's own node says how far it was read.
        const GraphNode node = *graph.node(Graph(library).bookNode(desk.book));
        QCOMPARE(node.data.value("percent_read").toInt(), qRound(percent));
        // Paging back is no reading; reading on the same day widens the day.
        QVERIFY(desk.open());
        desk.reader->goToPlace(0, 0.1);
        desk.reader->close();
        QCOMPARE(graph.readingLog(desk.book).first().to, percent);
        QVERIFY(desk.open());
        desk.reader->goToPlace(2, 0.5);
        desk.reader->close();
        QCOMPARE(graph.readingLog(desk.book).size(), 1);
        QVERIFY(graph.readingLog(desk.book).first().to > percent);
    }

    void whatTheModelIsToldStaysSmall() {
        Shelf shelf;
        Graph graph(shelf.library);
        const auto document = shelf.document(shelf.book);
        // A book with a great many things in its graph, all tied to one.
        const qint64 mara = graph.upsert({}, "character", "Mara Venn", "Keeper of the lighthouse.", {}, GraphPlace{shelf.book, 0, 0.0});
        for (int index = 0; index < 600; ++index) {
            const qint64 id = graph.upsert({}, index % 3 == 0 ? "place" : "character", QString("Villager %1").arg(index),
                                           QString("One of the many people of Saltmere, the %1th of them, with a long tale. ").arg(index).repeated(4),
                                           {}, GraphPlace{shelf.book, 0, 0.1});
            graph.link(id, mara, "neighbour of", {}, GraphPlace{shelf.book, 0, 0.1});
        }
        const QString slice = GraphContext::forQuestion(Graph::ofReader(shelf.library, shelf.book), shelf.book, {shelf.book, 2, 1.0}, "Who is Mara Venn?");
        QVERIFY(slice.contains("Mara Venn (character)"));
        QVERIFY2(slice.size() < 9000, qPrintable(QString::number(slice.size())));
        QVERIFY(GraphContext::forQuestion(Graph::ofReader(shelf.library, shelf.book), shelf.book, {shelf.book, 2, 1.0}, "What is the weather like?").isEmpty());
        QVERIFY(GraphContext::digest(Graph::ofReader(shelf.library, shelf.book), shelf.book, {shelf.book, 2, 1.0}, {"character"}).size() < 16000);

        // What goes with an extraction: what the text names in full, the
        // rest by name, and never the whole graph.
        const QList<GraphBuild::Passage> next = GraphBuild::passages(*document, GraphPlace{shelf.book, 0, 1.0}, {shelf.book, 1, 1.0});
        const QString known = GraphBuild::known(shelf.library, shelf.book, next);
        QVERIFY(known.contains("Mara Venn (character): Keeper of the lighthouse."));
        QVERIFY2(known.size() < 14000, qPrintable(QString::number(known.size())));
        QVERIFY(GraphBuild::request(shelf.library, shelf.book, next).messages.first().text.size() < GraphBuild::chunkSize + 16000);

        // A large library with years of reading and questions: the picture
        // the librarian is given keeps to its room.
        for (int index = 0; index < 150; ++index) {
            BookMeta meta;
            meta.title = QString("Book number %1").arg(index);
            meta.authors = QStringList{"An Author"};
            const qint64 id = shelf.library.addEntry(meta, false).id;
            for (int day = 1; day <= 20; ++day)
                graph.logReading(id, day * 4, day * 4 + 4, QString("2025-%1-%2").arg(1 + index % 12, 2, 10, QChar('0')).arg(day, 2, 10, QChar('0')));
            for (int theme = 0; theme < 4; ++theme)
                graph.upsert({}, "theme", QString("Theme %1").arg((index + theme) % 60), "What a book is about.", {}, GraphPlace{id, 0, 0.0});
            GraphContext::noteQuestion(Graph::ofLibrary(shelf.library), 0, std::nullopt, QString("What is book number %1 about, at length?").arg(index),
                                       QString("It is about a great many things. ").repeated(20));
        }
        const QString picture = GraphContext::picture(Graph::ofLibrary(shelf.library), "Which books are about the lighthouse and its keeper?", 6000);
        QVERIFY2(picture.size() <= 6000, qPrintable(QString::number(picture.size())));
        QVERIFY(picture.contains("Reading history"));
        QVERIFY(picture.contains("Themes"));
        QVERIFY(picture.contains("Questions the reader asked"));
        QVERIFY(picture.contains("Mara Venn"));
        // It rides along with a question of the library and with suggestions.
        const QList<Curation::BookNotes> books = BookOps::booksWithNotes(shelf.library);
        QVERIFY(Curation::libraryQuestion(books, "What next?", picture).messages.first().text.contains("<knowledge_graph>"));
        QVERIFY(Curation::suggestionRequest(books, picture).messages.first().text.contains("<knowledge_graph>"));
        QVERIFY(!Curation::libraryQuestion(books, "What next?").messages.first().text.contains("<knowledge_graph>"));
    }

    void theCommandLineReachesAllOfIt() {
        const Machine machine;
        QCOMPARE(machine.omabook({"add", machine.dir.filePath("story.txt")}).code, 0);
        QVERIFY(isCliCommand("graph"));

        // A book not yet read has no graph to grow, unless a place is named.
        Machine::Ran ran = machine.omabook({"graph", "build", "1", "--json"});
        QCOMPARE(ran.code, 1);
        QVERIFY(ran.errors.contains("has not been read yet"));
        ran = machine.omabook({"graph", "build", "1", "--upto", "0", "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(ran.json().value("nodes").toInt(), 6);
        QCOMPARE(ran.json().value("requests").toInt(), 1);
        QVERIFY(ran.json().value("complete").toBool());
        QVERIFY(machine.prompt().contains("FIRSTMARK"));
        QVERIFY(!machine.prompt().contains("SECONDMARK"));
        // Again, with nothing read since: no request.
        QCOMPARE(machine.omabook({"graph", "build", "1", "--upto", "0", "--json"}).json().value("requests").toInt(), 0);

        const QJsonObject shown = machine.omabook({"graph", "show", "1", "--json"}).json();
        QCOMPARE(shown.value("nodes").toArray().size(), 6);
        QCOMPARE(shown.value("built_to").toObject().value("section").toInt(), 0);
        QVERIFY(machine.omabook({"graph", "show", "1"}).output.contains("Old Tobin — mentor of → Mara Venn"));

        const QJsonArray found = machine.omabook({"graph", "find", "Mara Venn", "--kind", "character", "--json"}).json().value("nodes").toArray();
        QCOMPARE(found.size(), 1);
        const QString mara = QString::number(found.first().toObject().value("id").toInteger());

        // Adding, relating, folding together and removing by hand.
        ran = machine.omabook({"graph", "add", "--kind", "character", "--name", "The Keeper", "--summary", "Seen from afar.",
                               "--alias", "Keeper", "--book", "1", "--section", "0", "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        const QString keeper = QString::number(ran.json().value("node").toObject().value("id").toInteger());
        ran = machine.omabook({"graph", "add", "--kind", "place", "--name", "The Lamp Room", "--json"});
        const QString lamp = QString::number(ran.json().value("node").toObject().value("id").toInteger());
        ran = machine.omabook({"graph", "link", keeper, lamp, "works in", "--note", "every night", "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(ran.json().value("kind").toString(), QString("works in"));
        QCOMPARE(machine.omabook({"graph", "link", keeper, "99999", "knows"}).code, 1);

        ran = machine.omabook({"graph", "merge", mara, keeper, "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QVERIFY(ran.json().value("node").toObject().value("data").toObject().value("aliases").toArray().contains("The Keeper"));
        const QJsonObject node = machine.omabook({"graph", "node", mara, "--depth", "1", "--json"}).json();
        QCOMPARE(node.value("node").toObject().value("name").toString(), QString("Mara Venn"));
        QCOMPARE(node.value("seen").toArray().first().toObject().value("title").toString(), QString("story"));
        QStringList around;
        for (const QJsonValue &value : node.value("nodes").toArray())
            around << value.toObject().value("name").toString();
        QVERIFY(around.contains("The Lamp Room"));
        QVERIFY(around.contains("Old Tobin"));

        QCOMPARE(machine.omabook({"graph", "remove", lamp, "--json"}).json().value("removed").toObject().value("name").toString(),
                 QString("The Lamp Room"));
        QCOMPARE(machine.omabook({"graph", "remove", lamp}).code, 1);

        // Every format, to the terminal and to a file.
        QVERIFY(machine.omabook({"graph", "export", "1", "--format", "dot"}).output.startsWith("digraph omabook {"));
        QVERIFY(machine.omabook({"graph", "export", "1", "--format", "graphml"}).output.contains("<graphml"));
        QVERIFY(machine.omabook({"graph", "export", "1", "--format", "cypher"}).output.contains("MERGE (n:Thing"));
        const QString file = machine.dir.filePath("graph.json");
        QCOMPARE(machine.omabook({"graph", "export", "--format", "json", "--output", file}).code, 0);
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(written.readAll()).object().value("nodes").toArray().size() >= 7);
        QCOMPARE(machine.omabook({"graph", "export", "1", "--format", "pdf"}).code, 1);

        // Scopes: a graph of its own, forked from the general one and thrown away.
        QCOMPARE(machine.omabook({"graph", "copy-scope", "", "world:1", "--json"}).json().value("scope").toString(), QString("world:1"));
        QCOMPARE(machine.omabook({"graph", "add", "--kind", "character", "--name", "Ilse", "--scope", "world:1"}).code, 0);
        const QJsonArray scopes = machine.omabook({"graph", "scopes", "--json"}).json().value("scopes").toArray();
        QCOMPARE(scopes.size(), 2);
        QCOMPARE(scopes.last().toObject().value("scope").toString(), QString("world:1"));
        QVERIFY(machine.omabook({"graph", "find", "Ilse", "--json"}).json().value("nodes").toArray().isEmpty());
        QCOMPARE(machine.omabook({"graph", "find", "Ilse", "--everywhere", "--json"}).json().value("nodes").toArray().size(), 1);
        QVERIFY(machine.omabook({"graph", "show", "--scope", "world:1", "--json"}).json().value("nodes").toArray().size() >= 8);
        QCOMPARE(machine.omabook({"graph", "remove-scope", "world:1"}).code, 0);
        QCOMPARE(machine.omabook({"graph", "remove-scope", ""}).code, 1);

        // What is said of a book's reading is on its node, and the log is there.
        QCOMPARE(machine.omabook({"set", "1", "--rating", "4", "--status", "read"}).code, 0);
        const QJsonObject book = machine.omabook({"graph", "find", "story", "--kind", "book", "--json"}).json()
                                     .value("nodes").toArray().first().toObject();
        QCOMPARE(book.value("data").toObject().value("rating").toInt(), 4);
        QCOMPARE(book.value("data").toObject().value("status").toString(), QString("read"));
        QVERIFY(machine.omabook({"graph", "log", "--json"}).json().contains("days"));
        QCOMPARE(machine.omabook({"graph", "sync", "--json"}).json().value("books").toInt(), 1);
        QVERIFY(machine.omabook({"graph"}).output.contains("omabook graph <command>"));
        QCOMPARE(machine.omabook({"graph", "nonsense"}).code, 1);
    }

    void booksReadBeforeAreBuiltAfterTheFact() {
        Shelf shelf;
        const qint64 second = shelf.add("The Second Tide"), unread = shelf.add("Unopened");
        BookMeta paper;
        paper.title = "On Paper";
        const qint64 shelved = shelf.library.addEntry(paper, true).id;
        // One read to its end elsewhere, one partly read here, one not at all.
        shelf.library.setReading(shelf.book, "read", 4, {});
        shelf.library.setProgress(second, {0, 0.6, 30, 0});
        QCOMPARE(GraphBatch::readBooks(shelf.library), (QList<qint64>{shelf.book, second}));

        // What it would cost is known before anything is sent, and every
        // book left out says why.
        const GraphBatch::Choice choice{{shelf.book, second, unread, shelved, 999}, false, false};
        const GraphBatch::Plan planned = GraphBatch::plan(shelf.library, choice);
        QCOMPARE(planned.books, 2);
        QVERIFY(planned.requests >= 2 && planned.tokens() > planned.characters / 4);
        QVERIFY(planned.items.at(0).characters > planned.items.at(1).characters); // the whole book against part of one
        QCOMPARE(planned.items.at(2).skipped, QString("It hasn't been read yet."));
        QCOMPARE(planned.items.at(3).skipped, QString("It has no file to read from."));
        QVERIFY(!planned.items.at(4).skipped.isEmpty());

        // The books are worked through in turn, each as far as it was read.
        Scripted model;
        model.answers = QStringList(planned.requests, firstAnswer);
        int heard = 0;
        const GraphBatch::Done done = GraphBatch::run(shelf.library, choice, model.asker(), {},
                                                      [&](int, int total, const QString &, const GraphBatch::Done &) {
                                                          QCOMPARE(total, 2);
                                                          ++heard;
                                                      });
        QCOMPARE(done.books, 2);
        QCOMPARE(done.requests, planned.requests);
        QVERIFY(done.nodes > 0 && heard >= 2 && !done.stopped);
        QVERIFY(Graph(shelf.library).ofBook(shelf.book).nodes.size() > 0);
        QVERIFY(Graph(shelf.library).ofBook(unread).nodes.isEmpty());
        // The second book went no further than it was read.
        QVERIFY(GraphBuild::builtTo(shelf.library, second)->notAfter({second, 0, 0.6}));

        // Again, there is nothing left to send; whole, the unread ones join in.
        QCOMPARE(GraphBatch::plan(shelf.library, choice).books, 0);
        QCOMPARE(GraphBatch::plan(shelf.library, choice).items.at(0).skipped, QString("Its graph is already up to date."));
        QVERIFY(GraphBatch::plan(shelf.library, {{unread, second}, true, false}).books >= 1);

        // Stopped, it ends before the next request and says so.
        Scripted second_model;
        const GraphBatch::Done stopped = GraphBatch::run(shelf.library, {{unread}, true, false}, second_model.asker(), [] { return true; });
        QVERIFY(stopped.stopped);
        QCOMPARE(second_model.asked.size(), 0);
        QVERIFY(Graph(shelf.library).ofBook(unread).nodes.isEmpty());
    }

    void aBookIsShownWhatTheOtherBooksHold() {
        Shelf shelf;
        const qint64 other = shelf.add("The Glass Orchard");
        const auto first = shelf.document(shelf.book), second = shelf.document(other);
        GraphBuild::apply(shelf.library, shelf.book, GraphBuild::passages(*first, std::nullopt, {shelf.book, 0, 1.0}), firstAnswer);
        const QList<GraphBuild::Passage> passages = GraphBuild::passages(*second, std::nullopt, {other, 0, 1.0});

        // A book not begun says nothing to another, however far its graph goes.
        QVERIFY(GraphBuild::elsewhere(shelf.library, other, passages).isEmpty());
        // Read, its themes, topics and ideas are offered, with the places
        // and groups the new text names, each with the book it is from; its
        // people and things are not: those are its own.
        shelf.library.setReading(shelf.book, "read", 0, {});
        const QString elsewhere = GraphBuild::elsewhere(shelf.library, other, passages);
        QVERIFY(elsewhere.contains("Inheritance (theme; in The Keeper of Saltmere): A craft handed from keeper to keeper."));
        QVERIFY(elsewhere.contains("Tide-binding (concept; in The Keeper of Saltmere)"));
        QVERIFY(elsewhere.contains("Lighthouse keeping (topic; in The Keeper of Saltmere)"));
        QVERIFY(elsewhere.contains("Saltmere (place; in The Keeper of Saltmere)"));
        QVERIFY(!elsewhere.contains("Mara Venn"));
        const AiRequest request = GraphBuild::request(shelf.library, other, passages);
        QVERIFY(request.messages.first().text.contains("<other_books>"));
        QVERIFY(request.system.contains("same_as"));
        // A book's own things are not offered to itself.
        QVERIFY(!GraphBuild::request(shelf.library, shelf.book, GraphBuild::passages(*first, GraphPlace{shelf.book, 0, 1.0}, {shelf.book, 1, 1.0}))
                     .messages.first().text.contains("<other_books>"));

        // Said to be the same thing, a theme under another wording and an
        // idea join the node there is; a person and a place that only share
        // a name stay two, and a person is never the same as another book's.
        const GraphBuild::Applied applied = GraphBuild::apply(shelf.library, other, passages,
            R"({"nodes": [
                {"kind": "theme", "name": "What is handed down", "summary": "An orchard passed on with its debts.", "same_as": "Inheritance", "section": 0},
                {"kind": "concept", "name": "Tide-binding", "summary": "Used here to water the orchard.", "same_as": "Tide-binding", "section": 0},
                {"kind": "character", "name": "Mara Venn", "summary": "An orchard keeper.", "same_as": "Mara Venn", "section": 0},
                {"kind": "place", "name": "Saltmere", "summary": "A town of glasshouses.", "section": 0},
                {"kind": "theme", "name": "Thrift", "summary": "Nothing is wasted.", "same_as": "No Such Thing", "section": 0}],
                "edges": [{"from": "Mara Venn", "to": "What is handed down", "relation": "carries", "section": 0}]})");
        QVERIFY(applied.understood);
        QCOMPARE(applied.nodes, 5);
        QCOMPARE(applied.edges, 1);
        const Graph graph(shelf.library);
        const GraphNode theme = named(shelf.library, "theme", "Inheritance");
        QCOMPARE(graph.find({}, "theme").size(), 2);
        QCOMPARE(theme.aliases(), QStringList{"What is handed down"});
        QCOMPARE(theme.summary, QString("A craft handed from keeper to keeper."));
        QCOMPARE(graph.seen(theme.id).size(), 2);
        QCOMPARE(graph.find("Tide-binding", "concept").size(), 1);
        QCOMPARE(graph.seen(named(shelf.library, "concept", "Tide-binding").id).size(), 2);
        QCOMPARE(named(shelf.library, "concept", "Tide-binding").summary, QString("Holding the sea back with knotted rope and a spoken name."));
        QCOMPARE(graph.find("Mara Venn", "character").size(), 2);
        QVERIFY(named(shelf.library, "character", "Mara Venn (The Glass Orchard)").id > 0);
        QCOMPARE(graph.find("Saltmere", "place").size(), 2);
        QVERIFY(named(shelf.library, "theme", "Thrift").id > 0);
        // What the second book makes of the shared theme is on its tie to that book.
        bool said = false;
        for (const GraphEdge &edge : graph.around(theme.id, 1).edges)
            said = said || edge.note == "An orchard passed on with its debts.";
        QVERIFY(said);
        // The other book that shares it is what this one reaches beyond itself.
        QVERIFY(has(graph.beyond(other), "The Keeper of Saltmere"));
        QVERIFY(has(graph.beyond(shelf.book), "The Glass Orchard"));

        // A reader with a great many themes behind them: what goes along
        // stays some forty lines, the ones the text points at first.
        Graph filling(shelf.library);
        BookMeta meta;
        meta.title = "A Long Shelf";
        const qint64 long_shelf = shelf.library.addEntry(meta, false).id;
        shelf.library.setReading(long_shelf, "read", 0, {});
        for (int index = 0; index < 500; ++index)
            filling.upsert({}, index % 2 ? "theme" : "concept", QString("Idea number %1").arg(index),
                           QString("A long account of an idea met long ago, the %1th of them. ").arg(index).repeated(5), {},
                           GraphPlace{long_shelf, 0, 0.0});
        filling.upsert({}, "theme", "Drowned bells", "What the sea has taken still rings.", {}, GraphPlace{long_shelf, 0, 0.0});
        const QString many = GraphBuild::elsewhere(shelf.library, shelf.book,
                                                   GraphBuild::passages(*first, GraphPlace{shelf.book, 0, 1.0}, {shelf.book, 1, 1.0}));
        QVERIFY2(many.startsWith("Drowned bells (theme; in A Long Shelf)"), qPrintable(many.left(200)));
        QVERIFY2(many.count('\n') < 40, qPrintable(QString::number(many.count('\n'))));
        QVERIFY2(many.size() < 8000, qPrintable(QString::number(many.size())));
        QVERIFY(GraphBuild::request(shelf.library, shelf.book, GraphBuild::passages(*first, GraphPlace{shelf.book, 0, 1.0}, {shelf.book, 1, 1.0}))
                    .messages.first().text.size() < GraphBuild::chunkSize + 24000);
    }

    void booksAreConnectedAcrossTheShelf() {
        Shelf shelf;
        const qint64 other = shelf.add("The Storm Choir"), ahead = shelf.add("Half Read"), unopened = shelf.add("Unopened");
        const auto first = shelf.document(shelf.book), second = shelf.document(other);
        Graph graph(shelf.library);
        // One book read to its end, with its graph.
        GraphBuild::apply(shelf.library, shelf.book, GraphBuild::passages(*first, std::nullopt, {shelf.book, 0, 1.0}), firstAnswer);
        GraphBuild::setBuiltTo(shelf.library, shelf.book, {shelf.book, 2, 1.0});
        shelf.library.setReading(shelf.book, "read", 5, {});
        // The one being read, built as far as it was read.
        GraphBuild::apply(shelf.library, other, GraphBuild::passages(*second, std::nullopt, {other, 1, 0.5}),
            R"({"nodes": [{"kind": "theme", "name": "Debts to the sea", "summary": "What the sea gives it takes back.", "section": 0},
                          {"kind": "concept", "name": "Storm-singing", "summary": "Calming weather with a sung name.", "section": 0},
                          {"kind": "character", "name": "Ilse Brandt", "summary": "A choirmaster who sings to storms.", "section": 1}],
                "edges": [{"from": "Ilse Brandt", "to": "Storm-singing", "relation": "practises", "section": 1}]})");
        const GraphPlace here{other, 1, 0.5};
        GraphBuild::setBuiltTo(shelf.library, other, here);
        shelf.library.setProgress(other, {1, 0.5, 50, 0});
        // A third whose graph runs ahead of its reader, and a fourth never opened.
        const qint64 early = graph.upsert({}, "theme", "Salt and memory", "Met on the first page.", {}, GraphPlace{ahead, 0, 0.0});
        const qint64 late = graph.upsert({}, "theme", "The last betrayal", "Only told at the very end.", {}, GraphPlace{ahead, 2, 0.9});
        GraphBuild::setBuiltTo(shelf.library, ahead, {ahead, 2, 1.0});
        shelf.library.setProgress(ahead, {0, 0.5, 10, 0});
        const qint64 never = graph.upsert({}, "theme", "Never met", "Of a book not begun.", {}, GraphPlace{unopened, 0, 0.0});
        GraphBuild::setBuiltTo(shelf.library, unopened, {unopened, 2, 1.0});

        const qint64 storm = named(shelf.library, "concept", "Storm-singing").id, debts = named(shelf.library, "theme", "Debts to the sea").id;
        const qint64 tide = named(shelf.library, "concept", "Tide-binding").id, inheritance = named(shelf.library, "theme", "Inheritance").id;
        const qint64 ilse = named(shelf.library, "character", "Ilse Brandt").id, mara = named(shelf.library, "character", "Mara Venn").id;

        // What is put side by side: this book's telling things, and of the
        // others only what their reader has reached.
        const GraphLinks::Offer offer = GraphLinks::offer(shelf.library, other, here);
        auto ids = [](const QList<GraphNode> &nodes) {
            QSet<qint64> set;
            for (const GraphNode &node : nodes)
                set << node.id;
            return set;
        };
        QCOMPARE(ids(offer.own), (QSet<qint64>{storm, debts, ilse}));
        QVERIFY(ids(offer.others).contains(tide) && ids(offer.others).contains(inheritance) && ids(offer.others).contains(mara));
        QVERIFY(ids(offer.others).contains(early));
        QVERIFY(!ids(offer.others).contains(late));
        QVERIFY(!ids(offer.others).contains(never));
        QCOMPARE(offer.titles.value(tide), QString("The Keeper of Saltmere"));
        QVERIFY(offer.own.size() <= GraphLinks::mostOwn && offer.others.size() <= GraphLinks::mostOthers);
        const AiRequest request = GraphLinks::request(shelf.library, other, offer);
        QVERIFY(request.messages.first().text.contains(QString("#%1 Tide-binding (concept; in The Keeper of Saltmere)").arg(tide)));
        QVERIFY(request.messages.first().text.contains(QString("#%1 Storm-singing (concept)").arg(storm)));
        QVERIFY(!request.messages.first().text.contains("The last betrayal"));
        QVERIFY(GraphLinks::due(shelf.library, other));

        // What the model answers becomes edges across books; what was not
        // on offer, or is said twice, or ties two things of one book, doesn't.
        const QString answer = QString(R"(Here you are: {"links": [
            {"from": %1, "to": %2, "relation": "Same idea as", "note": "Both hold the weather back with a spoken name."},
            {"from": %2, "to": %1, "relation": "echoes", "note": "The same pair the other way round."},
            {"from": %1, "to": %5, "relation": "foreshadows", "note": "Not yet reached in that book."},
            {"from": %1, "to": %6, "relation": "echoes", "note": "Of a book not begun."},
            {"from": %1, "to": %3, "relation": "echoes", "note": "Two things of this one book."},
            {"from": 987654, "to": %2, "relation": "echoes", "note": "Nothing of the kind."}],
            "topics": [{"name": "The Odyssey", "summary": "The epic of a long way home by sea.", "from": "%7", "to": %4,
                        "note": "Both make the sea a creditor, as Homer does."}]})")
                                   .arg(storm).arg(tide).arg(debts).arg(inheritance).arg(late).arg(never).arg(QString("#%1").arg(debts));
        Scripted model;
        model.answers = {answer, answer, "I would rather not."};
        GraphLinks::Connected connected = GraphLinks::connect(shelf.library, other, model.asker());
        QVERIFY(connected.asked);
        QVERIFY2(connected.problem.isEmpty(), qPrintable(connected.problem));
        QCOMPARE(connected.links, 1);
        QCOMPARE(connected.topics, 1);
        QVERIFY(model.asked.first().system.contains("connections"));

        // They are told from a book's own facts by their mark, and were
        // learned where this book's reader was.
        GraphView beyond = graph.beyond(other);
        QVERIFY(has(beyond, "Tide-binding"));
        QVERIFY(has(beyond, "Inheritance"));
        QVERIFY(!has(beyond, "The last betrayal") && !has(beyond, "Never met") && !has(beyond, "Mara Venn"));
        const auto link = std::ranges::find_if(beyond.edges, [&](const GraphEdge &edge) { return edge.target == tide; });
        QVERIFY(link != beyond.edges.end());
        QVERIFY(link->across());
        QCOMPARE(link->kind, QString("across: same idea as"));
        QCOMPARE(link->relation(), QString("same idea as"));
        QCOMPARE(link->source, storm);
        QCOMPARE(link->note, QString("Both hold the weather back with a spoken name."));
        QCOMPARE(link->learned.bookId, other);
        QCOMPARE(link->learned.section, 1);
        QVERIFY(link->toJson().value("across").toBool());
        for (const GraphEdge &edge : graph.ofBook(shelf.book).edges)
            QVERIFY(!edge.across());
        // The work both point at is a topic of this book now, tied to both.
        const qint64 odyssey = named(shelf.library, "topic", "The Odyssey").id;
        QVERIFY(has(graph.ofBook(other), "The Odyssey"));
        QStringList pointing;
        for (const GraphEdge &edge : graph.around(odyssey, 1).edges)
            if (edge.across() && edge.target == odyssey)
                pointing << graph.node(edge.source)->name;
        pointing.sort();
        QCOMPARE(pointing, (QStringList{"Debts to the sea", "Inheritance"}));
        // A reader further back in this book hasn't noticed them yet; the
        // other book sees them from its side.
        QVERIFY(graph.beyond(other, GraphPlace{other, 0, 0.0}).nodes.isEmpty());
        QVERIFY(has(graph.beyond(shelf.book), "Storm-singing"));
        QVERIFY(has(graph.beyond(shelf.book), "The Odyssey"));

        // Asked again there is nothing new to compare, so nothing is sent;
        // made to ask, the same answer adds nothing.
        QVERIFY(!GraphLinks::due(shelf.library, other));
        connected = GraphLinks::connect(shelf.library, other, model.asker());
        QVERIFY(!connected.asked);
        QVERIFY(!connected.skipped.isEmpty());
        QCOMPARE(model.asked.size(), 1);
        const qsizetype edges = graph.whole({}, 100000).edges.size();
        connected = GraphLinks::connect(shelf.library, other, model.asker(), true);
        QVERIFY(connected.asked);
        QCOMPARE(connected.links + connected.topics, 0);
        QVERIFY(model.asked.at(1).messages.first().text.contains("<already>"));
        // An answer that is not the JSON asked for changes nothing.
        connected = GraphLinks::connect(shelf.library, other, model.asker(), true);
        QVERIFY(connected.asked);
        QVERIFY(!connected.problem.isEmpty());
        QCOMPARE(graph.whole({}, 100000).edges.size(), edges);
        for (const char *bad : {"", "[]", "{\"links\": [", "{\"nodes\": []}"})
            QVERIFY2(!GraphLinks::apply(shelf.library, other, here, offer, bad).understood, bad);

        // A book with no graph costs nothing.
        QVERIFY(!GraphLinks::connect(shelf.library, shelf.add("No Graph Yet"), model.asker()).asked);
        QCOMPARE(model.asked.size(), 3);

        // The companion is told of them with a question that names the
        // thing, the librarian in its picture, and who-is-who is not.
        const QString slice = GraphContext::forQuestion(Graph::ofReader(shelf.library, other), other, here, "What is storm-singing?");
        QVERIFY2(slice.contains("Storm-singing — same idea as → Tide-binding [concept in The Keeper of Saltmere"), qPrintable(slice));
        QVERIFY(slice.size() < 6000);
        QVERIFY(!GraphContext::forQuestion(Graph::ofReader(shelf.library, other), other, {other, 0, 0.0}, "What is storm-singing?").contains("Tide-binding"));
        QVERIFY(GraphContext::picture(Graph::ofLibrary(shelf.library)).contains(
            "Storm-singing [The Storm Choir] — same idea as → Tide-binding [The Keeper of Saltmere]"));
        QVERIFY(!GraphContext::digest(Graph::ofReader(shelf.library, other), other, here, {"character", "place", "group"}).contains("Tide-binding"));

        // What it will take is part of what a build says it will cost.
        const QJsonObject estimate = GraphOps::connect(shelf.library, other, model.asker(), true, true);
        QCOMPARE(estimate.value("requests").toInt(), 1);
        QCOMPARE(GraphOps::connect(shelf.library, other, model.asker(), false, true).value("requests").toInt(), 0);
        QCOMPARE(model.asked.size(), 3);
        const GraphBatch::Plan alone = GraphBatch::plan(shelf.library, {{unopened}, true, true});
        const GraphBatch::Plan both = GraphBatch::plan(shelf.library, {{other, unopened}, true, true});
        QVERIFY(alone.items.first().connects);
        QCOMPARE(alone.requests, int(GraphBuild::chunks(GraphBuild::passages(*shelf.document(unopened), std::nullopt, {unopened, 99, 1.0})).size()) + 1);
        QCOMPARE(both.requests, alone.requests * 2);
        // And a run makes that request once a book, after its last chunk.
        Scripted batch;
        const GraphBatch::Done done = GraphBatch::run(shelf.library, {{other}, true, true}, batch.asker());
        QCOMPARE(done.requests, int(batch.asked.size()));
        QVERIFY(batch.asked.last().system.contains("connections"));
        int connecting = 0;
        for (const AiRequest &asked : std::as_const(batch.asked))
            connecting += asked.system.contains("well-read friend") ? 1 : 0;
        QCOMPARE(connecting, 1);
    }

    // A thing met in a book, but described only by a book on the other side
    // of the top collections: looking at the first book, its card says
    // nothing of what the other taught. With one side, it does, as before.
    void aBooksCardSaysNothingTheOtherSideTaught() {
        Shelf shelf;
        Graph graph(shelf.library);
        const qint64 novel = shelf.book, paper = shelf.add("Wave Mechanics");
        for (const qint64 book : {novel, paper})
            shelf.library.setReading(book, "read", 0, {});
        const qint64 thing = graph.upsert({}, "concept", "Lantern-work", {}, {}, GraphPlace{novel, 0, 0.0});
        QCOMPARE(graph.upsert({}, "concept", "Lantern-work", "Light bent along a wire.", {}, GraphPlace{paper, 0, 0.0}), thing);
        const auto summaryIn = [&](qint64 book) {
            for (const GraphNode &node : Graph::ofReader(shelf.library, book).ofBook(book, GraphPlace{book, 2, 1.0}).nodes)
                if (node.id == thing)
                    return node.summary;
            return QString("(not shown)");
        };
        QCOMPARE(summaryIn(novel), QString("Light bent along a wire."));
        const qint64 play = *shelf.library.addCollection("Spel"), work = *shelf.library.addCollection("Werk");
        QVERIFY(shelf.library.putInCollection(play, novel, true));
        QVERIFY(shelf.library.putInCollection(work, paper, true));
        QCOMPARE(summaryIn(novel), QString());
        QCOMPARE(summaryIn(paper), QString("Light bent along a wire."));
    }

    void topCollectionsKeepTheirBooksApart() {
        Shelf shelf;
        Graph graph(shelf.library);
        // Without collections every book is on one side, as it always was.
        QVERIFY(!BookSides::of(shelf.library).apart());
        QVERIFY(BookSides::of(shelf.library).together(shelf.book, 999));

        // A library split in two at the top, with a part inside one of them,
        // named as its owner likes.
        const qint64 novel = shelf.book, paper = shelf.add("Wave Mechanics"), textbook = shelf.add("Fluid Dynamics");
        const qint64 loose = shelf.add("Loose Leaves"), stray = shelf.add("Stray Pages");
        const qint64 work = *shelf.library.addCollection("Werk");
        const qint64 inner = *shelf.library.addCollection("Werk/Golven");
        const qint64 play = *shelf.library.addCollection("Spel");
        QVERIFY(shelf.library.putInCollection(play, novel, true));
        QVERIFY(shelf.library.putInCollection(inner, paper, true));
        QVERIFY(shelf.library.putInCollection(work, textbook, true));
        BookSides sides = BookSides::of(shelf.library);
        QVERIFY(sides.apart());
        QCOMPARE(sides.side(paper), (BookSides::Side{work}));
        QVERIFY(!sides.together(novel, paper));
        // A part inside a top collection is on its side.
        QVERIFY(sides.together(paper, textbook));
        // Books in no collection are on a side of their own.
        QCOMPARE(sides.side(loose), (BookSides::Side{BookSides::none}));
        QVERIFY(sides.together(loose, stray) && !sides.together(loose, novel) && !sides.together(loose, paper));

        // Each book read, with a graph. The novel and the paper share an
        // idea, which stays one thing with both books.
        const auto first = shelf.document(novel);
        GraphBuild::apply(shelf.library, novel, GraphBuild::passages(*first, std::nullopt, {novel, 0, 1.0}), firstAnswer);
        for (const qint64 book : {novel, paper, textbook}) {
            GraphBuild::setBuiltTo(shelf.library, book, {book, 2, 1.0});
            shelf.library.setReading(book, "read", 0, {});
        }
        const qint64 tide = named(shelf.library, "concept", "Tide-binding").id;
        QCOMPARE(graph.upsert({}, "concept", "Tide-binding", {}, {}, GraphPlace{paper, 0, 0.0}), tide);
        const qint64 waves = graph.upsert({}, "concept", "Standing waves", "Waves that hold their place.", {}, GraphPlace{paper, 0, 0.0});
        const qint64 flow = graph.upsert({}, "concept", "Viscosity", "How thick a flow is.", {}, GraphPlace{textbook, 0, 0.0});
        const qint64 mara = named(shelf.library, "character", "Mara Venn").id;
        const qint64 inheritance = named(shelf.library, "theme", "Inheritance").id;

        // What is put side by side never holds a thing of the other side.
        const GraphLinks::Offer offer = GraphLinks::offer(shelf.library, paper, {paper, 2, 1.0});
        QSet<qint64> others;
        for (const GraphNode &node : offer.others)
            others << node.id;
        QVERIFY(others.contains(flow));
        QVERIFY(!others.contains(mara) && !others.contains(inheritance));
        const QString prompt = GraphLinks::request(shelf.library, paper, offer).messages.first().text;
        QVERIFY(prompt.contains("Viscosity") && !prompt.contains("The Keeper of Saltmere") && !prompt.contains("Mara Venn"));
        // Nor what a build is shown of the other books.
        const auto third = shelf.document(paper);
        const QString shown = GraphBuild::elsewhere(shelf.library, paper, GraphBuild::passages(*third, std::nullopt, {paper, 0, 1.0}));
        QVERIFY2(shown.contains("Viscosity") && !shown.contains("Inheritance") && !shown.contains("The Keeper of Saltmere"), qPrintable(shown));
        // Asked to link across anyway, the model's answer adds nothing there.
        const QString answer = QString(R"({"links": [{"from": %1, "to": %2, "relation": "echoes", "note": "Across the library."},
                                                     {"from": %1, "to": %3, "relation": "same idea as", "note": "Within one part."}]})")
                                   .arg(waves).arg(mara).arg(flow);
        const GraphLinks::Applied applied = GraphLinks::apply(shelf.library, paper, {paper, 2, 1.0}, offer, answer);
        QCOMPARE(applied.links, 1);

        // A connection between the two sides from before is kept, and not shown.
        QVERIFY(graph.link(waves, mara, GraphEdge::acrossMark() + "echoes", "From before the library was split.", GraphPlace{paper, 1, 0.0}) > 0);
        const auto edgeTo = [](const GraphView &view, qint64 a, qint64 b) {
            return std::ranges::any_of(view.edges, [&](const GraphEdge &edge) {
                return edge.across() && ((edge.source == a && edge.target == b) || (edge.source == b && edge.target == a));
            });
        };
        const auto check = [&](bool split) {
            const Graph now = Graph::ofReader(shelf.library, paper);
            const GraphView beyond = now.beyond(paper);
            QCOMPARE(has(beyond, "Viscosity"), true);
            QCOMPARE(has(beyond, "Mara Venn"), !split);
            // The idea both share is one thing, but the other side's book is not beyond.
            QCOMPARE(has(beyond, "The Keeper of Saltmere"), !split);
            QCOMPARE(edgeTo(now.acrossBooks(), waves, mara), !split);
            QVERIFY(edgeTo(now.acrossBooks(), waves, flow));
            const GraphView whole = now.whole({}, 100000);
            QCOMPARE(edgeTo(whole, waves, mara), !split);
            QVERIFY(edgeTo(whole, waves, flow) && has(whole, "Tide-binding") && has(whole, "Mara Venn") == !split);
            QCOMPARE(std::ranges::count(whole.nodes, tide, &GraphNode::id), 1);
            QCOMPARE(has(now.around(waves, 1), "Mara Venn"), !split);
            // The AI is told only of its own side.
            const QString slice = GraphContext::forQuestion(Graph::ofReader(shelf.library, paper), paper, {paper, 2, 1.0}, "What are standing waves?");
            QVERIFY2(slice.contains("Viscosity"), qPrintable(slice));
            QCOMPARE(slice.contains("Mara Venn"), !split);
            QCOMPARE(GraphContext::forQuestion(Graph::ofReader(shelf.library, novel), novel, {novel, 2, 1.0}, "Who is Mara Venn?").contains("Standing waves"), !split);
        };
        check(true);

        // A series isn't carried across either.
        const auto inSeries = [&](qint64 id, double index) {
            BookMeta meta = shelf.library.book(id)->meta;
            meta.series = "Saltmere";
            meta.seriesIndex = index;
            QVERIFY(shelf.library.setMeta(id, meta));
        };
        inSeries(novel, 1);
        inSeries(paper, 2);
        QVERIFY(shelf.library.earlierInSeries(paper).isEmpty());

        // Moved to the other side, the book's links move with it, and none is lost.
        QVERIFY(shelf.library.putInCollection(inner, paper, false));
        QVERIFY(shelf.library.putInCollection(play, paper, true));
        QCOMPARE(BookSides::of(shelf.library).side(paper), (BookSides::Side{play}));
        {
            const Graph now = Graph::ofReader(shelf.library, paper);
            QVERIFY(has(now.beyond(paper), "Mara Venn") && !has(now.beyond(paper), "Viscosity"));
            QVERIFY(edgeTo(now.acrossBooks(), waves, mara) && !edgeTo(now.acrossBooks(), waves, flow));
            QCOMPARE(shelf.library.earlierInSeries(paper).size(), 1);
        }
        // In both, it is in both tenants and links with either, in each.
        QVERIFY(shelf.library.putInCollection(work, paper, true));
        QCOMPARE(BookSides::of(shelf.library).side(paper), (BookSides::Side{work, play}));
        QVERIFY(has(Graph::ofCollection(shelf.library, play).beyond(paper), "Mara Venn") && has(Graph::ofCollection(shelf.library, work).beyond(paper), "Viscosity"));
        // And back where it was, as before.
        QVERIFY(shelf.library.putInCollection(play, paper, false));
        QVERIFY(shelf.library.putInCollection(work, paper, false));
        QVERIFY(shelf.library.putInCollection(inner, paper, true));
        check(true);

        // Turned off, the graph links across the whole library again.
        Settings settings;
        settings.setGraphOneTenant(true);
        const auto restore = qScopeGuard([&settings] { settings.setGraphOneTenant(false); });
        QVERIFY(!BookSides::of(shelf.library).apart());
        check(false);
        QCOMPARE(shelf.library.earlierInSeries(paper).size(), 1);
        QVERIFY(GraphLinks::offer(shelf.library, paper, {paper, 2, 1.0}).titles.values().contains("The Keeper of Saltmere"));
        settings.setGraphOneTenant(false);
        check(true);
    }

    void theWindowComparesOnceASession() {
        Desk desk;
        Library &library = *desk.backend->library();
        // Another book, read, with a graph of its own.
        Fixtures::write(desk.dir.filePath("The Storm Choir.txt"), story() + "\nAnother book altogether.\n");
        const qint64 other = library.add(desk.dir.filePath("The Storm Choir.txt")).id;
        QString error;
        const auto document = BookOps::open(&library, QString::number(other), &error);
        GraphBuild::apply(library, other, GraphBuild::passages(*document, std::nullopt, {other, 0, 1.0}),
            R"({"nodes": [{"kind": "concept", "name": "Storm-singing", "summary": "Calming weather with a sung name.", "section": 0}], "edges": []})");
        GraphBuild::setBuiltTo(library, other, {other, 2, 1.0});
        library.setReading(other, "read", 0, {});

        // A chapter finished: its text is sent, with what the other book
        // holds, and then, once, the two books are put side by side.
        QVERIFY(desk.open());
        desk.ai.answer = firstAnswer;
        desk.reader->setSection(1);
        QVERIFY(QTest::qWaitFor([&] { return desk.ai.requests.size() == 2 && !desk.assistant->graphBusy(); }));
        QVERIFY(desk.ai.requests.at(0).messages.first().text.contains("Storm-singing (concept; in The Storm Choir)"));
        QVERIFY(desk.ai.requests.at(1).system.contains("well-read friend"));
        QVERIFY(desk.ai.requests.at(1).messages.first().text.contains("(concept; in The Storm Choir)"));
        QVERIFY(!desk.ai.requests.at(1).messages.first().text.contains("FIRSTMARK"));
        // The next chapter brings nothing new to compare, so only its text goes.
        desk.reader->setSection(2);
        QVERIFY(QTest::qWaitFor([&] { return desk.ai.requests.size() == 3 && !desk.assistant->graphBusy(); }));
        QTest::qWait(50);
        QCOMPARE(desk.ai.requests.size(), 3);
        QVERIFY(desk.ai.requests.at(2).messages.first().text.contains("SECONDMARK"));
    }

    void connectionsAreReachedFromTheCommandLineAndTheTools() {
        const Machine machine;
        Fixtures::write(machine.dir.filePath("choir.txt"), story() + "\nAnother book altogether.\n");
        QCOMPARE(machine.omabook({"add", machine.dir.filePath("story.txt")}).code, 0);
        QCOMPARE(machine.omabook({"add", machine.dir.filePath("choir.txt")}).code, 0);
        // One book read, with its graph; the other being read, with its own.
        QCOMPARE(machine.omabook({"graph", "build", "1", "--upto", "0", "--json"}).json().value("nodes").toInt(), 6);
        QCOMPARE(machine.omabook({"set", "1", "--status", "read"}).code, 0);
        Fixtures::write(machine.dir.filePath("answer"),
                        R"({"nodes": [{"kind": "concept", "name": "Storm-singing", "summary": "Calming weather with a sung name.", "section": 0}], "edges": []})");
        QCOMPARE(machine.omabook({"graph", "build", "2", "--upto", "0", "--no-connect", "--json"}).json().value("nodes").toInt(), 1);
        QVERIFY(machine.prompt().contains("Tide-binding (concept; in story)"));
        QCOMPARE(machine.omabook({"progress", "2", "--section", "1"}).code, 0);
        auto idOf = [&](const QString &name) {
            return machine.omabook({"graph", "find", name, "--json"}).json().value("nodes").toArray().first().toObject().value("id").toInteger();
        };
        const qint64 storm = idOf("Storm-singing"), tide = idOf("Tide-binding");
        QVERIFY(storm > 0 && tide > 0);

        // What it would take, then the request itself, then nothing more.
        QJsonObject estimate = machine.omabook({"graph", "connect", "2", "--estimate", "--json"}).json();
        QCOMPARE(estimate.value("requests").toInt(), 1);
        QCOMPARE(estimate.value("own").toInt(), 1);
        QVERIFY(estimate.value("others").toInt() >= 2);
        Fixtures::write(machine.dir.filePath("answer"),
                        QString(R"({"links": [{"from": %1, "to": %2, "relation": "same idea as", "note": "A spoken name holds the weather."}], "topics": []})")
                            .arg(storm).arg(tide).toUtf8());
        Machine::Ran ran = machine.omabook({"graph", "connect", "2", "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QVERIFY(ran.json().value("asked").toBool());
        QCOMPARE(ran.json().value("links").toInt(), 1);
        const QJsonObject made = ran.json().value("connections").toArray().first().toObject();
        QCOMPARE(made.value("relation").toString(), QString("same idea as"));
        QCOMPARE(made.value("from").toString(), QString("Storm-singing"));
        QCOMPARE(made.value("other_book").toString(), QString("story"));
        QVERIFY(machine.prompt().contains(QString("#%1 Tide-binding (concept; in story)").arg(tide)));
        ran = machine.omabook({"graph", "connect", "2", "--json"});
        QVERIFY(!ran.json().value("asked").toBool());
        QVERIFY(!ran.json().value("skipped").toString().isEmpty());
        QCOMPARE(machine.omabook({"graph", "connect", "2", "--estimate", "--json"}).json().value("requests").toInt(), 0);
        QCOMPARE(machine.omabook({"graph", "connect", "99"}).code, 1);
        // Every book read, each once.
        ran = machine.omabook({"graph", "connect", "--read", "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QVERIFY(ran.json().value("books").toArray().size() >= 1);

        // A book's graph with what it reaches beyond itself.
        QVERIFY(!machine.omabook({"graph", "show", "2", "--json"}).json().contains("beyond"));
        const QJsonObject beyond = machine.omabook({"graph", "show", "2", "--beyond", "--json"}).json().value("beyond").toObject();
        QCOMPARE(beyond.value("nodes").toArray().first().toObject().value("name").toString(), QString("Tide-binding"));
        QCOMPARE(beyond.value("nodes").toArray().first().toObject().value("in").toArray().first().toObject().value("title").toString(), QString("story"));
        QVERIFY(beyond.value("edges").toArray().first().toObject().value("across").toBool());
        QVERIFY(machine.omabook({"graph", "show", "2", "--beyond"}).output.contains("Storm-singing — across: same idea as → Tide-binding [story]"));
        QVERIFY(machine.omabook({"graph", "show", "2", "--beyond", "--upto", "0", "--fraction", "0", "--json"}).json()
                    .value("beyond").toObject().value("nodes").toArray().isEmpty());

        // The same over MCP.
        Server server(machine);
        bool failed = false;
        QCOMPARE(server.call("graph_connect", {{"id", 2}, {"estimate", true}}, &failed).value("requests").toInt(), 0);
        QVERIFY(!failed);
        QCOMPARE(server.call("graph_connect", {{"id", 2}, {"again", true}}).value("links").toInt(), 0);
        QCOMPARE(server.call("graph_book", {{"id", 2}, {"beyond", true}}).value("beyond").toObject().value("nodes").toArray().size(), 1);
        QVERIFY(!server.call("graph_book", {{"id", 2}}).contains("beyond"));
        server.call("graph_connect", {{"id", 99}}, &failed);
        QVERIFY(failed);
    }

    void theToolsAreTheCommands() {
        const Machine machine;
        QCOMPARE(machine.omabook({"add", machine.dir.filePath("story.txt")}).code, 0);
        Server server(machine);

        // Each tool is there, says what it does to the world, and is a command.
        const QHash<QString, QString> effects{
            {"graph_find", "read"}, {"graph_node", "read"}, {"graph_book", "read"}, {"graph_scope", "read"},
            {"graph_scopes", "read"}, {"graph_reading_log", "read"}, {"graph_add", "change"}, {"graph_link", "change"},
            {"graph_build", "change"}, {"graph_export", "change"}, {"graph_copy_scope", "change"}, {"graph_connect", "change"},
            {"graph_unlink", "destroy"}, {"graph_merge", "destroy"}, {"graph_remove", "destroy"}, {"graph_remove_scope", "destroy"},
        };
        int met = 0;
        for (const QJsonValue &value : server.request("tools/list").value("tools").toArray()) {
            const QJsonObject tool = value.toObject();
            const QString name = tool.value("name").toString();
            if (!name.startsWith("graph_"))
                continue;
            ++met;
            QVERIFY2(effects.contains(name), qPrintable(name));
            const QJsonObject notes = tool.value("annotations").toObject();
            QCOMPARE(notes.value("readOnlyHint").toBool(), effects.value(name) == "read");
            QCOMPARE(notes.value("destructiveHint").toBool(), effects.value(name) == "destroy");
            QCOMPARE(notes.value("openWorldHint").toBool(), name == "graph_build" || name == "graph_connect");
        }
        QCOMPARE(met, int(effects.size()));

        bool failed = false;
        QJsonObject built = server.call("graph_build", {{"id", 1}, {"up_to_section", 0}}, &failed);
        QVERIFY(!failed);
        QCOMPARE(built.value("nodes").toInt(), 6);
        QVERIFY(!machine.prompt().contains("SECONDMARK"));

        const QJsonArray found = server.call("graph_find", {{"words", "Tobin"}}).value("nodes").toArray();
        QCOMPARE(found.size(), 1);
        const qint64 tobin = found.first().toObject().value("id").toInteger();
        const QJsonObject around = server.call("graph_node", {{"id", tobin}, {"depth", 1}});
        QCOMPARE(around.value("node").toObject().value("name").toString(), QString("Old Tobin"));
        QVERIFY(around.value("edges").toArray().size() >= 2);
        QCOMPARE(server.call("graph_book", {{"id", 1}}).value("nodes").toArray().size(), 6);
        QCOMPARE(server.call("graph_book", {{"id", 1}, {"up_to_section", 0}, {"up_to_fraction", 0.0}}).value("nodes").toArray().size(), 0);

        const qint64 added = server.call("graph_add", {{"kind", "item"}, {"name", "The Lamp"}, {"summary", "It turns."},
                                                      {"aliases", QJsonArray{"Lamp"}}, {"book", 1}, {"section", 0}})
                                 .value("node").toObject().value("id").toInteger();
        QVERIFY(added > 0);
        const qint64 edge = server.call("graph_link", {{"source", tobin}, {"target", added}, {"relation", "tends"}}).value("edge").toInteger();
        QVERIFY(edge > 0);
        QCOMPARE(server.call("graph_book", {{"id", 1}}).value("nodes").toArray().size(), 7);
        server.call("graph_unlink", {{"edge", edge}}, &failed);
        QVERIFY(!failed);
        const qint64 again = server.call("graph_add", {{"kind", "item"}, {"name", "The Great Lamp"}}).value("node").toObject().value("id").toInteger();
        QCOMPARE(server.call("graph_merge", {{"keep", added}, {"other", again}}).value("folded_in").toInteger(), again);
        QCOMPARE(server.call("graph_remove", {{"id", added}}).value("removed").toObject().value("name").toString(), QString("The Lamp"));
        server.call("graph_remove", {{"id", added}}, &failed);
        QVERIFY(failed);

        QVERIFY(server.call("graph_export", {{"format", "dot"}, {"book", 1}}).value("content").toString().startsWith("digraph"));
        QCOMPARE(server.call("graph_copy_scope", {{"from", ""}, {"to", "world:7"}}).value("scope").toString(), QString("world:7"));
        QCOMPARE(server.call("graph_scopes", {}).value("scopes").toArray().size(), 2);
        QVERIFY(server.call("graph_scope", {{"scope", "world:7"}}).value("nodes").toArray().size() >= 7);
        QCOMPARE(server.call("graph_remove_scope", {{"scope", "world:7"}}).value("removed_scope").toString(), QString("world:7"));
        QVERIFY(server.call("graph_reading_log", {}).contains("days"));

        // The book's graph is also something to attach.
        QStringList templates;
        for (const QJsonValue &value : server.request("resources/templates/list").value("resourceTemplates").toArray())
            templates << value.toObject().value("uriTemplate").toString();
        QVERIFY(templates.contains("omabook://graph/book/{id}"));
        const QJsonObject resource = server.request("resources/read", {{"uri", "omabook://graph/book/1"}})
                                         .value("contents").toArray().first().toObject();
        QCOMPARE(QJsonDocument::fromJson(resource.value("text").toString().toUtf8()).object().value("nodes").toArray().size(), 6);
    }

    void pluginsReadAndWriteTheGraph() {
        Shelf shelf;
        Settings settings(shelf.dir.filePath("config.json"));
        const auto document = shelf.document(shelf.book);
        GraphBuild::apply(shelf.library, shelf.book, GraphBuild::passages(*document, std::nullopt, {shelf.book, 0, 1.0}), firstAnswer);

        const QString folder = shelf.dir.filePath("plugins");
        QDir().mkpath(folder + "/worlds");
        Fixtures::write(folder + "/worlds/plugin.json", R"({"name": "Worlds", "version": "1.0", "script": "main.js"})");
        Fixtures::write(folder + "/worlds/main.js", R"js(
            function work(book) {
                var g = omabook.graph;
                var mara = g.find("Mara", {kind: "character"})[0];
                var world = "world:test";
                var copied = g.copyScope("", world);
                var hero = g.upsert({scope: world, kind: "character", name: "Ilse", summary: "A cartographer.", aliases: ["The Mapper"]});
                var city = g.upsert({scope: world, kind: "place", name: "Hollow"});
                var edge = g.link(hero, city, "Born In", {note: "long ago"});
                var item = g.upsert({kind: "item", name: "The Lamp", book: book, section: 0});
                var refused;
                try { g.upsert({kind: "", name: ""}); } catch (error) { refused = error.message; }
                var result = {
                    mara: mara.name, node: g.node(mara.id).summary, missing: g.node(987654),
                    around: g.around(mara.id, 1).nodes.length, ofBook: g.ofBook(book).nodes.length,
                    early: g.ofBook(book, {section: 0, fraction: 0}).nodes.length,
                    copied: copied, scopes: g.scopes(), hero: g.node(hero).data.aliases,
                    relation: g.around(hero, 1).edges[0].kind, inWorld: g.find("Ilse", {scope: world}).length,
                    inGeneral: g.find("Ilse").length, everywhere: g.find("Ilse", {everywhere: true}).length,
                    whole: g.whole(world).nodes.length > 6, dot: g.exported("dot", {scope: world}).indexOf("digraph") === 0,
                    bookJson: JSON.parse(g.exported("json", {book: book})).nodes.length, refused: refused,
                    beyond: g.beyond(book).nodes.length, connect: typeof g.connect(book).then
                };
                result.unlinked = g.unlink(edge);
                var twin = g.upsert({scope: world, kind: "character", name: "Ilsa"});
                result.merged = g.merge(hero, twin);
                result.removed = g.remove(city);
                result.gone = g.removeScope(world);
                result.general = g.removeScope("");
                result.after = g.scopes();
                return result;
            }
        )js");
        ScriptTools::Context context;
        context.folder = folder;
        context.settings = &settings;
        context.library = [&] { return &shelf.library; };
        const QJsonObject answer = ScriptTools::run(context, "worlds", "work", QJsonArray{shelf.book});
        QVERIFY2(!ScriptTools::failed(answer), qPrintable(answer.value("error").toString()));
        const QJsonObject did = answer.value("result").toObject();
        QCOMPARE(did.value("mara").toString(), QString("Mara Venn"));
        QCOMPARE(did.value("node").toString(), QString("Keeper of the lighthouse at Saltmere."));
        QVERIFY(did.value("missing").isNull());
        QVERIFY(did.value("around").toInt() >= 3);
        QCOMPARE(did.value("ofBook").toInt(), 7);
        // Only what was met at the very start: the lamp the plugin placed there.
        QCOMPARE(did.value("early").toInt(), 1);
        QVERIFY(did.value("copied").toBool());
        QCOMPARE(did.value("scopes").toArray(), (QJsonArray{"", "world:test"}));
        QCOMPARE(did.value("hero").toArray(), QJsonArray{"The Mapper"});
        QCOMPARE(did.value("relation").toString(), QString("born in"));
        QCOMPARE(did.value("inWorld").toInt(), 1);
        QCOMPARE(did.value("inGeneral").toInt(), 0);
        QCOMPARE(did.value("everywhere").toInt(), 1);
        QVERIFY(did.value("whole").toBool());
        QVERIFY(did.value("dot").toBool());
        QCOMPARE(did.value("bookJson").toInt(), 7);
        QVERIFY(did.value("refused").toString().contains("kind and a name"));
        QCOMPARE(did.value("beyond").toInt(), 0);
        QCOMPARE(did.value("connect").toString(), QString("function"));
        QVERIFY(did.value("unlinked").toBool());
        QVERIFY(did.value("merged").toBool());
        QVERIFY(did.value("removed").toBool());
        QVERIFY(did.value("gone").toBool());
        QVERIFY(!did.value("general").toBool());
        QCOMPARE(did.value("after").toArray(), QJsonArray{""});
        // What the plugin put in the general graph is there for everyone.
        QVERIFY(named(shelf.library, "item", "The Lamp").id > 0);
    }

    // -----------------------------------------------------------------------
    // When knowledge is revealed.

    void whatIsLearnedIsKeptWithItsMoment() {
        Shelf shelf;
        const auto document = shelf.document(shelf.book);
        const qint64 book = shelf.book;
        const auto upTo = [&](int section) { return GraphPlace{book, section, 1.0}; };
        QVERIFY(GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, std::nullopt, upTo(0)), firstAnswer).understood);
        QVERIFY(GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, upTo(0), upTo(1)), secondAnswer).understood);
        QVERIFY(GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, upTo(1), upTo(2)), thirdAnswer).understood);

        const Graph graph(shelf.library);
        const GraphNode corin = named(shelf.library, "character", "Corin Ash");
        // The node itself says what is known now...
        QCOMPARE(corin.summary, QString("Captain of the Tidewardens, and Old Tobin's son."));
        QVERIFY(corin.aliases().contains("Tobin's son"));
        // ...and every version is kept, each where it was revealed.
        QList<GraphFact> summaries, names;
        for (const GraphFact &fact : graph.facts(corin.id))
            (fact.kind == "summary" ? summaries : names) << fact;
        QCOMPARE(summaries.size(), 2);
        QCOMPARE(summaries.at(0).value, QString("Captain of the Tidewardens, after the drowned bell."));
        QCOMPARE(summaries.at(0).revealed.place.section, 1);
        QCOMPARE(summaries.at(1).revealed.place.section, 2);
        // At the words the model quoted, as the text has them.
        const QString third = document->text(2);
        QCOMPARE(summaries.at(1).revealed.anchor, QString("Corin Ash was Old Tobin's son."));
        QVERIFY(qAbs(summaries.at(1).revealed.place.fraction - double(third.indexOf("Corin Ash was")) / third.size()) < 1e-9);
        QCOMPARE(names.size(), 1);
        QCOMPARE(names.first().value, QString("Tobin's son"));
        QCOMPARE(names.first().revealed.place.section, 2);
        QCOMPARE(names.first().revealed.anchor, QString("Tobin's son"));
        // A quote with its spaces run together still finds its place, much
        // further down than where the bell is named.
        const GraphNode bell = named(shelf.library, "item", "The drowned bell");
        const GraphFact told = graph.facts(bell.id).last();
        QCOMPARE(told.revealed.anchor, QString("Mara learned it from the bell itself"));
        QVERIFY(qAbs(told.revealed.place.fraction - double(third.indexOf("Mara learned it")) / third.size()) < 1e-9);
        QVERIFY(told.revealed.place.fraction > summaries.at(1).revealed.place.fraction);
        // Where it was met stays where its name first showed, with the words.
        QCOMPARE(graph.firstMet(corin.id, book)->place.section, 1);
        QCOMPARE(graph.firstMet(corin.id, book)->anchor, QString("Corin Ash"));

        // A relation at the words quoted for it; one whose quote the text
        // doesn't hold, once both its ends have shown.
        GraphEdge son, speaks;
        for (const GraphEdge &edge : graph.around(corin.id, 2).edges) {
            if (edge.kind == "son of") son = edge;
            if (edge.kind == "speaks to") speaks = edge;
        }
        QCOMPARE(son.anchor, QString("was Old Tobin's son"));
        QCOMPARE(son.learned.section, 2);
        QCOMPARE(speaks.learned.section, 2);
        // (Neither end is named in that chapter as the graph knows it: at its
        // end, with no words to find it by.)
        QVERIFY(speaks.anchor.isEmpty());
        QCOMPARE(speaks.learned.fraction, 1.0);

        // The timeline, in the order a reader comes to it.
        QStringList steps;
        for (const GraphReveal &step : graph.timeline(corin.id, book))
            steps << QString("%1:%2").arg(step.what).arg(step.revealed.place.section);
        QCOMPARE(steps.first(), QString("met:1"));
        QVERIFY(steps.indexOf("summary:1") < steps.indexOf("summary:2"));
        QVERIFY(steps.contains("alias:2"));
        QVERIFY(steps.contains("relation:2"));
    }

    void aPlaceKnowsOnlyWhatWasRevealedByThen() {
        Shelf shelf;
        const auto document = shelf.document(shelf.book);
        const qint64 book = shelf.book;
        const auto upTo = [&](int section) { return GraphPlace{book, section, 1.0}; };
        GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, std::nullopt, upTo(0)), firstAnswer);
        GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, upTo(0), upTo(1)), secondAnswer);
        GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, upTo(1), upTo(2)), thirdAnswer);
        const Graph graph(shelf.library);
        const auto find = [](const GraphView &view, const QString &name) {
            const auto found = std::ranges::find(view.nodes, name, &GraphNode::name);
            return found == view.nodes.end() ? GraphNode() : *found;
        };
        const auto related = [](const GraphView &view, const QString &kind) {
            return std::ranges::any_of(view.edges, [&](const GraphEdge &edge) { return edge.kind == kind; });
        };

        // At the end of the second chapter: the summary of then, without
        // the name revealed later, and without the relation.
        const GraphView second = graph.ofBook(book, upTo(1));
        QCOMPARE(find(second, "Corin Ash").summary, QString("Captain of the Tidewardens, after the drowned bell."));
        QVERIFY(!find(second, "Corin Ash").aliases().contains("Tobin's son"));
        QVERIFY(!related(second, "son of"));
        QVERIFY(!QString::fromUtf8(QJsonDocument(second.toJson()).toJson()).contains("Tobin's son"));
        // Just before the words that reveal it: still not known.
        GraphFact reveal;
        for (const GraphFact &fact : graph.facts(find(second, "Corin Ash").id))
            if (fact.kind == "summary")
                reveal = fact;
        const GraphView before = graph.ofBook(book, GraphPlace{book, 2, reveal.revealed.place.fraction - 0.001});
        QCOMPARE(find(before, "Corin Ash").summary, QString("Captain of the Tidewardens, after the drowned bell."));
        // From those words on, it is.
        const GraphView third = graph.ofBook(book, upTo(2));
        QCOMPARE(find(third, "Corin Ash").summary, QString("Captain of the Tidewardens, and Old Tobin's son."));
        QVERIFY(find(third, "Corin Ash").aliases().contains("Tobin's son"));
        QVERIFY(related(third, "son of"));
        // A summary written by hand is the latest word, wherever the reader is.
        QVERIFY(Graph(shelf.library).edit(find(third, "Mara Venn").id, "Mara Venn", "The keeper, in my own words."));
        QCOMPARE(find(graph.ofBook(book, upTo(0)), "Mara Venn").summary, QString("The keeper, in my own words."));
        // A node no fact was kept of (made before facts were) is as it is.
        GraphNode old;
        old.id = 424242;
        old.summary = "As it always was.";
        QCOMPARE(Graph::asKnown(old, {}, book, [](const GraphPlace &) { return false; }).summary, QString("As it always was."));
        // A merge keeps the facts and their moments.
        const qint64 extra = Graph(shelf.library).upsert({}, "character", "The Captain", "Leads them.", {}, GraphPlace{book, 1, 0.9});
        QVERIFY(Graph(shelf.library).merge(find(third, "Corin Ash").id, extra));
        QVERIFY(std::ranges::any_of(graph.facts(find(third, "Corin Ash").id), [](const GraphFact &fact) { return fact.value == "Leads them."; }));
        const GraphNode captain = find(graph.ofBook(book, GraphPlace{book, 1, 0.5}), "Corin Ash");
        QVERIFY(captain.id > 0);
        QVERIFY(!captain.aliases().contains("The Captain"));
        QVERIFY(!captain.aliases().contains("Tobin's son"));
        QVERIFY(find(graph.ofBook(book, upTo(1)), "Corin Ash").aliases().contains("The Captain"));
    }

    void theSearchFindsBooksByWhatIsKnown() {
        Shelf shelf;
        const qint64 book = shelf.book, unopened = shelf.add("The Keeper Again");
        const auto upTo = [](qint64 id, int section) { return GraphPlace{id, section, 1.0}; };
        for (const qint64 id : {book, unopened}) {
            const auto document = shelf.document(id);
            GraphBuild::apply(shelf.library, id, GraphBuild::passages(*document, std::nullopt, upTo(id, 0)), firstAnswer);
            GraphBuild::apply(shelf.library, id, GraphBuild::passages(*document, upTo(id, 0), upTo(id, 1)), secondAnswer);
            GraphBuild::apply(shelf.library, id, GraphBuild::passages(*document, upTo(id, 1), upTo(id, 2)), thirdAnswer);
        }
        // Read to the end of the second chapter; the other book never opened.
        shelf.library.setProgress(book, {.place = {.section = 1, .fraction = 1.0}, .percent = 60, .updated = 0});
        const auto found = [&](const QString &word, bool deep = false) { return KnowledgeIndex::build(shelf.library).find(word, deep); };

        // A thing met by then finds its book, by a word of its name, and says so.
        QVERIFY(found("corin").contains(book));
        QCOMPARE(found("ash").value(book).name, QString("Corin Ash"));
        QCOMPARE(found("ash").value(book).kind, QString("character"));
        QCOMPARE(found("ash").value(book).met.place.section, 1);
        QVERIFY(!found("orin").contains(book));
        // Never a book not begun, however far its graph was built.
        QVERIFY(!found("corin").contains(unopened));
        // Nor a name, a summary or a thing from past the reader's page.
        QVERIFY(!found("tobin's son").contains(book));
        QVERIFY(found("drowned", true).contains(book));
        QVERIFY(!found("son", true).contains(book));
        // What the reader wrote by hand counts, wherever they are.
        QVERIFY(Graph(shelf.library).edit(named(shelf.library, "character", "Mara Venn").id, "Mara Venn", "The keeper, in my own words."));
        QVERIFY(found("words", true).contains(book));
        // And so does a question asked of a book, opened or not.
        GraphContext::noteQuestion(Graph::ofReader(shelf.library, unopened), unopened, std::nullopt, "Why does the lamp turn at night?", "It warns the ships.");
        QCOMPARE(found("lamp").value(unopened).kind, QString("question"));
        // A book marked as read is known to its end.
        QVERIFY(shelf.library.setReading(unopened, "read", 0, {}));
        QVERIFY(found("tobin's son").contains(unopened));

        // The search: details first, then what is known, with why.
        const QList<KnowledgeSearch::Found> hits = KnowledgeSearch::search(shelf.library, shelf.library.books(), "corin");
        QCOMPARE(hits.size(), 2);
        QCOMPARE(hits.first().why->name, QString("Corin Ash"));
        // Found by its title, a book needs no other reason.
        const QList<KnowledgeSearch::Found> byTitle = KnowledgeSearch::search(shelf.library, shelf.library.books(), "saltmere");
        QVERIFY(!byTitle.isEmpty() && byTitle.first().book.id == book && !byTitle.first().why);
        QCOMPARE(KnowledgeSearch::search(shelf.library, shelf.library.books(), "knows:corin -knows:tobin's").size(), 1);
        // Without the graph, knows: finds nothing; and it is no collection's rule.
        QVERIFY(!shelf.library.book(book)->matches("knows:corin"));
        QVERIFY(!Book::searchProblem("knows:corin").isEmpty());
        QVERIFY(Book::searchProblem("author:ann").isEmpty());
    }

    void nothingLaterReachesTheModel() {
        Shelf shelf;
        const auto document = shelf.document(shelf.book);
        const qint64 book = shelf.book;
        const auto upTo = [&](int section) { return GraphPlace{book, section, 1.0}; };
        GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, std::nullopt, upTo(0)), firstAnswer);
        GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, upTo(0), upTo(1)), secondAnswer);
        // The whole book was built (on request), but the reader is in the second chapter.
        GraphBuild::apply(shelf.library, book, GraphBuild::passages(*document, upTo(1), upTo(2)), thirdAnswer);
        shelf.library.setProgress(book, {.place = {.section = 1, .fraction = 0.9}, .percent = 60, .updated = 0});
        const GraphPlace here{book, 1, 0.9};

        const QString question = GraphContext::forQuestion(Graph::ofReader(shelf.library, book), book, here, "Who is Corin Ash, and what of the bell?");
        QVERIFY(question.contains("Corin Ash"));
        QVERIFY(question.contains("after the drowned bell"));
        QVERIFY(!question.contains("Tobin's son"));
        QVERIFY(!question.contains("son of"));
        QVERIFY(!question.contains("Told Mara"));
        const QString cast = GraphContext::digest(Graph::ofReader(shelf.library, book), book, here, {"character", "item"});
        QVERIFY(cast.contains("Corin Ash"));
        QVERIFY(!cast.contains("Tobin's son"));
        QVERIFY(!cast.contains("Told Mara"));
        // A question of the whole library goes by how far each book was read.
        const QString picture = GraphContext::picture(Graph::ofLibrary(shelf.library), "What is Corin doing?");
        QVERIFY(picture.contains("Corin Ash"));
        QVERIFY(!picture.contains("Tobin's son"));
        // And what the model is told when it notes the second chapter again.
        const QString asked = GraphBuild::request(shelf.library, book, GraphBuild::passages(*document, upTo(0), upTo(1))).messages.first().text;
        QVERIFY(asked.contains("Corin Ash"));
        QVERIFY(!asked.contains("Tobin's son"));
        QVERIFY(!asked.contains("Told Mara"));

        // The furthest the book was read stands when the reader goes back.
        shelf.library.setProgress(book, {.place = {.section = 0, .fraction = 0.1}, .percent = 5, .updated = 0});
        QCOMPARE(GraphContext::furthest(shelf.library, book).section, 1);
        QCOMPARE(GraphContext::spoilersAfter(shelf.library, book)->section, 1);
        QVERIFY(GraphContext::unspoiled(shelf.library)(GraphPlace{book, 1, 0.5}));
        QVERIFY(!GraphContext::unspoiled(shelf.library)(GraphPlace{book, 2, 0.1}));
        // A book marked as read has none.
        QVERIFY(shelf.library.setReading(book, "read", 0, {}));
        QVERIFY(!GraphContext::spoilersAfter(shelf.library, book));
    }

    void theTimelineFromTheCommandLineAndTheTools() {
        const Machine machine;
        QCOMPARE(machine.omabook({"add", machine.dir.filePath("story.txt")}).code, 0);
        qint64 corin = 0;
        {
            Library library(machine.library);
            QVERIFY(library.open());
            QString error;
            const auto document = BookOps::open(&library, "1", &error);
            QVERIFY(document);
            const auto upTo = [](int section) { return GraphPlace{1, section, 1.0}; };
            GraphBuild::apply(library, 1, GraphBuild::passages(*document, std::nullopt, upTo(0)), firstAnswer);
            GraphBuild::apply(library, 1, GraphBuild::passages(*document, upTo(0), upTo(1)), secondAnswer);
            GraphBuild::apply(library, 1, GraphBuild::passages(*document, upTo(1), upTo(2)), thirdAnswer);
            library.setProgress(1, {.place = {.section = 1, .fraction = 1.0}, .percent = 60, .updated = 0});
            corin = named(library, "character", "Corin Ash").id;
        }
        const QString id = QString::number(corin);
        Machine::Ran ran = machine.omabook({"graph", "node", id, "--timeline", "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        const QJsonArray steps = ran.json().value("timeline").toArray();
        QVERIFY(steps.size() >= 4);
        bool reveal = false;
        for (const QJsonValue &value : steps) {
            const QJsonObject step = value.toObject();
            if (step.value("what") == "summary" && step.value("text").toString().contains("Tobin's son")) {
                reveal = true;
                QCOMPARE(step.value("revealed").toObject().value("section").toInt(), 2);
                QCOMPARE(step.value("revealed").toObject().value("anchor").toString(), QString("Corin Ash was Old Tobin's son."));
                // Past the reader: said so.
                QVERIFY(step.value("ahead").toBool());
            }
        }
        QVERIFY(reveal);
        // As a reader at the end of the second chapter knows it: nothing later.
        ran = machine.omabook({"graph", "node", id, "--timeline", "--book", "1", "--upto", "1", "--json"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QVERIFY(!ran.output.contains("Tobin's son"));
        QCOMPARE(ran.json().value("node").toObject().value("summary").toString(), QString("Captain of the Tidewardens, after the drowned bell."));
        ran = machine.omabook({"graph", "node", id, "--timeline"});
        QVERIFY(ran.output.contains("When it was revealed:"));
        QVERIFY(ran.output.contains("section 2\tsummary"));
        QVERIFY(ran.output.contains("(ahead of the reader)"));
        // The same as a tool, with the same answer.
        Server server(machine);
        const QJsonObject tool = server.call("graph_node", {{"id", corin}, {"timeline", true}, {"book", 1}, {"up_to_section", 1}});
        QVERIFY(!QJsonDocument(tool).toJson().contains("Tobin's son"));
        QVERIFY(QJsonDocument(tool).toJson().contains("timeline"));
    }
};

QTEST_MAIN(GraphTests)
#include "graph_tests.moc"
