// The tenants of the knowledge graph in storage (ADR 0003, #94): each thing,
// relation and fact stored once, with the tenants that see it, derived from
// the books it came from; and a book's knowledge following it as it moves
// between collections (#97).

#include <QDir>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "changelog.h"
#include "fixtures.h"
#include "graph.h"
#include "library.h"
#include "librarymigrations.h"
#include "samples.h"
#include "settings.h"
#include "spacereplay.h"
#include "spaces.h"
#include "tenancy.h"

namespace {

// A library split in two at the top, Fiction and Study (with Physics inside
// it), a book in both, one in neither; an author who wrote in both, a theme
// both share, and what each book taught.
struct Split {
    QTemporaryDir dir;
    Library library{dir.filePath("Library")};
    qint64 sea = 0, both = 0, waves = 0, loose = 0;
    qint64 fiction = 0, study = 0;
    qint64 mara = 0, bell = 0, theme = 0, viscosity = 0, note = 0, ann = 0, bo = 0;
    qint64 keeps = 0, echoes = 0, wrote = 0, seaFact = 0, wavesFact = 0, noteFact = 0, bothFact = 0;

    Split() {
        library.open();
        sea = book("The Sea Wall", "Ann Author");
        both = book("Both Shores", "Ann Author");
        waves = book("Wave Mechanics", "Bo Writer");
        loose = book("Loose Leaves", "Cy Guide");
        fiction = *library.addCollection("Fiction");
        study = *library.addCollection("Study");
        const qint64 physics = *library.addCollection("Study/Physics");
        library.putInCollection(fiction, sea, true);
        library.putInCollection(fiction, both, true);
        library.putInCollection(physics, both, true);
        library.putInCollection(physics, waves, true);

        Graph graph(library);
        for (const qint64 each : {sea, both, waves, loose})
            graph.bookNode(each);
        mara = graph.upsert({}, "character", "Mara", "Walks the sea wall.", {}, GraphPlace{sea, 1, 0.5});
        bell = graph.upsert({}, "item", "The bell", {}, {}, GraphPlace{both, 0, 0.2});
        theme = graph.upsert({}, "theme", "Inheritance", {}, {}, GraphPlace{sea, 2, 0.1});
        graph.upsert({}, "theme", "Inheritance", {}, {}, GraphPlace{waves, 0, 0.3});
        seaFact = graph.learn(theme, "summary", "Handed from keeper to keeper.", GraphMoment{{sea, 2, 0.1}, "handed"});
        wavesFact = graph.learn(theme, "summary", "What a wave keeps of the one before.", GraphMoment{{waves, 0, 0.3}, "keeps"});
        viscosity = graph.upsert({}, "concept", "Viscosity", "How thick a flow is.", {}, GraphPlace{waves, 1, 0.0});
        note = graph.upsert({}, "topic", "Tides in general", "Put in by hand.");
        noteFact = graph.learn(note, "alias", "Tides");
        keeps = graph.link(mara, bell, "keeps", {}, GraphPlace{sea, 1, 0.6});
        echoes = graph.link(viscosity, mara, GraphEdge::acrossMark() + "echoes", {}, GraphPlace{waves, 1, 0.0});
        ann = graph.named({}, "author", "Ann Author");
        bo = graph.named({}, "author", "Bo Writer");
        wrote = edgeBetween(ann, graph.bookNode(sea));
        bothFact = graph.learn(bell, "alias", "The old bell", GraphMoment{{both, 0, 0.3}, "old bell"});
    }
    qint64 book(const QString &title, const QString &author) {
        BookMeta meta;
        meta.title = title;
        meta.authors = {author};
        return library.addEntry(meta, true).id;
    }
    [[nodiscard]] qint64 edgeBetween(qint64 source, qint64 target) const {
        for (const GraphEdge &edge : Graph(library).around(source, 1).edges)
            if (edge.source == source && edge.target == target)
                return edge.id;
        return 0;
    }
    [[nodiscard]] Tenant tenantOf(qint64 collection) const { return Tenant::ofCollection(Spaces::collectionId(library, collection)); }
    // These collections' tenants, in the order Tenancy::ofNode and the others answer them.
    [[nodiscard]] QList<Tenant> in(std::initializer_list<qint64> collections) const {
        QList<Tenant> all;
        for (const qint64 collection : collections)
            all << tenantOf(collection);
        std::ranges::sort(all, {}, &Tenant::globalId);
        return all;
    }
    // Sea Wall out of Fiction and into Study.
    [[nodiscard]] bool moveSeaToStudy() {
        return library.putInCollection(fiction, sea, false) && library.putInCollection(study, sea, true);
    }
    // Both Shores out of Physics, and so of Study: in Fiction alone.
    [[nodiscard]] bool takeBothOutOfStudy() { return library.putInCollection(library.collectionNamed("Study/Physics")->id, both, false); }
};

// Each row of `table` (tenant_nodes, tenant_edges, tenant_facts, tenant_books) in `tenant`.
[[nodiscard]] qint64 rowsIn(const Library &library, const QString &table, const Tenant &tenant) {
    QSqlQuery query =
        ChangeLog::query(library, QString("SELECT COUNT(*) FROM %1 WHERE tenant = ?").arg(table), {Tenancy::filter(library, tenant).id()});
    return query.next() ? query.value(0).toLongLong() : -1;
}

// The books of the facts `tenant` sees, apart from those every tenant sees.
[[nodiscard]] QSet<qint64> booksOfFactsIn(const Library &library, const Tenant &tenant) {
    QSet<qint64> books;
    for (QSqlQuery query =
             ChangeLog::query(library,
                              "SELECT COALESCE(f.book_id, 0) FROM graph_facts AS f JOIN tenant_facts AS m ON m.fact_id = f.id "
                              "WHERE m.tenant = ?",
                              {Tenancy::filter(library, tenant).id()});
         query.next();)
        books << query.value(0).toLongLong();
    return books;
}

// Every membership row, by the global ids of both sides, in a stable order:
// what two libraries that hold the same rows must agree on.
[[nodiscard]] QStringList memberships(const Library &library) {
    QStringList rows;
    const QList<std::pair<QString, QString>> tables{{"tenant_nodes", "node"}, {"tenant_edges", "edge"}, {"tenant_facts", "fact"}};
    for (const auto &[table, entity] : tables) {
        const QString column = entity + "_id";
        for (QSqlQuery query = ChangeLog::query(
                 library, QString("SELECT t.gid, m.%1 FROM %2 AS m JOIN tenants AS t ON t.id = m.tenant").arg(column, table));
             query.next();)
            rows << entity + " " + Spaces::globalId(library, entity, query.value(1).toLongLong()) + " " + query.value(0).toString();
    }
    for (QSqlQuery query = ChangeLog::query(library, "SELECT t.gid, m.book_id FROM tenant_books AS m JOIN tenants AS t ON t.id = m.tenant");
         query.next();)
        rows << "book " + Spaces::bookId(library, query.value(1).toLongLong()) + " " + query.value(0).toString();
    rows.sort();
    return rows;
}

// The membership after a rebuild, which recomputes it all from the books' tenants.
[[nodiscard]] QStringList rebuilt(const Library &library) {
    Tenancy::rebuild(library);
    return memberships(library);
}

} // namespace

