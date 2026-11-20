// Each thing's degree in each tenant (#101): the relations of it the tenant
// holds, kept by triggers through every write, the membership's changes and
// a rebuild; and the whole view, the graph window's first, read by it in the
// order the count it replaces gave.

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <ranges>

#include "changelog.h"
#include "fixtures.h"
#include "graph.h"
#include "graphrows.h"
#include "library.h"
#include "spacereplay.h"
#include "tenancy.h"
#include "tenants.h"

namespace {

// Each kept degree that differs from a count of the relations its tenant holds, as "tenant node kept counted".
[[nodiscard]] QStringList wrongDegrees(const Library &library) {
    QStringList wrong;
    for (QSqlQuery query = ChangeLog::query(library, "SELECT tenant, node_id, degree, c FROM (SELECT m.tenant, m.node_id, m.degree, "
                                                     "(SELECT COUNT(*) FROM graph_edges AS e WHERE (e.source = m.node_id OR "
                                                     "e.target = m.node_id) AND e.id IN (SELECT edge_id FROM tenant_edges "
                                                     "WHERE tenant = m.tenant)) AS c FROM tenant_nodes AS m) WHERE degree <> c");
         query.next();)
        wrong << QString("%1 %2 %3 %4")
                     .arg(query.value(0).toString(), query.value(1).toString(), query.value(2).toString(), query.value(3).toString());
    return wrong;
}

[[nodiscard]] qint64 relations(const Library &library) {
    QSqlQuery query = ChangeLog::query(library, "SELECT COUNT(*) FROM tenant_edges");
    return query.next() ? query.value(0).toLongLong() : 0;
}

// The things of `scope` as the whole view ordered them before the kept degree:
// by a count, for each thing, of the relations the tenant sees.
[[nodiscard]] QList<qint64> countedOrder(const Library &library, const Tenant &tenant, const QString &scope, int limit) {
    const TenantFilter within = Tenancy::filter(library, tenant);
    QList<qint64> ids;
    for (QSqlQuery query = ChangeLog::query(library,
                                            QString("SELECT id FROM graph_nodes WHERE scope = ? AND %1 ORDER BY (SELECT COUNT(*) FROM "
                                                    "graph_edges WHERE (source = graph_nodes.id OR target = graph_nodes.id) AND %2) "
                                                    "DESC, id LIMIT %3")
                                                .arg(within.nodes(u"graph_nodes.id"), within.edges(u"graph_edges.id"))
                                                .arg(limit),
                                            {scope});
         query.next();)
        ids << query.value(0).toLongLong();
    return ids;
}

[[nodiscard]] QList<qint64> idsOf(const GraphView &view) {
    return view.nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>();
}

// Whether every tenant's whole view, of the general graph and of a world, cut
// and not, is in the counted order; why not when it isn't.
[[nodiscard]] QString inCountedOrder(const Library &library) {
    for (const Tenant &tenant : Tenancy::tenants(library))
        for (const QString &scope : {QString(""), QString("world:1")})
            for (const int limit : {7, 100000}) {
                const QList<qint64> kept = idsOf(Graph::forTenant(library, tenant).whole(scope, limit));
                const QList<qint64> counted = countedOrder(library, tenant, scope, limit);
                if (kept != counted)
                    return QString("%1 in %2, %3: %4 things, counted %5")
                        .arg(scope, tenant.globalId())
                        .arg(limit)
                        .arg(kept.size())
                        .arg(counted.size());
            }
    return {};
}

// The plan SQLite makes of `sql`, its steps in order.
[[nodiscard]] QString plan(const Library &library, const QString &sql, const QVariantList &values) {
    QStringList steps;
    for (QSqlQuery query = ChangeLog::query(library, "EXPLAIN QUERY PLAN " + sql, values); query.next();)
        steps << query.value(3).toString();
    return steps.join(" | ");
}

// A library of `books` books, a third in each of two top-level collections
// and a few in both, with things met in them, in the general graph and in a
// world, related at random within a book and across books, and a few put in
// by hand that no book is behind.
struct Generated {
    QTemporaryDir dir;
    Library library{dir.filePath("Library")};
    QList<qint64> books, things;
    qint64 fiction = 0, study = 0;

