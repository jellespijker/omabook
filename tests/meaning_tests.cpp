// A book's text found by what it means (#72): cut in stretches, embedded by a
// stand-in for an embedding server, kept per book in the library, searched
// with its words, never past the reader's place, and made again when the
// book's file changes; the privacy rule, cancelling, tenants and the command
// line.

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "aifallback.h"
#include "booktext.h"
#include "embeddings.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "library.h"
#include "meaningindex.h"
#include "meaningsearch.h"
#include "settings.h"
#include "tenancy.h"
#include "textstretches.h"

using Fixtures::whyNot;

namespace {

// The stand-in's model of meaning: four ideas, each the words that mean it,
// and a little of a fifth that is everything else. "Cross the river" and
// "waded over the ford" share no word and mean the same.
const QList<QStringList> ideas{
    {"river", "ford", "water", "waded", "wade", "cross", "crossing", "bank", "stream", "lake", "swam"},
    {"bread", "market", "baker", "loaf", "bought", "flour"},
    {"dragon", "fire", "flames", "burned", "smoke"},
    {"letter", "wrote", "ink", "paper", "sealed"},
};

[[nodiscard]] QJsonArray meaningOf(const QString &text) {
    static const QRegularExpression word("\\w+");
    QList<double> vector(ideas.size() + 1, 0.0);
    vector.last() = 0.2;
    for (auto match = word.globalMatch(text.toLower()); match.hasNext();) {
        const QString found = match.next().captured();
        for (qsizetype idea = 0; idea < ideas.size(); ++idea)
            if (ideas.at(idea).contains(found))
                vector[idea] += 1;
    }
    QJsonArray numbers;
    for (const double number : vector)
        numbers.append(number);
    return numbers;
}

// Answers POST /embeddings as Ollama's OpenAI-compatible API does.
FakeServer::Reply embeddings(const FakeServer::Request &request) {
    QJsonArray data;
    const QJsonArray input = request.json().value("input").toArray();
    for (qsizetype at = 0; at < input.size(); ++at)
        data.append(QJsonObject{{"object", "embedding"}, {"index", at}, {"embedding", meaningOf(input.at(at).toString())}});
    return {.status = "200 OK", .body = QJsonDocument(QJsonObject{{"object", "list"}, {"data", data}}).toJson(QJsonDocument::Compact)};
}

const QList<Fixtures::Chapter> story{
    {"Market", "<p>In the morning Ana bought a loaf from the baker at the market, still warm from the oven.</p>"},
    {"The Ford", "<p>They waded over the ford at dawn, the cold water up to their knees, and climbed the far bank.</p>"},
    {"The Dragon", "<p>At night the dragon came down from the hills and the barns burned in its flames.</p>"},
};

// A library with the story in it, and the settings that send its text to the stand-in.
struct Shelf {
    QTemporaryDir dir;
    FakeServer server;
    Settings settings{dir.filePath("config.json")};
    Library library{dir.filePath("Library")};
    qint64 book = 0;

    explicit Shelf(const QList<Fixtures::Chapter> &chapters = story) {
        server.answering = embeddings;
        settings.setMeaningChoice("url", server.url());
        library.open();
        Fixtures::chapteredEpub(dir.filePath("Ferry.epub"), "The Ferry", chapters);
        book = library.add(dir.filePath("Ferry.epub")).id;
    }
    [[nodiscard]] Embeddings::Route route() const { return *Embeddings::route(settings); }
    void readTo(const BookPlace &place) { library.setProgress(book, Progress{.place = place, .percent = 50, .updated = 1}); }
    [[nodiscard]] QList<MeaningIndex::Hit> search(const QString &words, bool whole = false) const {
        return MeaningSearch::search(library, route(), {.words = words, .books = {book}, .whole = whole, .limit = 10})
            .value_or(QList<MeaningIndex::Hit>{});
    }
};

[[nodiscard]] bool about(const MeaningIndex::Hit &hit, const QString &word) { return hit.text.contains(word, Qt::CaseInsensitive); }

} // namespace