class TenancyTests : public QObject {
    Q_OBJECT

private slots:
    void eachThingIsInTheTenantsOfTheBooksItCameFrom() {
        const Split split;
        const Library &library = split.library;
        const QList<Tenant> fiction = split.in({split.fiction}), study = split.in({split.study});
        const QList<Tenant> twoOf = split.in({split.fiction, split.study});
        QCOMPARE(Tenancy::ofBook(library, split.sea), fiction);
        QCOMPARE(Tenancy::ofBook(library, split.both), twoOf);
        // Through the collection inside Study.
        QCOMPARE(Tenancy::ofBook(library, split.waves), study);
        QCOMPARE(Tenancy::ofBook(library, split.loose), QList<Tenant>{Tenant::unfiled()});

        // A thing learned only from Fiction's books is only Fiction's.
        QCOMPARE(Tenancy::ofNode(library, split.mara), fiction);
        QCOMPARE(Tenancy::ofNode(library, split.viscosity), study);
        // Met in the book in both, it is in both.
        QCOMPARE(Tenancy::ofNode(library, split.bell), twoOf);
        // A theme two tenants' books share is one thing, seen by both, and
        // each of its facts only by the tenant of the book that said it.
        QCOMPARE(Tenancy::ofNode(library, split.theme), twoOf);
        QCOMPARE(Tenancy::ofFact(library, split.seaFact), fiction);
        QCOMPARE(Tenancy::ofFact(library, split.wavesFact), study);
        // An author who wrote in both is in both; one who wrote in Study only, in Study.
        QCOMPARE(Tenancy::ofNode(library, split.ann), twoOf);
        QCOMPARE(Tenancy::ofNode(library, split.bo), study);
        QCOMPARE(Tenancy::ofEdge(library, split.wrote), fiction);
        // A relation is its book's; across books, only where both its ends are: nowhere here.
        QCOMPARE(Tenancy::ofEdge(library, split.keeps), fiction);
        QCOMPARE(Tenancy::ofEdge(library, split.echoes), QList<Tenant>{});
        // What no book is behind is seen from every tenant, its facts with it.
        QCOMPARE(Tenancy::ofNode(library, split.note), QList<Tenant>{Tenant::everyone()});
        QCOMPARE(Tenancy::ofFact(library, split.noteFact), QList<Tenant>{Tenant::everyone()});
        const QList<Tenant> tenants = Tenancy::tenants(library);
        QCOMPARE(QSet<Tenant>(tenants.begin(), tenants.end()),
                 (QSet<Tenant>{Tenant::unfiled(), split.tenantOf(split.fiction), split.tenantOf(split.study)}));
    }

