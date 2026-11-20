// The graph searched by its words (#102, format 31): Graph::find, which the
// AI context, the graph window's search and graph_find go through, asks the
// graph's full-text index (GraphIndex) instead of reading every thing, and
// finds what the old scan found where the two mean the same; it keeps to the
// tenant, with a thing both tenants see found by what this one was told;
// accents and scripts other than Latin are found as one would type them; and
// a change is found at once. The knows: index is brought up to date by
// reading again only what changed, and a search reads only what its words
// can find; both answer what reading it all again answers.

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <functional>
#include <ranges>

#include "changelog.h"
#include "fixtures.h"
#include "graph.h"
#include "graphindex.h"
#include "knowledgesearch.h"
#include "librarymigrations.h"
#include "samples.h"
#include "tenancy.h"
#include "tenants.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QList<qint64> idsOf(const QList<GraphNode> &nodes) {
    return nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>();
}

// What Graph::find answered before the index (up to format 27): a scan of
// every thing, each word anywhere in its name, summary or data; of two of a
// name, the one made first first.
[[nodiscard]] QList<qint64> scanned(const Library &library, const QString &words, const QString &kind, int limit) {
    QString where = u"scope = ''"_s;
    QVariantList values;
    if (!kind.isEmpty()) {
        where += u" AND kind = ?"_s;
        values << kind;
    }
    for (const QString &word : words.split(u' ', Qt::SkipEmptyParts)) {
        where += u" AND (name LIKE ? OR summary LIKE ? OR data LIKE ?)"_s;
        values << u"%"_s + word + u"%"_s << u"%"_s + word + u"%"_s << u"%"_s + word + u"%"_s;
    }
    QList<qint64> ids;
    for (QSqlQuery query = ChangeLog::query(
             library, u"SELECT id FROM graph_nodes WHERE %1 ORDER BY name COLLATE NOCASE, id LIMIT %2"_s.arg(where).arg(limit), values);
         query.next();)
        ids << query.value(0).toLongLong();
    return ids;
}

// A made-up word that begins with "q", a letter no other place in the words
// has: a word of a search that begins with "q" is found at the start of a
// word or nowhere, so the old scan and the index mean the same by it.
[[nodiscard]] QString madeUpWord(QRandomGenerator &random) {
    static const QString letters = [] {
        QString all;
        for (char16_t c = u'a'; c <= u'z'; ++c)
            if (c != u'q')
                all += QChar(c);
        return all;
    }();
    QString word = u"q"_s;
    for (int i = 0, length = 3 + static_cast<int>(random.bounded(5)); i < length; ++i)
        word += letters.at(static_cast<qsizetype>(random.bounded(static_cast<int>(letters.size()))));
    return word;
}

struct Generated {
    QTemporaryDir dir;
    Library library{dir.filePath(u"Library"_s)};
    QList<qint64> books;
    QStringList words;
    QList<qint64> things;

    // `count` things of the story kinds in `bookCount` books, each named with
    // two made-up words and said to be three more, met at places through
    // each book, some with another name or a later summary.
    Generated(int count, int bookCount) {
        library.open();
        QRandomGenerator random(102);
        for (int i = 0; i < 300; ++i)
            words << madeUpWord(random);
        const auto any = [&] { return words.at(static_cast<qsizetype>(random.bounded(static_cast<int>(words.size())))); };
        for (int i = 0; i < bookCount; ++i) {
            BookMeta meta;
            meta.title = u"Book %1"_s.arg(i);
            books << library.addEntry(meta, true).id;
        }
        const Library::Batch batch(library);
        Graph graph(library);
        const QStringList kinds = KnowledgeIndex::kinds().mid(0, 9);
        for (int i = 0; i < count; ++i) {
            const qint64 book = books.at(i % bookCount);
            const GraphPlace at{book, static_cast<int>(random.bounded(10)), random.bounded(1.0)};
            const QString name = any() + u" "_s + any();
            const qint64 id = graph.upsert({}, kinds.at(i % kinds.size()), name, any() + u" "_s + any() + u" "_s + any(), {}, at);
            things << id;
            if (i % 5 == 0)
                graph.learn(id, u"alias"_s, any(), GraphMoment{{book, at.section + 1, 0.5}, {}});
            if (i % 7 == 0)
                graph.learn(id, u"summary"_s, any() + u" later"_s, GraphMoment{{book, at.section + 2, 0.1}, {}});
            if (i % 11 == 0 && i > 0)
                graph.link(things.at(i - 1), id, u"knows"_s, {}, at);
        }
    }

