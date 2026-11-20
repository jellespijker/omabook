// The tenants of the knowledge graph in storage (ADR 0003, #94): each thing,
// relation and fact stored once, with the tenants that see it, derived from
// the books it came from.

#include <QDir>
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
    qint64 keeps = 0, echoes = 0, wrote = 0, seaFact = 0, wavesFact = 0, noteFact = 0;

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
};

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