    void aNewFactFromABookInTwoTenantsIsInBoth() {
        Split split;
        Graph graph(split.library);
        const qint64 fact = graph.learn(split.mara, "alias", "The keeper", GraphMoment{{split.both, 3, 0.4}, "the keeper"});
        QVERIFY(fact > 0);
        QCOMPARE(Tenancy::ofFact(split.library, fact), split.in({split.fiction, split.study}));
        // And the thing it was said of is in both now.
        QCOMPARE(Tenancy::ofNode(split.library, split.mara), split.in({split.fiction, split.study}));
        // A thing put in by hand and then met in a book leaves "every tenant" for that book's.
        QVERIFY(graph.upsert({}, "topic", "Tides in general", {}, {}, GraphPlace{split.waves, 2, 0.0}) == split.note);
        QCOMPARE(Tenancy::ofNode(split.library, split.note), split.in({split.study}));
        QCOMPARE(Tenancy::ofFact(split.library, split.noteFact), split.in({split.study}));
    }

    void rebuildingAgreesWithWhatTheWritesKept() {
        Split split;
        const QStringList kept = memberships(split.library);
        QVERIFY(kept.size() > 20);
        Tenancy::rebuild(split.library);
        QCOMPARE(memberships(split.library), kept);
    }

    void aBookPutInATopCollectionIsInItsTenant() {
        Split split;
        Graph graph(split.library);
        const qint64 leaf = graph.upsert({}, "place", "The reading room", {}, {}, GraphPlace{split.loose, 0, 0.0});
        QCOMPARE(Tenancy::ofNode(split.library, leaf), QList<Tenant>{Tenant::unfiled()});
        // Writes follow the collections: in Study now, and no longer Unfiled.
        QVERIFY(split.library.putInCollection(split.study, split.loose, true));
        QCOMPARE(Tenancy::ofBook(split.library, split.loose), split.in({split.study}));
        QCOMPARE(Tenancy::ofNode(split.library, leaf), split.in({split.study}));
        // A new book in no collection is Unfiled at once, and so is what it teaches.
        const qint64 later = split.book("Later", "Di Writer");
        QCOMPARE(Tenancy::ofBook(split.library, later), QList<Tenant>{Tenant::unfiled()});
        const qint64 met = graph.upsert({}, "character", "Dee", {}, {}, GraphPlace{later, 0, 0.0});
        QCOMPARE(Tenancy::ofNode(split.library, met), QList<Tenant>{Tenant::unfiled()});
    }