class MeaningTests : public QObject {
    Q_OBJECT

private slots:
    void aBookIsCutInStretchesThatShareTheirEdges() {
        QStringList words;
        for (int at = 0; at < 400; ++at)
            words << QString("w%1").arg(at);
        const BookText text(3, [&words](int section) { return section == 1 ? QString() : words.join("  "); });
        const auto stretches = TextStretches::of(text, 160, 40);
        // Each section by itself: 0-159, 120-279, 240-399; none for the empty one.
        QCOMPARE(stretches.size(), size_t{6});
        QCOMPARE(stretches.at(0).from, (BookPlace{.section = 0, .fraction = 0}));
        QVERIFY(stretches.at(1).text.startsWith("w120 w121"));
        QVERIFY(stretches.at(0).text.endsWith("w159"));
        QCOMPARE(stretches.at(2).to, (BookPlace{.section = 0, .fraction = 1}));
        QCOMPARE(stretches.at(3).from.section, 2);
        QVERIFY2(!stretches.at(0).text.contains("  "), "the spacing is made plain");
    }

    void aPassageIsFoundByWhatItMeansNeverPastTheReader() {
        Shelf shelf;
        QVERIFY(shelf.book > 0);
        const auto route = Embeddings::route(shelf.settings);
        QVERIFY2(route, qPrintable(whyNot(route)));
        QVERIFY(!route->hosted);
        QElapsedTimer took;
        took.start();
        const auto made = MeaningSearch::index(shelf.library, *route, shelf.book);
        QVERIFY2(made, qPrintable(whyNot(made)));
        qInfo("indexed %d stretches in %lld ms", *made, took.elapsed());
        QCOMPARE(*made, 3);
        QVERIFY(shelf.server.received.first().line.startsWith("POST /v1/embeddings"));
        // nomic-embed-text is the model unless one is chosen, and it is told what a text is.
        QCOMPARE(shelf.server.received.first().json().value("model").toString(), QString("nomic-embed-text"));
        QVERIFY(shelf.server.received.first().json().value("input").toArray().first().toString().startsWith("search_document: "));
        QCOMPARE(MeaningSearch::freshness(shelf.library, shelf.book, route->model), MeaningSearch::Freshness::Fresh);

        // The whole book: the ford, though no word of the question is in it.
        const QString question = "where do they cross the river?";
        const QList<MeaningIndex::Hit> whole = shelf.search(question, true);
        QVERIFY(!whole.isEmpty());
        QVERIFY2(about(whole.first(), "ford"), qPrintable(whole.first().text));
        QCOMPARE(whole.first().meaningRank, 0);
        QVERIFY(shelf.server.received.last().json().value("input").toArray().first().toString().startsWith("search_query: "));
        const BookPlace ford = whole.first().from;

        // Not begun: nothing. Before the ford: never the ford, nor anything past where the reader is.
        QVERIFY(shelf.search(question).isEmpty());
        shelf.readTo({.section = ford.section, .fraction = 0});
        const QList<MeaningIndex::Hit> before = shelf.search(question);
        QVERIFY(!before.isEmpty());
        for (const MeaningIndex::Hit &hit : before) {
            QVERIFY2(!about(hit, "ford"), qPrintable(hit.text));
            QVERIFY(hit.to <= (BookPlace{.section = ford.section, .fraction = 0}));
        }
        // Read past it: the ford comes first.
        shelf.readTo({.section = ford.section + 1, .fraction = 0});
        const QList<MeaningIndex::Hit> after = shelf.search(question);
        QVERIFY(!after.isEmpty());
        QVERIFY2(about(after.first(), "ford"), qPrintable(after.first().text));

        // The words as they were typed lift the stretch that has them.
        const QList<MeaningIndex::Hit> worded = shelf.search("loaf", true);
        QVERIFY(!worded.isEmpty());
        QVERIFY(about(worded.first(), "loaf"));
        QCOMPARE(worded.first().wordsRank, 0);
        QCOMPARE(MeaningSearch::toJson(worded.first()).value("by").toString(), QString("both"));
    }