    explicit Generated(int count, int perBook) {
        library.open();
        fiction = *library.addCollection("Fiction");
        study = *library.addCollection("Study");
        QRandomGenerator random(101);
        Graph graph(library);
        const Library::Batch batch(library);
        for (int i = 0; i < count; ++i) {
            BookMeta meta;
            meta.title = QString("Book %1").arg(i);
            meta.authors = {QString("Author %1").arg(i % 7)};
            const qint64 book = library.addEntry(meta, true).id;
            books << book;
            if (i % 3 == 0 || i % 10 == 1)
                library.putInCollection(fiction, book, true);
            if (i % 3 == 1 || i % 10 == 1)
                library.putInCollection(study, book, true);
            graph.bookNode(book);
            QList<qint64> own;
            for (int j = 0; j < perBook; ++j) {
                // Some shared between books, some a world's.
                const QString name =
                    random.bounded(3) == 0 ? QString("Shared %1").arg(random.bounded(count)) : QString("Thing %1.%2").arg(i).arg(j);
                own << graph.upsert(j % 5 == 4 ? "world:1" : "", "character", name, {}, {}, GraphPlace{book, j, 0.5});
            }
            for (int k = 0; k < perBook; ++k)
                graph.link(own.at(random.bounded(own.size())), own.at(random.bounded(own.size())), "knows", {}, GraphPlace{book, k, 0.6});
            if (!things.isEmpty())
                graph.link(own.first(), things.at(random.bounded(things.size())), GraphEdge::acrossMark() + "echoes", {},
                           GraphPlace{book, 0, 0.7});
            things << own;
        }
        // Put in by hand, of no tenant, related among themselves and to a thing of a book.
        QList<qint64> loose;
        for (int i = 0; i < 4; ++i)
            loose << graph.upsert({}, "topic", QString("Loose %1").arg(i), "By hand.");
        for (int i = 0; i < 4; ++i)
            graph.link(loose.at(i), loose.at((i + 1) % 4), "about");
        graph.link(loose.first(), things.first(), "about");
    }
};

} // namespace

class GraphDegreeTests : public QObject {
    Q_OBJECT

private slots:
    void aThingsDegreeFollowsItsRelationsInEachTenant() {
        Split split;
        Library &library = split.library;
        QVERIFY(relations(library) > 5);
        QCOMPARE(wrongDegrees(library), QStringList{});
        // Relations added, taken away, moved by a merge, and a thing removed.
        Graph graph(library);
        const qint64 again = graph.upsert({}, "character", "Mara's sister", {}, {}, GraphPlace{split.sea, 3, 0.1});
        QVERIFY(graph.link(again, split.mara, "sister of", {}, GraphPlace{split.sea, 3, 0.2}) > 0);
        QVERIFY(graph.link(split.theme, again, "carried by", {}, GraphPlace{split.waves, 2, 0.2}) > 0);
        QCOMPARE(wrongDegrees(library), QStringList{});
        QVERIFY(graph.unlink(split.keeps));
        QCOMPARE(wrongDegrees(library), QStringList{});
        QVERIFY(graph.merge(split.mara, again));
        QCOMPARE(wrongDegrees(library), QStringList{});
        QVERIFY(graph.remove(split.viscosity));
        QCOMPARE(wrongDegrees(library), QStringList{});
        // The membership changing as books move between tenants.
        QVERIFY(split.moveSeaToStudy());
        QCOMPARE(wrongDegrees(library), QStringList{});
        QVERIFY(split.takeBothOutOfStudy());
        QCOMPARE(wrongDegrees(library), QStringList{});
        QVERIFY(library.putInCollection(split.study, split.sea, false) && library.putInCollection(split.fiction, split.sea, true));
        QCOMPARE(wrongDegrees(library), QStringList{});
        // Two books made one, and a book removed (which rebuilds the membership).
        QVERIFY(library.merge(split.sea, split.both));
        QCOMPARE(wrongDegrees(library), QStringList{});
        QVERIFY(library.remove(split.waves).has_value());
        Tenancy::follow(library);
        QCOMPARE(wrongDegrees(library), QStringList{});
        Tenancy::rebuild(library);
        QCOMPARE(wrongDegrees(library), QStringList{});
    }