    void oneTenantForTheWholeLibraryHoldsEverything() {
        Split split;
        Settings settings;
        settings.setGraphOneTenant(true);
        const auto restore = qScopeGuard([&settings, &split] {
            settings.setGraphOneTenant(false);
            Tenancy::follow(split.library);
        });
        Tenancy::follow(split.library);
        QCOMPARE(Tenancy::tenants(split.library), QList<Tenant>{Tenant::wholeLibrary()});
        const QList<Tenant> whole{Tenant::wholeLibrary()};
        for (const qint64 book : {split.sea, split.both, split.waves, split.loose})
            QCOMPARE(Tenancy::ofBook(split.library, book), whole);
        for (const qint64 node : {split.mara, split.bell, split.theme, split.viscosity, split.ann})
            QCOMPARE(Tenancy::ofNode(split.library, node), whole);
        QCOMPARE(Tenancy::ofEdge(split.library, split.echoes), whole);
        QCOMPARE(Tenancy::ofFact(split.library, split.seaFact), whole);
        // The name it had before 0.1.8 still says so; the new one, once written, wins.
        QTemporaryDir config;
        QVERIFY(Fixtures::write(config.filePath("config.json"), R"({"graph_across_collections": true})"));
        QVERIFY(Settings(config.filePath("config.json")).graphOneTenant());
        QVERIFY(Fixtures::write(config.filePath("config.json"), R"({"graph_across_collections": true, "graph_one_tenant": false})"));
        QVERIFY(!Settings(config.filePath("config.json")).graphOneTenant());
        // Back to tenants, everything is where it was.
        settings.setGraphOneTenant(false);
        Tenancy::follow(split.library);
        QCOMPARE(Tenancy::ofNode(split.library, split.mara), split.in({split.fiction}));
        QCOMPARE(Tenancy::ofEdge(split.library, split.echoes), QList<Tenant>{});
    }

    void aGraphKeepsToItsTenantWithTheFilter() {
        Split split;
        const Graph fiction = Graph::forTenant(split.library, split.tenantOf(split.fiction));
        QCOMPARE(fiction.tenant(), std::optional(split.tenantOf(split.fiction)));
        QVERIFY(!Graph(split.library).within());
        const TenantFilter within = *fiction.within();
        QSet<qint64> nodes;
        for (QSqlQuery query = ChangeLog::query(split.library, "SELECT n.id FROM graph_nodes AS n WHERE " + within.nodes(u"n.id"));
             query.next();)
            nodes << query.value(0).toLongLong();
        QVERIFY(nodes.contains(split.mara) && nodes.contains(split.theme) && nodes.contains(split.bell) && nodes.contains(split.note));
        QVERIFY(!nodes.contains(split.viscosity) && !nodes.contains(split.bo));
        QSet<qint64> facts;
        for (QSqlQuery query = ChangeLog::query(split.library, "SELECT f.id FROM graph_facts AS f WHERE " + within.facts(u"f.id"));
             query.next();)
            facts << query.value(0).toLongLong();
        QVERIFY(facts.contains(split.seaFact) && !facts.contains(split.wavesFact));
        QSet<qint64> books;
        for (QSqlQuery query =
                 ChangeLog::query(split.library, "SELECT DISTINCT s.book_id FROM graph_seen AS s WHERE " + within.books(u"s.book_id"));
             query.next();)
            books << query.value(0).toLongLong();
        QCOMPARE(books, (QSet<qint64>{split.sea, split.both}));
        // A tenant with no books here sees only what no book is behind.
        QCOMPARE(Tenancy::filter(split.library, Tenant::ofCollection("c:nowhere")).id(), -1);
    }