    void theIndexIsMadeAgainWhenTheBooksFileChanges() {
        Shelf shelf;
        const Embeddings::Route route = shelf.route();
        QVERIFY(MeaningSearch::index(shelf.library, route, shelf.book));
        QVERIFY(MeaningSearch::due(shelf.library, shelf.settings, route.model).isEmpty());
        // A new edition: the crossing is now a swim across the lake.
        QList<Fixtures::Chapter> edition = story;
        edition[1].body = "<p>They swam across the lake at dawn and dried by a fire of driftwood.</p>";
        QVERIFY(Fixtures::chapteredEpub(shelf.dir.filePath("Ferry 2.epub"), "The Ferry", edition));
        QVERIFY(shelf.library.addFormat(shelf.book, shelf.dir.filePath("Ferry 2.epub"), Library::SameFormat::Overwrite));
        QCOMPARE(MeaningSearch::freshness(shelf.library, shelf.book, route.model), MeaningSearch::Freshness::Stale);
        // Asked for once, it is due again; another model is out of date too.
        QCOMPARE(MeaningSearch::due(shelf.library, shelf.settings, route.model), QList<qint64>{shelf.book});
        QCOMPARE(MeaningSearch::freshness(shelf.library, shelf.book, "another-model"), MeaningSearch::Freshness::Stale);
        QVERIFY(MeaningSearch::index(shelf.library, route, shelf.book));
        const QList<MeaningIndex::Hit> found = shelf.search("where do they cross the river?", true);
        QVERIFY(!found.isEmpty());
        QVERIFY2(about(found.first(), "lake"), qPrintable(found.first().text));
        QVERIFY(std::ranges::none_of(found, [](const MeaningIndex::Hit &hit) { return about(hit, "ford"); }));
        QCOMPARE(MeaningIndex::state(shelf.library, shelf.book)->stretches, 3);
        // A book taken out of the index is forgotten; one never asked for isn't due.
        QVERIFY(MeaningIndex::want(shelf.library, shelf.book, false));
        QVERIFY(!MeaningIndex::state(shelf.library, shelf.book));
        QVERIFY(MeaningSearch::due(shelf.library, shelf.settings, route.model).isEmpty());
        // Unless every book is to be indexed.
        shelf.settings.setMeaningAll(true);
        QCOMPARE(MeaningSearch::due(shelf.library, shelf.settings, route.model), QList<qint64>{shelf.book});
    }

    void aHostedServerIsSentNoTextUnlessTheSettingsAllowIt() {
        QTemporaryDir dir;
        Settings settings{dir.filePath("config.json")};
        settings.setMeaningChoice("url", "https://embeddings.example.com/v1");
        auto route = Embeddings::route(settings);
        QVERIFY(!route);
        QCOMPARE(route.error().code(), Failure::Code::MeaningHosted);
        QVERIFY(route.error().text().contains("embeddings.example.com"));
        // A model that runs elsewhere is someone else's computer, however near its server.
        settings.setMeaningChoice("url", "http://localhost:11434/v1");
        settings.setMeaningChoice("model", "nomic-embed-text:cloud");
        QVERIFY(!Embeddings::route(settings));
        // An address marked as one's own is.
        settings.setMeaningChoice("model", QJsonValue());
        settings.setMeaningChoice("url", "https://embeddings.example.com/v1");
        settings.setRecognition({{"own", QJsonArray{"embeddings.example.com"}}});
        QVERIFY(Embeddings::route(settings));
        QVERIFY(!Embeddings::route(settings)->hosted);
        // Allowed, it may be sent, and it is said to be hosted, which the user is told; with no
        // fallback (#174), as this address answers nothing here and this computer would stand in.
        AiFallback::choose(settings, Capability::Embeddings, {{QStringLiteral("to"), QStringLiteral("none")}});
        settings.setRecognition({});
        settings.setMeaningHosted(true);
        route = Embeddings::route(settings);
        QVERIFY2(route, qPrintable(whyNot(route)));
        QVERIFY(route->hosted);
        // Without one named, it is the AI server, Ollama on this computer unless chosen.
        settings.setMeaning({});
        QCOMPARE(Embeddings::route(settings)->server.baseUrl, QString("http://localhost:11434/v1"));
    }