    // The searches to compare: a word, the start of one, two words.
    [[nodiscard]] QStringList searches() const {
        QStringList asked;
        for (qsizetype i = 0; i < 60; ++i) {
            const QString &word = words.at(i * 5 % words.size());
            asked << word << word.left(4) << word + u" "_s + words.at((i * 5 + 1) % words.size()) << word.left(2);
        }
        return asked;
    }
};

// What an index finds for each of these words, in an order to compare.
[[nodiscard]] QStringList foundBy(const KnowledgeIndex &index, const QStringList &words, const QList<qint64> &books) {
    QStringList found;
    for (const QString &word : words)
        for (const bool deep : {false, true}) {
            const QHash<qint64, KnownMatch> hits = index.find(word, deep);
            QList<qint64> inOrder = hits.keys();
            std::ranges::sort(inOrder);
            for (const qint64 book : inOrder) {
                const KnownMatch &hit = hits.value(book);
                found << u"%1 %2 → %3 %4 %5 %6 %7 %8"_s.arg(word)
                             .arg(deep ? 1 : 0)
                             .arg(book)
                             .arg(hit.node)
                             .arg(hit.name, hit.kind)
                             .arg(hit.met.place.section)
                             .arg(hit.inSummary ? 1 : 0);
            }
        }
    for (const qint64 book : books)
        for (const KnownMatch &known : index.of(book, 25))
            found << u"of %1: %2 %3 %4"_s.arg(book).arg(known.node).arg(known.name).arg(known.met.place.section);
    return found;
}

} // namespace

class GraphSearchTests : public QObject {
    Q_OBJECT

private slots:
    // The SQLite the app is built with (Qt's QSQLITE, the system's library on
    // Arch) has FTS5, with the deletable contentless tables the index is.
    void theDatabaseHasFullTextSearch() {
        QTemporaryDir dir;
        Library library(dir.filePath(u"Library"_s));
        QVERIFY(library.open());
        QSqlQuery query = ChangeLog::query(library, u"SELECT sqlite_compileoption_used('ENABLE_FTS5'), sqlite_version()"_s);
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        const QStringList version = query.value(1).toString().split(u'.');
        QVERIFY2(version.value(0).toInt() > 3 || version.value(1).toInt() >= 43, qPrintable(query.value(1).toString()));
        QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
    }