    void theMembershipIsDerivedWhereTheLogArrives() {
        // Nothing of it travels in the log: a library rebuilt from another's
        // log alone holds the same membership.
        Split split;
        const QList<SpaceOperation> ops = SpaceReplay::operations(split.library);
        QVERIFY(!ops.isEmpty());
        QTemporaryDir there;
        Library other(there.filePath("Library"));
        QVERIFY(other.open());
        QVERIFY(SpaceReplay::apply(other, ops));
        QCOMPARE(memberships(other), memberships(split.library));
        for (const SpaceOperation &op : ops)
            QVERIFY2(!op.entity.startsWith("tenant"), qPrintable(op.entity));
    }

    void aBookMovedToAnotherTenantTakesItsKnowledgeAlong() {
        Split split;
        const Library &library = split.library;
        QVERIFY(split.moveSeaToStudy());
        const QList<Tenant> fiction = split.in({split.fiction}), study = split.in({split.study});
        const QList<Tenant> twoOf = split.in({split.fiction, split.study});
        QCOMPARE(Tenancy::ofBook(library, split.sea), study);
        // What only it taught went with it, its facts and relations too.
        QCOMPARE(Tenancy::ofNode(library, split.mara), study);
        QCOMPARE(Tenancy::ofFact(library, split.seaFact), study);
        QCOMPARE(Tenancy::ofEdge(library, split.keeps), study);
        QCOMPARE(Tenancy::ofEdge(library, split.wrote), study);
        // A theme it shared with Study is Study's alone now; things Fiction's
        // other book is behind keep Fiction, and gain Study where the moved book was behind them.
        QCOMPARE(Tenancy::ofNode(library, split.theme), study);
        QCOMPARE(Tenancy::ofNode(library, split.bell), twoOf);
        QCOMPARE(Tenancy::ofNode(library, split.ann), twoOf);
        QCOMPARE(Tenancy::ofFact(library, split.bothFact), twoOf);
        // A connection across books whose ends are both in Study now is seen there.
        QCOMPARE(Tenancy::ofEdge(library, split.echoes), study);
        // Fiction keeps only the facts of the book still in it.
        QCOMPARE(booksOfFactsIn(library, fiction.first()), QSet<qint64>{split.both});
        // As a rebuild would have it.
        const QStringList kept = memberships(library);
        QCOMPARE(rebuilt(library), kept);
        // And back again.
        QVERIFY(split.library.putInCollection(split.study, split.sea, false)
                && split.library.putInCollection(split.fiction, split.sea, true));
        QCOMPARE(Tenancy::ofNode(library, split.mara), fiction);
        QCOMPARE(Tenancy::ofNode(library, split.theme), twoOf);
        QCOMPARE(Tenancy::ofEdge(library, split.echoes), QList<Tenant>{});
        const QStringList back = memberships(library);
        QCOMPARE(rebuilt(library), back);
    }

    void aBookInTwoCollectionsLeavingOneKeepsTheOther() {
        Split split;
        const Library &library = split.library;
        QVERIFY(split.takeBothOutOfStudy());
        const QList<Tenant> fiction = split.in({split.fiction}), study = split.in({split.study});
        QCOMPARE(Tenancy::ofBook(library, split.both), fiction);
        QCOMPARE(Tenancy::ofNode(library, split.bell), fiction);
        QCOMPARE(Tenancy::ofFact(library, split.bothFact), fiction);
        // Ann wrote nothing else in Study; the theme is still Study's by Wave Mechanics.
        QCOMPARE(Tenancy::ofNode(library, split.ann), fiction);
        QCOMPARE(Tenancy::ofNode(library, split.theme), split.in({split.fiction, split.study}));
        QCOMPARE(booksOfFactsIn(library, study.first()), QSet<qint64>{split.waves});
        const QStringList kept = memberships(library);
        QCOMPARE(rebuilt(library), kept);
    }