    void indexingStopsWhenCancelledAndKeepsNothingNew() {
        QStringList words;
        for (int at = 0; at < 3000; ++at)
            words << (at % 7 == 0 ? "river" : "word");
        Shelf shelf({{"Long", "<p>" + words.join(' ') + "</p>"}});
        std::stop_source stop;
        QList<MeaningSearch::Progress> told;
        const auto made =
            MeaningSearch::index(shelf.library, shelf.route(), shelf.book, stop.get_token(), [&](MeaningSearch::Progress progress) {
                told << progress;
                if (progress.done > 0)
                    stop.request_stop();
            });
        QVERIFY(!made);
        QCOMPARE(made.error().code(), Failure::Code::MeaningCancelled);
        QCOMPARE(told.size(), 2);
        QCOMPARE(told.last().done, MeaningSearch::batch);
        QVERIFY(told.last().total > MeaningSearch::batch);
        QCOMPARE(shelf.server.received.size(), 2); // two batches sent, no third
        const auto state = MeaningIndex::state(shelf.library, shelf.book);
        QVERIFY(state && state->wanted && !state->built() && state->stretches == 0);
        // Again, to its end.
        QElapsedTimer took;
        took.start();
        const auto again = MeaningSearch::index(shelf.library, shelf.route(), shelf.book);
        QVERIFY2(again, qPrintable(whyNot(again)));
        qInfo("indexed %d stretches of a 3,000-word chapter in %lld ms", *again, took.elapsed());
        QCOMPARE(*again, told.last().total);
    }

    void aSearchOfTheLibraryKeepsToItsTenant() {
        Shelf shelf;
        QVERIFY(Fixtures::chapteredEpub(shelf.dir.filePath("Field.epub"), "Field Notes",
                                        {{"Rivers", "<p>The stream bank erodes where the water runs fastest at the crossing.</p>"}}));
        const qint64 notes = shelf.library.add(shelf.dir.filePath("Field.epub")).id;
        const qint64 fiction = *shelf.library.addCollection("Fiction");
        const qint64 study = *shelf.library.addCollection("Study");
        shelf.library.putInCollection(fiction, shelf.book, true);
        shelf.library.putInCollection(study, notes, true);
        Tenancy::follow(shelf.library);
        const Embeddings::Route route = shelf.route();
        QVERIFY(MeaningSearch::index(shelf.library, route, shelf.book));
        QVERIFY(MeaningSearch::index(shelf.library, route, notes));
        const auto tenant = Tenancy::chosen(shelf.library, "Study");
        QVERIFY2(tenant, qPrintable(whyNot(tenant)));
        const auto found = MeaningSearch::search(
            shelf.library, route,
            {.words = "crossing the river", .books = Tenancy::booksOf(shelf.library, *tenant), .whole = true, .limit = 10});
        QVERIFY2(found, qPrintable(whyNot(found)));
        QVERIFY(!found->isEmpty());
        QVERIFY(std::ranges::all_of(*found, [notes](const MeaningIndex::Hit &hit) { return hit.bookId == notes; }));
        // A book not indexed is asked about in vain, and said so.
        const auto none =
            MeaningSearch::search(shelf.library, route, {.words = "river", .books = {notes + 100}, .whole = true, .limit = 10});
        QVERIFY(!none);
        QCOMPARE(none.error().code(), Failure::Code::MeaningNotIndexed);
    }