    void theDegreesAreCountedWhereTheLogArrives() {
        // A library made from another's log alone: the graph's rows replayed
        // (relations replaced where two say the same), and the membership
        // filled whole as it is set up.
        const Generated made(30, 4);
        QTemporaryDir there;
        Library other(there.filePath("Library"));
        QVERIFY(other.open());
        QVERIFY(SpaceReplay::apply(other, SpaceReplay::operations(made.library)));
        Tenancy::follow(other);
        QVERIFY(relations(other) > 30);
        QCOMPARE(wrongDegrees(other), QStringList{});
        QCOMPARE(inCountedOrder(other), QString());
    }

    void theWholeViewKeepsTheOrderTheCountGave() {
        Generated made(60, 6);
        Library &library = made.library;
        QCOMPARE(wrongDegrees(library), QStringList{});
        QCOMPARE(inCountedOrder(library), QString());
        // Through moves, a rebuild, and a book removed.
        for (const qint64 book : made.books | std::views::take(10)) {
            QVERIFY(library.putInCollection(made.fiction, book, false));
            QVERIFY(library.putInCollection(made.study, book, true));
        }
        QCOMPARE(wrongDegrees(library), QStringList{});
        QCOMPARE(inCountedOrder(library), QString());
        QVERIFY(library.remove(made.books.at(20)).has_value());
        Tenancy::follow(library);
        QCOMPARE(wrongDegrees(library), QStringList{});
        QCOMPARE(inCountedOrder(library), QString());
        Tenancy::rebuild(library);
        QCOMPARE(inCountedOrder(library), QString());
    }

    void theWholeViewIsARangeOfTheTenantsDegrees() {
        // A few thousand things, the window's 2,000 of them read in an
        // order the index gives, not counted and sorted.
        Generated made(1500, 5);
        const Library &library = made.library;
        const Tenant fiction = Tenant::ofCollection(Spaces::collectionId(library, made.fiction));
        const qint64 tenant = Tenancy::filter(library, fiction).id();
        const QString steps = plan(library, GraphRows::mostConnected(tenant, true, 2000), {QString("")});
        QVERIFY2(steps.startsWith("SEARCH m USING COVERING INDEX tenant_nodes_degree (tenant=?)"), qPrintable(steps));
        QVERIFY2(!steps.contains("TEMP B-TREE"), qPrintable(steps));
        const Graph graph = Graph::forTenant(library, fiction);
        QElapsedTimer clock;
        clock.start();
        const GraphView view = graph.whole({}, 2000);
        const qint64 took = clock.elapsed();
        // Their order alone, read by the kept degree and by the count it replaces.
        clock.restart();
        for (QSqlQuery query = ChangeLog::query(library, GraphRows::mostConnected(tenant, true, 2000), {QString("")}); query.next();) {
        }
        const qint64 kept = clock.elapsed();
        clock.restart();
        const QList<qint64> counted = countedOrder(library, fiction, "", 2000);
        const qint64 countedTook = clock.elapsed();
        QCOMPARE(idsOf(view), counted);
        qInfo() << view.nodes.size() << "things in the whole view in" << took << "ms; their order by the kept degree in" << kept
                << "ms, by the count it replaces in" << countedTook << "ms";
        QVERIFY2(took < 150, qPrintable(QString("the whole view took %1 ms").arg(took)));
    }
};

QTEST_MAIN(GraphDegreeTests)
#include "graphdegree_tests.moc"