    void removingATopCollectionEmptiesItsTenant() {
        Split split;
        const Library &library = split.library;
        const Tenant fiction = split.tenantOf(split.fiction);
        QVERIFY(rowsIn(library, "tenant_nodes", fiction) > 0);
        QVERIFY(split.library.removeCollection(split.fiction));
        QCOMPARE(Tenancy::ofBook(library, split.sea), QList<Tenant>{Tenant::unfiled()});
        QCOMPARE(Tenancy::ofBook(library, split.both), split.in({split.study}));
        for (const char *table : {"tenant_books", "tenant_nodes", "tenant_edges", "tenant_facts"})
            QVERIFY2(rowsIn(library, table, fiction) == 0, table);
        QCOMPARE(Tenancy::ofNode(library, split.mara), QList<Tenant>{Tenant::unfiled()});
        QCOMPARE(Tenancy::ofEdge(library, split.keeps), QList<Tenant>{Tenant::unfiled()});
        QList<Tenant> studyAndUnfiled = split.in({split.study}) + QList<Tenant>{Tenant::unfiled()};
        std::ranges::sort(studyAndUnfiled, {}, &Tenant::globalId);
        QCOMPARE(Tenancy::ofNode(library, split.bell), studyAndUnfiled);
        const QStringList kept = memberships(library);
        QCOMPARE(rebuilt(library), kept);
    }

    void aMoveThatSyncsGivesTheSameMembershipThere() {
        Split split;
        QTemporaryDir there;
        Library other(there.filePath("Library"));
        QVERIFY(other.open());
        QVERIFY(SpaceReplay::apply(other, SpaceReplay::operations(split.library)));
        QCOMPARE(memberships(other), memberships(split.library));
        // The collections travel; the membership is derived from them there.
        QVERIFY(split.moveSeaToStudy());
        QVERIFY(split.takeBothOutOfStudy());
        QVERIFY(SpaceReplay::apply(other, SpaceReplay::operations(split.library)));
        const QStringList here = memberships(split.library);
        QCOMPARE(memberships(other), here);
        QCOMPARE(rebuilt(other), here);
    }

    void movingABookCostsWhatItTaughtNotTheLibrary() {
        // A library of 200 books in two tenants and 6000 things, each met in
        // two books, with a fact and a relation in the first; and a theme all share.
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 fiction = *library.addCollection("Fiction"), study = *library.addCollection("Study");
        constexpr int books = 200, things = 6000;
        qint64 moved = 0;
        {
            const Library::Batch batch(library);
            Graph graph(library);
            for (int each = 0; each < books; ++each) {
                BookMeta meta;
                meta.title = QString("Book %1").arg(each);
                meta.authors = {QString("Writer %1").arg(each % 40)};
                const qint64 book = library.addEntry(meta, true).id;
                moved = moved == 0 ? book : moved;
                QVERIFY(library.putInCollection(each % 2 == 0 ? fiction : study, book, true));
                graph.bookNode(book);
            }
            const QString ofBook = "JOIN books AS b ON b.id = (SELECT MIN(id) FROM books) + (n.id * %1) % %2 ";
            QVERIFY(
                ChangeLog::exec(library, QString("WITH RECURSIVE i(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM i WHERE x < %1) "
                                                 "INSERT INTO graph_nodes (kind, name, updated) SELECT 'concept', 'Thing ' || x, 0 FROM i")
                                             .arg(things)));
            for (const int step : {1, 7})
                QVERIFY(ChangeLog::exec(library,
                                        "INSERT OR IGNORE INTO graph_seen (node_id, book_id, section, fraction) SELECT n.id, b.id, 0, 0 "
                                        "FROM graph_nodes AS n "
                                            + ofBook.arg(step).arg(books) + "WHERE n.kind = 'concept'"));
            QVERIFY(ChangeLog::exec(library, "INSERT INTO graph_facts (node_id, kind, value, book_id) SELECT n.id, 'summary', 'Said', b.id "
                                             "FROM graph_nodes AS n "
                                                 + ofBook.arg(1).arg(books) + "WHERE n.kind = 'concept'"));
            QVERIFY(ChangeLog::exec(
                library, "INSERT INTO graph_edges (source, target, kind, book_id) SELECT n.id, n.id + 1, 'leads to', b.id "
                         "FROM graph_nodes AS n "
                             + ofBook.arg(1).arg(books) + "WHERE n.kind = 'concept' AND n.id + 1 IN (SELECT id FROM graph_nodes)"));
            const qint64 theme = graph.upsert({}, "theme", "Memory");
            QVERIFY(ChangeLog::exec(
                library, "INSERT OR IGNORE INTO graph_seen (node_id, book_id, section, fraction) SELECT ?, id, 0, 0 FROM books", {theme}));
        }
        QElapsedTimer clock;
        clock.start();
        QVERIFY(library.putInCollection(fiction, moved, false));
        const qint64 out = clock.restart();
        QVERIFY(library.putInCollection(study, moved, true));
        const qint64 in = clock.restart();
        const QStringList kept = memberships(library);
        clock.restart();
        Tenancy::rebuild(library);
        const qint64 whole = clock.elapsed();
        qInfo() << things << "things: a book out of its collection in" << out << "ms, into another in" << in << "ms; a rebuild" << whole
                << "ms";
        QVERIFY2(out < 100 && in < 100, qPrintable(QString("%1 ms, %2 ms").arg(out).arg(in)));
        QCOMPARE(memberships(library), kept);
    }