    // A library from before the index has the graph it had indexed as it is
    // brought up to date, and is searched by it.
    void aLibraryFromBeforeIsIndexedAsItIsBroughtUpToDate() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(u"Old"_s));
        int format = 0;
        Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), dir.filePath(u"Old/omabook.db"_s), format);
        QVERIFY(format < LibraryMigrations::currentFormat());
        Library library(dir.filePath(u"Old"_s));
        QVERIFY(library.open());
        QSqlQuery counted = ChangeLog::query(
            library, u"SELECT (SELECT count(*) FROM graph_nodes), (SELECT count(*) FROM graph_words WHERE graph_words MATCH 'sea*')"_s);
        QVERIFY(counted.next() && counted.value(0).toInt() > 0 && counted.value(1).toInt() > 0);
        const QList<GraphNode> walks = Graph(library).find(u"walks"_s);
        QVERIFY(std::ranges::any_of(walks, [](const GraphNode &node) { return node.name == "Ann"_L1; }));
    }

    // On a graph of made-up words the index finds what the scan found, in
    // the same order, by kind and with a limit as the AI context asks.
    void theIndexFindsWhatTheScanFound() {
        const Generated made(2000, 20);
        const Graph graph(made.library);
        int compared = 0;
        for (const QString &asked : made.searches())
            for (const auto &[kind, limit] : {std::pair{QString(), 8}, std::pair{u"character"_s, 50}, std::pair{QString(), 1000}}) {
                const QList<qint64> before = scanned(made.library, asked, kind, limit);
                QCOMPARE(idsOf(graph.find(asked, kind, QString(), limit)), before);
                compared += before.isEmpty() ? 0 : 1;
            }
        QVERIFY(compared > 300);
    }

    // A word is a word's start, case and accents aside, in any script: not a
    // piece from inside a word, which the scan found.
    void wordsAreFoundAsTheyAreTyped() {
        QTemporaryDir dir;
        Library library(dir.filePath(u"Library"_s));
        QVERIFY(library.open());
        Graph graph(library);
        const qint64 mara = graph.upsert({}, u"character"_s, u"Mára Venn"_s, u"Keeps the lamp at Ærø."_s);
        const qint64 corin = graph.upsert({}, u"character"_s, u"Корин Пепел"_s, u"Сын смотрителя."_s);
        const qint64 sofia = graph.upsert({}, u"place"_s, u"σοφία"_s, u"Η πόλη."_s);
        const qint64 hindi = graph.upsert({}, u"concept"_s, u"हिन्दी किताब"_s);
        const auto found = [&graph](const QString &words) { return idsOf(graph.find(words)); };
        QCOMPARE(found(u"mara"_s), QList<qint64>{mara});
        QCOMPARE(found(u"MÁRA ven"_s), QList<qint64>{mara});
        QCOMPARE(found(u"aerø"_s), QList<qint64>{});
        QCOMPARE(found(u"ærø"_s), QList<qint64>{mara});
        QCOMPARE(found(u"корин"_s), QList<qint64>{corin});
        QCOMPARE(found(u"ПЕП"_s), QList<qint64>{corin});
        QCOMPARE(found(u"смотр"_s), QList<qint64>{corin});
        QCOMPARE(found(u"ΣΟΦΊΑ"_s), QList<qint64>{sofia});
        QCOMPARE(found(u"किताब"_s), QList<qint64>{hindi});
        // Not from inside a word.
        QVERIFY(found(u"enn"_s).isEmpty());
        // A word of no letter narrows nothing.
        QCOMPARE(found(u"mara -"_s), QList<qint64>{mara});
    }

    // A change is found at once: the index is kept as the graph is written.
    void aChangeIsFoundAtOnce() {
        QTemporaryDir dir;
        Library library(dir.filePath(u"Library"_s));
        QVERIFY(library.open());
        Graph graph(library);
        const qint64 id = graph.upsert({}, u"character"_s, u"Tobin Ash"_s);
        QCOMPARE(idsOf(graph.find(u"tobin"_s)), QList<qint64>{id});
        QVERIFY(graph.edit(id, u"Tobin Reed"_s, u"A ferryman."_s));
        QCOMPARE(idsOf(graph.find(u"reed ferry"_s)), QList<qint64>{id});
        QCOMPARE(idsOf(graph.find(u"ash"_s)), QList<qint64>{id}); // the old name stays one of its names
        graph.learn(id, u"summary"_s, u"Rows the drowned home."_s);
        QCOMPARE(idsOf(graph.find(u"drowned"_s)), QList<qint64>{id});
        QVERIFY(graph.remove(id));
        QVERIFY(graph.find(u"tobin"_s).isEmpty());
    }

    // A tenant finds a thing both see by what it was told of it (#95), never
    // by what only the other was told, and never the other's own.
    void aTenantFindsByWhatItWasTold() {
        const Split split;
        const Graph fiction = Graph::forTenant(split.library, split.tenantOf(split.fiction));
        const Graph study = Graph::forTenant(split.library, split.tenantOf(split.study));
        QCOMPARE(idsOf(fiction.find(u"handed"_s)), QList<qint64>{split.theme});
        QVERIFY(fiction.find(u"wave"_s).isEmpty());
        QVERIFY(idsOf(study.find(u"wave"_s)).contains(split.theme)); // and the book Wave Mechanics
        QVERIFY(study.find(u"handed"_s).isEmpty());
        QCOMPARE(idsOf(fiction.find(u"inheritance"_s)), QList<qint64>{split.theme});
        QVERIFY(fiction.find(u"viscosity"_s).isEmpty() && study.find(u"mara"_s).isEmpty());
        QCOMPARE(fiction.find(u"handed"_s).value(0).summary, u"Handed from keeper to keeper."_s);
    }

    // A word of a search is looked up in a few milliseconds, however many
    // things there are; the old scan read them all for each.
    void aWordIsFoundQuickly() {
        const Generated made(4000, 30);
        const Graph graph(made.library);
        QElapsedTimer took;
        took.start();
        int found = 0;
        for (const QString &word : made.words.mid(0, 100))
            found += static_cast<int>(graph.find(word, {}, QString(), 8).size());
        const qint64 each = took.elapsed() / 100;
        QVERIFY(found > 0);
        QVERIFY2(each < 20, qPrintable(u"%1 ms a word"_s.arg(each)));
    }

    // The knows: index brought up to date after each change answers what
    // reading it all again answers, and is quicker for it.
    void whatIsKnownIsBroughtUpToDate() {
        Generated made(1500, 15);
        const QList<qint64> &books = made.books;
        for (const qint64 book : books.mid(0, 10))
            made.library.setProgress(book, {.place = {.section = 5, .fraction = 0.5}, .percent = 50, .updated = 0});
        const QStringList words = made.words.mid(0, 80)
                                  + (made.words.mid(80, 40) | std::views::transform([](const QString &word) { return word.left(3); })
                                     | std::ranges::to<QStringList>());
        KnowledgeIndex index = KnowledgeIndex::build(made.library);
        QVERIFY(index.size() > 500);
        Graph graph(made.library);
        // Each a change the window sees: things learned, a name and a summary
        // given, a reader further on, a thing gone, a book read.
        const QList<std::function<void()>> changes{
            [&] { graph.upsert({}, u"character"_s, made.words.at(1) + u" newcomer"_s, {}, {}, GraphPlace{books.at(0), 1, 0.1}); },
            [&] { graph.learn(made.things.at(3), u"alias"_s, made.words.at(2) + u"son"_s, GraphMoment{{books.at(3), 0, 0.0}, {}}); },
            [&] { graph.learn(made.things.at(4), u"summary"_s, made.words.at(5) + u" again"_s, GraphMoment{{books.at(4), 0, 0.0}, {}}); },
            [&] { made.library.setProgress(books.at(2), {.place = {.section = 9, .fraction = 0.9}, .percent = 90, .updated = 0}); },
            [&] { made.library.setProgress(books.at(12), {.place = {.section = 3, .fraction = 0.2}, .percent = 30, .updated = 0}); },
            [&] { QVERIFY(graph.remove(made.things.at(6))); },
            [&] { graph.link(made.things.at(7), graph.bookNode(books.at(14)), u"appears in"_s, {}, GraphPlace{books.at(14), 0, 0.0}); },
            [&] { QVERIFY(made.library.setReading(books.at(11), u"read"_s, 0, {})); },
        };
        qint64 updating = 0, reading = 0;
        for (const auto &change : changes) {
            change();
            QElapsedTimer took;
            took.start();
            index = KnowledgeIndex::since(made.library, &index);
            updating += took.restart();
            const KnowledgeIndex whole = KnowledgeIndex::build(made.library);
            reading += took.elapsed();
            QCOMPARE(foundBy(index, words, books), foundBy(whole, words, books));
            QCOMPARE(index.changes, made.library.changes());
        }
        QVERIFY2(updating < reading, qPrintable(u"%1 ms brought up to date, %2 ms read whole"_s.arg(updating).arg(reading)));
        QVERIFY2(updating / changes.size() < 500, qPrintable(u"%1 ms brought up to date"_s.arg(updating)));
        // A newcomer is known at once, without reading it all again.
        QVERIFY(index.find(u"newcomer"_s).contains(books.at(0)));
        // With nothing changed, nothing is read again, and the same is known.
        const QStringList before = foundBy(index, words, books);
        QCOMPARE(foundBy(KnowledgeIndex::since(made.library, &index), words, books), before);
    }

    // Collections changed, it is read whole again, and knows what each tenant knows.
    void whatIsKnownIsReadAgainWhenTheTenantsChange() {
        Split split;
        for (const qint64 book : {split.sea, split.both, split.waves, split.loose})
            QVERIFY(split.library.setReading(book, u"read"_s, 0, {}));
        const KnowledgeIndex before = KnowledgeIndex::build(split.library);
        QVERIFY(split.moveSeaToStudy());
        const QList<qint64> books{split.sea, split.both, split.waves, split.loose};
        const QStringList words{u"mara"_s, u"bell"_s, u"inheritance"_s, u"handed"_s, u"wave"_s, u"viscosity"_s};
        QCOMPARE(foundBy(KnowledgeIndex::since(split.library, &before), words, books),
                 foundBy(KnowledgeIndex::build(split.library), words, books));
    }

    // A search reads only what its words can find, and finds what the whole index does.
    void aSearchReadsOnlyWhatItsWordsCanFind() {
        Generated made(1500, 15);
        for (const qint64 book : made.books)
            made.library.setProgress(book, {.place = {.section = 6, .fraction = 0.5}, .percent = 60, .updated = 0});
        const KnowledgeIndex whole = KnowledgeIndex::build(made.library);
        for (const QString &word : made.words.mid(0, 40)) {
            // Each search, and the word of it it asks of what is known.
            for (const auto &[query, asked] : {std::pair{word, word}, std::pair{u"knows:"_s + word.left(3), word.left(3)},
                                               std::pair{u"-"_s + word + u" OR title:x"_s, word}}) {
                const KnowledgeIndex part = KnowledgeIndex::forSearch(made.library, query);
                QVERIFY(part.size() < whole.size());
                QCOMPARE(foundBy(part, {asked}, {}), foundBy(whole, {asked}, {}));
            }
            const QList<KnowledgeSearch::Found> found = KnowledgeSearch::search(made.library, made.library.books(), u"knows:"_s + word);
            QCOMPARE(found.size(), whole.find(word, true).size());
        }
        // A word the index has no word for reads it all.
        QCOMPARE(KnowledgeIndex::forSearch(made.library, u"..."_s).size(), whole.size());
    }
};

QTEST_MAIN(GraphSearchTests)
#include "graphsearch_tests.moc"