    void theCommandLineIndexesAndSearchesByMeaning() {
        Shelf shelf;
        // The command reads the settings in use, as every command does.
        Settings inUse;
        inUse.setMeaningChoice("url", shelf.server.url());
        const auto run = [&](const QStringList &arguments) {
            QProcess process;
            QEventLoop loop;
            connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
            process.start(OMABOOK_BINARY, arguments + QStringList{"--library", shelf.library.root()});
            // The stand-in answers on this thread while the command waits.
            if (process.waitForStarted())
                loop.exec();
            return std::pair{process.exitCode(), process.readAllStandardOutput() + process.readAllStandardError()};
        };
        const auto [indexed, said] = run({"meaning", "index", QString::number(shelf.book)});
        QVERIFY2(indexed == 0, said.constData());
        QVERIFY2(said.contains("nomic-embed-text"), said.constData());
        const auto [searched, found] =
            run({"meaning", "search", QString::number(shelf.book), "cross", "the", "river", "--whole", "--json"});
        QVERIFY2(searched == 0, found.constData());
        const QJsonArray hits = QJsonDocument::fromJson(found).array();
        QVERIFY2(!hits.isEmpty(), found.constData());
        QVERIFY(hits.first().toObject().value("text").toString().contains("ford"));
        QCOMPARE(hits.first().toObject().value("by").toString(), QString("meaning"));
        // Not past where the reader is, without --whole.
        const auto [unread, nothing] = run({"meaning", "search", QString::number(shelf.book), "cross", "the", "river", "--json"});
        QCOMPARE(QJsonDocument::fromJson(nothing).array().size(), 0);
        QCOMPARE(unread, 1);
        const auto [listed, state] = run({"meaning", "--json"});
        QCOMPARE(listed, 0);
        QCOMPARE(QJsonDocument::fromJson(state).object().value("books").toArray().first().toObject().value("state").toString(),
                 QString("indexed"));
    }

    // How long a search of one book's stretches takes, measured over many:
    // every vector is read and measured, so it grows with them.
    void aSearchMeasuresEveryStretchQuickly() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "Measured";
        const qint64 book = library.addEntry(meta, true).id;
        constexpr int count = 20000;
        constexpr int dimensions = 768;
        std::vector<MeaningIndex::Stretch> stretches;
        stretches.reserve(count);
        auto *random = QRandomGenerator::global();
        for (int at = 0; at < count; ++at) {
            MeaningIndex::Vector vector(dimensions);
            std::ranges::generate(vector, [random] { return static_cast<float>(random->generateDouble() - 0.5); });
            MeaningIndex::normalize(vector);
            stretches.push_back({.from = {.section = at / 100, .fraction = 0},
                                 .to = {.section = at / 100, .fraction = 1},
                                 .text = QString("stretch %1").arg(at),
                                 .vector = std::move(vector)});
        }
        QElapsedTimer took;
        took.start();
        QVERIFY(MeaningIndex::store(library, book, "hash", "model", stretches));
        const qint64 stored = took.restart();
        const MeaningIndex::Scope scope{.books = {book}, .model = "model", .keeps = {}, .candidates = 50};
        const QList<MeaningIndex::Hit> hits = MeaningIndex::nearest(library, stretches.at(4321).vector, scope);
        const qint64 searched = took.elapsed();
        qInfo("stored %d stretches of %d dimensions in %lld ms; searched them in %lld ms", count, dimensions, stored, searched);
        QCOMPARE(hits.size(), 50);
        QCOMPARE(MeaningIndex::fused(library, hits, {}, 1).first().text, QString("stretch 4321"));
    }
};

QTEST_MAIN(MeaningTests)
#include "meaning_tests.moc"