    void theSampleOfReleaseFiveIsSplitAsItIsUpdated() {
        QTemporaryDir dir;
        int format = 0;
        QDir().mkpath(dir.filePath("Library"));
        Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), dir.filePath("Library/omabook.db"), format);
        QVERIFY(format > 0 && format < LibraryMigrations::currentFormat());
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        // Copied first, as every step is.
        QVERIFY(!QDir(library.backupFolder()).entryList({"omabook-format-*.db"}).isEmpty());
        const Graph graph(library);
        const qint64 stories = library.collectionNamed("Stories")->id;
        const QList<Tenant> inStories{Tenant::ofCollection(Spaces::collectionId(library, stories))};
        // The Sea Wall is in Stories through The Coast inside it, A Paper Book by hand; One For Later in none.
        QCOMPARE(Tenancy::ofBook(library, 1), inStories);
        QCOMPARE(Tenancy::ofBook(library, 2), inStories);
        QCOMPARE(Tenancy::ofBook(library, 3), QList<Tenant>{Tenant::unfiled()});
        for (const auto &[kind, name] : QList<std::pair<QString, QString>>{{"book", "The Sea Wall"},
                                                                           {"author", "Ann Author"},
                                                                           {"series", "The Coast"},
                                                                           {"tag", "fiction"},
                                                                           {"author", "Bo Writer"},
                                                                           {"character", "Ann"},
                                                                           {"place", "The sea wall"}})
            QVERIFY2(Tenancy::ofNode(library, graph.named({}, kind, name)) == inStories, qPrintable(kind + " " + name));
        QCOMPARE(Tenancy::ofNode(library, graph.named("world:1", "character", "Ilse")), QList<Tenant>{Tenant::everyone()});
        for (const GraphFact &fact : graph.facts(graph.named({}, "character", "Ann")))
            QCOMPARE(Tenancy::ofFact(library, fact.id), inStories);
        // And it reads as it did: the graph's rows are as they were.
        QCOMPARE(graph.whole({}, 1000).nodes.size(), 9);
        // As the triggers would have kept it from the start.
        const QStringList made = memberships(library);
        Tenancy::rebuild(library);
        QCOMPARE(memberships(library), made);
    }
};

QTEST_GUILESS_MAIN(TenancyTests)
#include "tenancy_tests.moc"
