#include "tenancyschema.h"

#include <QString>

using namespace Qt::StringLiterals;

namespace {

// Whether the relation of `row` is a connection across books (GraphEdge::across).
[[nodiscard]] QString across(const QString &row) { return u"substr(%1.kind, 1, 8) = 'across: '"_s.arg(row); }

// A thing that stands for a book (Graph::bookNode).
[[nodiscard]] QString bookNode(const QString &row) { return u"(%1.kind = 'book' AND %1.book_id IS NOT NULL)"_s.arg(row); }

[[nodiscard]] QString trigger(const QString &name, const QString &when, const QStringList &body) {
    return u"CREATE TRIGGER IF NOT EXISTS %1 %2 BEGIN %3; END"_s.arg(name, when, body.join(u"; "_s));
}

// While Tenancy fills or rebuilds the tables whole, statement by statement,
// the triggers that follow one row at a time stand aside.
[[nodiscard]] QString notBulk() { return u"WHEN (SELECT value FROM library_state WHERE key = 'tenancy_bulk') = 0"_s; }

// The tenants `gids` (a SELECT of one column, g) names that are not there yet.
// Not INSERT OR IGNORE: a trigger takes the conflict clause of the statement
// that set it off, and a replay's INSERT OR REPLACE would replace a tenant,
// and with it every membership of it (#109).
[[nodiscard]] QString newTenants(const QString &gids) {
    return u"INSERT INTO tenants (gid) SELECT g FROM ("_s + gids + u") WHERE g NOT IN (SELECT gid FROM tenants)"_s;
}

// Where a relation (NEW) is, and what it gives its ends; `written`, by the
// rules of format 30 (#109), which keep what is written by hand in a tenant
// in it, and a relation said in other books in theirs too.
[[nodiscard]] QStringList placingEdge(bool written) {
    const QString inBook = u"NOT "_s + across(u"NEW"_s);
    // Written by hand in a tenant, it gives its ends nothing either.
    const QString notMade = written ? u" AND NOT "_s + TenancySchema::writtenHere(u"NEW"_s) : QString();
    const QString both = TenancySchema::tenantOfBoth(u"s"_s, u"t"_s);
    QStringList all{
        // Learned in a book: both ends are that book's, and so is the relation.
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT tenant, NEW.source FROM tenant_books WHERE book_id = NEW.book_id "
        "AND "_s
            + inBook,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT tenant, NEW.target FROM tenant_books WHERE book_id = NEW.book_id "
        "AND "_s
            + inBook,
        // Of no book, to a book's own node: the other end (an author, a
        // series, a tag, a question) is that book's.
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT t.tenant, NEW.target FROM graph_nodes AS b "
        "JOIN tenant_books AS t ON t.book_id = b.book_id WHERE b.id = NEW.source AND "_s
            + bookNode(u"b"_s) + u" AND NEW.book_id = 0 AND "_s + inBook + notMade
            + u" AND NOT EXISTS (SELECT 1 FROM graph_nodes AS o WHERE o.id = NEW.target AND ("_s + bookNode(u"o"_s) + u" OR "_s
            + TenancySchema::madeHere(u"o"_s) + u"))"_s,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT t.tenant, NEW.source FROM graph_nodes AS b "
        "JOIN tenant_books AS t ON t.book_id = b.book_id WHERE b.id = NEW.target AND "_s
            + bookNode(u"b"_s) + u" AND NEW.book_id = 0 AND "_s + inBook + notMade
            + u" AND NOT EXISTS (SELECT 1 FROM graph_nodes AS o WHERE o.id = NEW.source AND ("_s + bookNode(u"o"_s) + u" OR "_s
            + TenancySchema::madeHere(u"o"_s) + u"))"_s,
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT tenant, NEW.id FROM tenant_books WHERE book_id = NEW.book_id AND "_s
            + inBook,
        // Across books, or of no book: where both ends are (an end in tenant 0 is in each).
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT "_s + both + u", NEW.id FROM tenant_nodes AS s "
            "JOIN tenant_nodes AS t ON t.node_id = NEW.target AND "_s + TenancySchema::seenTogether(u"s"_s, u"t"_s)
            + u" WHERE s.node_id = NEW.source AND "_s
            + (written ? TenancySchema::whereEndsAre(u"NEW"_s, both)
                       : u"(NEW.book_id = 0 OR ("_s + across(u"NEW"_s) + u" AND "_s + both
                             + u" IN (SELECT tenant FROM tenant_books WHERE book_id = NEW.book_id)))"_s),
    };
    if (written) {
        // Written by hand in tenants: in each, and in the whole library's.
        const QString named = u"SELECT key AS g FROM json_each("_s + TenancySchema::notesOf(u"NEW"_s) + u", '$.tenants')"_s;
        const QString made = TenancySchema::writtenHere(u"NEW"_s);
        all << newTenants(named + u" UNION SELECT 'library' WHERE "_s + made)
            << u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT id, NEW.id FROM tenants WHERE "_s + made + u" AND (gid IN ("_s
                   + named + u") OR gid = 'library')"_s;
    }
    return all;
}

// Where a fact (NEW) is, and what it gives its thing; `written`, as for a relation.
[[nodiscard]] QStringList placingFact(bool written) {
    const QString ofNoBook = u"COALESCE(NEW.book_id, 0) = 0"_s;
    QStringList all{
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT tenant, NEW.node_id FROM tenant_books WHERE book_id = NEW.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_facts (tenant, fact_id) SELECT tenant, NEW.id FROM tenant_books WHERE book_id = NEW.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_facts (tenant, fact_id) SELECT tenant, NEW.id FROM tenant_nodes WHERE node_id = NEW.node_id AND "_s
            + ofNoBook + (written ? u" AND NEW.tenant = ''"_s : QString()),
    };
    if (written) {
        // Written by hand in a tenant: in it, and in the whole library's.
        const QString made = ofNoBook + u" AND NEW.tenant <> ''"_s;
        all << newTenants(u"SELECT NEW.tenant AS g WHERE "_s + made + u" UNION SELECT 'library' WHERE "_s + made)
            << u"INSERT OR IGNORE INTO tenant_facts (tenant, fact_id) SELECT id, NEW.id FROM tenants WHERE "_s + made
                   + u" AND gid IN (NEW.tenant, 'library')"_s;
    }
    return all;
}

// A thing in a tenant (NEW): no longer of no book, and what hangs on it alone
// goes with it; `written`, not what was written by hand in another tenant.
[[nodiscard]] QStringList followingNode(bool written) {
    // The tenant a relation of it is in with its other end (`o`): this one,
    // or for a thing in tenant 0, which every tenant sees, the other end's.
    const QString both = u"CASE WHEN NEW.tenant = 0 THEN o.tenant ELSE NEW.tenant END"_s;
    const QString together = u"(o.tenant = NEW.tenant OR o.tenant = 0 OR NEW.tenant = 0)"_s;
    const QString shared = written ? TenancySchema::whereEndsAre(u"e"_s, both)
                                   : u"(e.book_id = 0 OR ("_s + across(u"e"_s) + u" AND "_s + both
                                         + u" IN (SELECT tenant FROM tenant_books WHERE book_id = e.book_id)))"_s;
    // In a tenant it had a relation in only through tenant 0, it no longer is:
    // the relation is not there either, unless written there by hand.
    const QStringList gone = written ? QStringList{u"DELETE FROM tenant_edges WHERE NEW.tenant <> 0 AND tenant NOT IN (0, NEW.tenant) "
                                               "AND tenant NOT IN (SELECT tenant FROM tenant_nodes WHERE node_id = NEW.node_id) "
                                               "AND edge_id IN (SELECT e.id FROM graph_edges AS e WHERE (e.source = NEW.node_id OR "
                                               "e.target = NEW.node_id) AND NOT "_s
                                               + TenancySchema::writtenHere(u"e"_s) + u")"_s}
                                 : QStringList();
    return gone + QStringList{
        u"DELETE FROM tenant_nodes WHERE NEW.tenant <> 0 AND tenant = 0 AND node_id = NEW.node_id"_s,
        u"DELETE FROM tenant_facts WHERE NEW.tenant <> 0 AND tenant = 0 AND fact_id IN (SELECT id FROM graph_facts WHERE node_id = "
        "NEW.node_id)"_s,
        u"DELETE FROM tenant_edges WHERE NEW.tenant <> 0 AND tenant = 0 AND edge_id IN (SELECT id FROM graph_edges WHERE source = "
        "NEW.node_id UNION ALL SELECT id FROM graph_edges WHERE target = NEW.node_id)"_s,
        u"INSERT OR IGNORE INTO tenant_facts (tenant, fact_id) SELECT NEW.tenant, id FROM graph_facts WHERE node_id = NEW.node_id "
        "AND COALESCE(book_id, 0) = 0"_s
            + (written ? u" AND tenant = ''"_s : QString()),
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT "_s + both + u", e.id FROM graph_edges AS e "
            "JOIN tenant_nodes AS o ON o.node_id = e.target AND "_s + together + u" WHERE e.source = NEW.node_id AND "_s + shared,
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT "_s + both + u", e.id FROM graph_edges AS e "
            "JOIN tenant_nodes AS o ON o.node_id = e.source AND "_s + together + u" WHERE e.target = NEW.node_id AND "_s + shared,
    };
}

// A book in a tenant (NEW): what was learned from it is in it too; `written`,
// a relation it said that another book is behind, where both ends are.
[[nodiscard]] QStringList followingBook(bool written) {
    const QString inBook = u"NOT "_s + across(u"e"_s);
    const QString tied = inBook + (written ? u" AND NOT "_s + TenancySchema::writtenHere(u"e"_s) : QString());
    QStringList all{
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT NEW.tenant, id FROM graph_nodes WHERE book_id = NEW.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT NEW.tenant, node_id FROM graph_seen WHERE book_id = NEW.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT NEW.tenant, node_id FROM graph_facts WHERE book_id = NEW.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT NEW.tenant, e.source FROM graph_edges AS e WHERE e.book_id = "
        "NEW.book_id AND "_s
            + inBook + u" UNION SELECT NEW.tenant, e.target FROM graph_edges AS e WHERE e.book_id = NEW.book_id AND "_s + inBook,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT NEW.tenant, e.target FROM graph_nodes AS b "
        "JOIN graph_edges AS e ON e.source = b.id JOIN graph_nodes AS o ON o.id = e.target WHERE b.book_id = NEW.book_id AND "_s
            + bookNode(u"b"_s) + u" AND e.book_id = 0 AND "_s + tied + u" AND NOT "_s + bookNode(u"o"_s) + u" AND NOT "_s
            + TenancySchema::madeHere(u"o"_s)
            + u" UNION SELECT NEW.tenant, e.source FROM graph_nodes AS b JOIN graph_edges AS e ON e.target = b.id "
              "JOIN graph_nodes AS o ON o.id = e.source WHERE b.book_id = NEW.book_id AND "_s
            + bookNode(u"b"_s) + u" AND e.book_id = 0 AND "_s + tied + u" AND NOT "_s + bookNode(u"o"_s) + u" AND NOT "_s
            + TenancySchema::madeHere(u"o"_s),
        u"INSERT OR IGNORE INTO tenant_facts (tenant, fact_id) SELECT NEW.tenant, id FROM graph_facts WHERE book_id = NEW.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT NEW.tenant, e.id FROM graph_edges AS e WHERE e.book_id = "
        "NEW.book_id AND "_s
            + inBook,
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT NEW.tenant, e.id FROM graph_edges AS e "
        "JOIN tenant_nodes AS s ON s.tenant = NEW.tenant AND s.node_id = e.source "
        "JOIN tenant_nodes AS t ON t.tenant = NEW.tenant AND t.node_id = e.target WHERE e.book_id = NEW.book_id AND "_s
            + across(u"e"_s),
    };
    if (written)
        // Said in it, at the things met in it.
        for (const auto &[end, other] : {std::pair{u"source"_s, u"target"_s}, std::pair{u"target"_s, u"source"_s}})
            all << u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT NEW.tenant, e.id FROM graph_seen AS m "
                   "JOIN graph_edges AS e ON e.%1 = m.node_id JOIN tenant_nodes AS s ON s.tenant IN (NEW.tenant, 0) AND s.node_id = e.%2 "
                   "JOIN tenant_nodes AS t ON t.tenant IN (NEW.tenant, 0) AND t.node_id = e.%1 WHERE m.book_id = NEW.book_id AND "_s.arg(end,
                                                                                                                                   other)
                       + TenancySchema::saidIn(u"e"_s, u"NEW.book_id"_s);
    return all;
}

// The rows of one kind in a tenant: led by the tenant, so a reader of one
// tenant reads a range; and by the row, for what follows a row and for the
// row's removal (a cascade).
[[nodiscard]] QStringList membership(const QString &table, const QString &column, const QString &rows) {
    return {u"CREATE TABLE IF NOT EXISTS %1 (tenant INTEGER NOT NULL REFERENCES tenants(id) ON DELETE CASCADE, "
            "%2 INTEGER NOT NULL REFERENCES %3(id) ON DELETE CASCADE, PRIMARY KEY (tenant, %2)) WITHOUT ROWID"_s.arg(table, column, rows),
            u"CREATE INDEX IF NOT EXISTS %1_of ON %1(%2)"_s.arg(table, column)};
}

// A book's node is its book's.
[[nodiscard]] QString ofBookNode() {
    return u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT tenant, NEW.id FROM tenant_books WHERE book_id = NEW.book_id"_s;
}

// The data of a thing (NEW), as JSON functions take it.
[[nodiscard]] QString dataOf() { return u"CASE WHEN json_valid(NEW.data) THEN NEW.data ELSE '{}' END"_s; }

// A thing made by hand in tenants (NEW): in each it names, and in the whole
// library's, which a library kept as one tenant reads.
[[nodiscard]] QStringList madeIn() {
    const QString named = u"SELECT value FROM json_each("_s + dataOf() + u", '$.tenants') WHERE type = 'text'"_s;
    const QString made = TenancySchema::madeHere(u"NEW"_s);
    return {
        u"INSERT OR IGNORE INTO tenants (gid) "_s + named,
        u"INSERT OR IGNORE INTO tenants (gid) SELECT 'library' WHERE "_s + made,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT id, NEW.id FROM tenants WHERE "_s + made + u" AND (gid IN ("_s + named
            + u") OR gid = 'library')"_s,
    };
}

// A new thing: of no book until one is behind it, a book's node its book's,
// and one made by hand in a tenant that tenant's.
[[nodiscard]] QStringList placingNode() {
    return QStringList{u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) VALUES (0, NEW.id)"_s, ofBookNode()} + madeIn();
}

[[nodiscard]] QStringList statements() {
    QStringList all{
        // A tenant by its global id: a top-level collection's (Spaces::collectionId),
        // "unfiled", "library" (one for the whole library) or "any" (0, what no book is behind).
        u"CREATE TABLE IF NOT EXISTS tenants (id INTEGER PRIMARY KEY, gid TEXT NOT NULL UNIQUE)"_s,
        u"INSERT OR IGNORE INTO tenants (id, gid) VALUES (0, 'any')"_s,
        // Whether Tenancy last placed the books as one tenant (1) or by their
        // top-level collections (0), -1 never; the books counter then; its
        // switch for filling the tables whole; and whether a book left a tenant since.
        u"INSERT OR IGNORE INTO library_state (key, value) VALUES ('tenancy_one', -1), ('tenancy_books', -1), ('tenancy_bulk', 0), "
        "('tenancy_shrunk', 0)"_s,
    };
    all << membership(u"tenant_books"_s, u"book_id"_s, u"books"_s) << membership(u"tenant_nodes"_s, u"node_id"_s, u"graph_nodes"_s)
        << membership(u"tenant_edges"_s, u"edge_id"_s, u"graph_edges"_s) << membership(u"tenant_facts"_s, u"fact_id"_s, u"graph_facts"_s);
    all << trigger(u"graph_nodes_tenant"_s, u"AFTER INSERT ON graph_nodes"_s, placingNode())
        << trigger(u"graph_nodes_book_tenant"_s, u"AFTER UPDATE OF book_id ON graph_nodes"_s, {ofBookNode()});
    const QString seen = u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT tenant, NEW.node_id FROM tenant_books "
                         "WHERE book_id = NEW.book_id"_s;
    all << trigger(u"graph_seen_tenant"_s, u"AFTER INSERT ON graph_seen"_s, {seen})
        << trigger(u"graph_seen_moved_tenant"_s, u"AFTER UPDATE OF node_id, book_id ON graph_seen"_s, {seen});
    all << trigger(u"graph_facts_tenant"_s, u"AFTER INSERT ON graph_facts"_s, placingFact(false))
        << trigger(u"graph_facts_moved_tenant"_s, u"AFTER UPDATE OF node_id, book_id ON graph_facts"_s,
                   QStringList{u"DELETE FROM tenant_facts WHERE fact_id = NEW.id"_s} + placingFact(false));
    all << trigger(u"graph_edges_tenant"_s, u"AFTER INSERT ON graph_edges"_s, placingEdge(false))
        << trigger(u"graph_edges_moved_tenant"_s, u"AFTER UPDATE OF source, target, kind, book_id ON graph_edges"_s,
                   QStringList{u"DELETE FROM tenant_edges WHERE edge_id = NEW.id"_s} + placingEdge(false));
    all << trigger(u"tenant_nodes_follow"_s, u"AFTER INSERT ON tenant_nodes "_s + notBulk(), followingNode(false))
        << trigger(u"tenant_books_follow"_s, u"AFTER INSERT ON tenant_books "_s + notBulk(), followingBook(false))
        << trigger(u"tenant_books_left"_s, u"AFTER DELETE ON tenant_books"_s,
                   {u"UPDATE library_state SET value = 1 WHERE key = 'tenancy_shrunk'"_s});
    return all;
}

// While Tenancy fills the tables whole (tenancy_bulk 2) the degrees are
// counted once afterwards (TenancySchema::countDegrees), not a row at a time.
[[nodiscard]] QString notWhole() { return u"WHEN (SELECT value FROM library_state WHERE key = 'tenancy_bulk') <> 2"_s; }

// The things a relation in a tenant (`row`, NEW or OLD of tenant_edges) counts for there: its ends, once each.
[[nodiscard]] QString endsOf(const QString &row) {
    return u"tenant = %1.tenant AND node_id IN (SELECT source FROM graph_edges WHERE id = %1.edge_id "
           "UNION SELECT target FROM graph_edges WHERE id = %1.edge_id)"_s.arg(row);
}

// The relations of `node` that `tenant` holds, counted through the relations'
// indexes by end: what a thing's degree there is.
[[nodiscard]] QString degreeOf(const QString &tenant, const QString &node) {
    return u"(SELECT COUNT(*) FROM graph_edges AS e WHERE (e.source = %2 OR e.target = %2) "
           "AND EXISTS (SELECT 1 FROM tenant_edges WHERE tenant = %1 AND edge_id = e.id))"_s.arg(tenant, node);
}

[[nodiscard]] QStringList degreeStatements() {
    // Another relation of the same ends and kind (NEW's): the one an INSERT OR
    // REPLACE puts it in place of, and an UPDATE OR IGNORE leaves it beside.
    const QString same = u"SELECT id FROM graph_edges WHERE source = NEW.source AND target = NEW.target AND kind = NEW.kind"_s;
    return {
        // tenant_nodes made again with a degree (SQLite's way of changing a
        // table, so that the step can be taken again where it was). The
        // triggers of other tables name it, and keep doing so through the
        // rename only as SQLite renamed before 3.26; its own go with it, and
        // are made again.
        u"DROP TABLE IF EXISTS tenant_nodes_degreed"_s,
        u"CREATE TABLE tenant_nodes_degreed (tenant INTEGER NOT NULL REFERENCES tenants(id) ON DELETE CASCADE, "
        "node_id INTEGER NOT NULL REFERENCES graph_nodes(id) ON DELETE CASCADE, degree INTEGER NOT NULL DEFAULT 0, "
        "PRIMARY KEY (tenant, node_id)) WITHOUT ROWID"_s,
        u"INSERT INTO tenant_nodes_degreed (tenant, node_id) SELECT tenant, node_id FROM tenant_nodes"_s,
        u"DROP TABLE tenant_nodes"_s,
        u"PRAGMA legacy_alter_table = ON"_s,
        u"ALTER TABLE tenant_nodes_degreed RENAME TO tenant_nodes"_s,
        u"PRAGMA legacy_alter_table = OFF"_s,
        u"CREATE INDEX IF NOT EXISTS tenant_nodes_of ON tenant_nodes(node_id)"_s,
        trigger(u"tenant_nodes_follow"_s, u"AFTER INSERT ON tenant_nodes "_s + notBulk(), followingNode(false)),
        // The graph window's order: a tenant's most connected things first, as a range of this.
        u"CREATE INDEX IF NOT EXISTS tenant_nodes_degree ON tenant_nodes(tenant, degree DESC, node_id)"_s,
        trigger(u"tenant_edges_degree"_s, u"AFTER INSERT ON tenant_edges "_s + notWhole(),
                {u"UPDATE tenant_nodes SET degree = degree + 1 WHERE "_s + endsOf(u"NEW"_s)}),
        trigger(u"tenant_edges_degree_left"_s, u"AFTER DELETE ON tenant_edges "_s + notWhole(),
                {u"UPDATE tenant_nodes SET degree = degree - 1 WHERE "_s + endsOf(u"OLD"_s)}),
        // A thing that joins a tenant counts what the tenant already holds of its relations.
        trigger(u"tenant_nodes_degree"_s, u"AFTER INSERT ON tenant_nodes "_s + notWhole(),
                {u"UPDATE tenant_nodes SET degree = "_s + degreeOf(u"NEW.tenant"_s, u"NEW.node_id"_s)
                 + u" WHERE tenant = NEW.tenant AND node_id = NEW.node_id"_s}),
        // A relation's tenants are let go of while its ends are still the
        // ones they were counted for: before it is removed (a cascade from
        // graph_edges would come after, with the ends gone), moved to other
        // ends (whose tenants graph_edges_moved_tenant then places again) or
        // replaced by another of the same ends and kind (which removes it
        // without its triggers).
        trigger(u"graph_edges_degree_removed"_s, u"BEFORE DELETE ON graph_edges"_s,
                {u"DELETE FROM tenant_edges WHERE edge_id = OLD.id"_s}),
        trigger(u"graph_edges_degree_moved"_s,
                u"BEFORE UPDATE OF source, target ON graph_edges WHEN (NEW.source <> OLD.source OR NEW.target <> OLD.target) "
                "AND NOT EXISTS ("_s
                    + same + u" AND id <> OLD.id)"_s,
                {u"DELETE FROM tenant_edges WHERE edge_id = OLD.id"_s}),
        trigger(u"graph_edges_degree_replaced"_s, u"BEFORE INSERT ON graph_edges WHEN EXISTS ("_s + same + u")"_s,
                {u"DELETE FROM tenant_edges WHERE edge_id IN ("_s + same + u")"_s}),
        TenancySchema::countDegrees(),
    };
}

} // namespace

const QStringList &TenancySchema::degrees() {
    static const QStringList made = degreeStatements();
    return made;
}

QString TenancySchema::countDegrees() {
    // Each relation once for each end, a relation of a thing to itself once.
    return u"UPDATE tenant_nodes SET degree = d.n FROM (SELECT tenant, node, COUNT(*) AS n FROM ("
           "SELECT m.tenant AS tenant, e.source AS node FROM tenant_edges AS m JOIN graph_edges AS e ON e.id = m.edge_id "
           "UNION ALL SELECT m.tenant, e.target FROM tenant_edges AS m JOIN graph_edges AS e ON e.id = m.edge_id WHERE e.target <> e.source) "
           "GROUP BY tenant, node) AS d WHERE tenant_nodes.tenant = d.tenant AND tenant_nodes.node_id = d.node AND tenant_nodes.degree <> d.n"_s;
}

const QStringList &TenancySchema::step() {
    static const QStringList made = statements();
    return made;
}

QString TenancySchema::madeHere(const QString &row) {
    return u"(%1.data LIKE '%\"tenants\"%' AND json_type(CASE WHEN json_valid(%1.data) THEN %1.data ELSE '{}' END, '$.tenants') = 'array')"_s
        .arg(row);
}

const QStringList &TenancySchema::madeInTheirTenant() {
    static const QStringList made = [] {
        QStringList all;
        for (const QString &name :
             {u"graph_nodes_tenant"_s, u"graph_edges_tenant"_s, u"graph_edges_moved_tenant"_s, u"tenant_books_follow"_s})
            all << u"DROP TRIGGER IF EXISTS "_s + name;
        all << trigger(u"graph_nodes_tenant"_s, u"AFTER INSERT ON graph_nodes"_s, placingNode())
            << trigger(u"graph_nodes_made_tenant"_s, u"AFTER UPDATE OF data ON graph_nodes WHEN "_s + madeHere(u"NEW"_s), madeIn())
            << trigger(u"graph_edges_tenant"_s, u"AFTER INSERT ON graph_edges"_s, placingEdge(false))
            << trigger(u"graph_edges_moved_tenant"_s, u"AFTER UPDATE OF source, target, kind, book_id ON graph_edges"_s,
                       QStringList{u"DELETE FROM tenant_edges WHERE edge_id = NEW.id"_s} + placingEdge(false))
            << trigger(u"tenant_books_follow"_s, u"AFTER INSERT ON tenant_books "_s + notBulk(), followingBook(false));
        // Placed again, whole, by the new rules the next time Tenancy follows the books.
        all << u"UPDATE library_state SET value = -1 WHERE key = 'tenancy_one'"_s;
        return all;
    }();
    return made;
}

QString TenancySchema::notesOf(const QString &row) { return u"CASE WHEN json_valid(%1.notes) THEN %1.notes ELSE '{}' END"_s.arg(row); }

QString TenancySchema::writtenHere(const QString &row) {
    return u"(%1.notes LIKE '%\"tenants\"%' AND json_type(%2, '$.tenants') = 'object')"_s.arg(row, notesOf(row));
}

QString TenancySchema::saidIn(const QString &row, const QString &book) {
    return u"(%1.notes LIKE '%\"books\"%' AND EXISTS (SELECT 1 FROM json_each(%2, '$.books') AS j JOIN global_ids AS g "
           "ON g.entity = 'book' AND g.global_id = j.key WHERE g.local_id = %3))"_s.arg(row, notesOf(row), book);
}

QString TenancySchema::whereEndsAre(const QString &row, const QString &tenant) {
    return u"((%1.book_id = 0 AND NOT %3) OR (substr(%1.kind, 1, 8) = 'across: ' AND %2 IN (SELECT tenant FROM tenant_books WHERE "
           "book_id = %1.book_id)) OR (%1.notes LIKE '%\"books\"%' AND %2 IN (SELECT b.tenant FROM json_each(%4, '$.books') AS j "
           "JOIN global_ids AS g ON g.entity = 'book' AND g.global_id = j.key JOIN tenant_books AS b ON b.book_id = g.local_id)))"_s.arg(
               row, tenant, writtenHere(row), notesOf(row));
}

QString TenancySchema::seenTogether(const QString &one, const QString &other) {
    return u"(%2.tenant = %1.tenant OR %1.tenant = 0 OR %2.tenant = 0)"_s.arg(one, other);
}

QString TenancySchema::tenantOfBoth(const QString &one, const QString &other) {
    return u"CASE WHEN %1.tenant = 0 THEN %2.tenant ELSE %1.tenant END"_s.arg(one, other);
}

const QStringList &TenancySchema::writtenColumns() {
    static const QStringList made{
        // The tenant a fact of no book was written in by hand, by its global
        // id ('' for one no tenant wrote); and what a relation was noted in
        // each of the other books that said it, and by hand in each tenant:
        // {"books": {book's global id: note}, "tenants": {tenant's: note}}.
        u"ALTER TABLE graph_facts ADD COLUMN tenant TEXT NOT NULL DEFAULT ''"_s,
        u"ALTER TABLE graph_edges ADD COLUMN notes TEXT NOT NULL DEFAULT ''"_s,
    };
    return made;
}

const QStringList &TenancySchema::writtenInTheirTenant() {
    static const QStringList made = [] {
        QStringList all;
        for (const QString &name : {u"graph_facts_tenant"_s, u"graph_facts_moved_tenant"_s, u"graph_edges_tenant"_s,
                                    u"graph_edges_moved_tenant"_s, u"tenant_nodes_follow"_s, u"tenant_books_follow"_s})
            all << u"DROP TRIGGER IF EXISTS "_s + name;
        all << trigger(u"graph_facts_tenant"_s, u"AFTER INSERT ON graph_facts"_s, placingFact(true))
            << trigger(u"graph_facts_moved_tenant"_s, u"AFTER UPDATE OF node_id, book_id, tenant ON graph_facts"_s,
                       QStringList{u"DELETE FROM tenant_facts WHERE fact_id = NEW.id"_s} + placingFact(true))
            << trigger(u"graph_edges_tenant"_s, u"AFTER INSERT ON graph_edges"_s, placingEdge(true))
            << trigger(u"graph_edges_moved_tenant"_s, u"AFTER UPDATE OF source, target, kind, book_id, notes ON graph_edges"_s,
                       QStringList{u"DELETE FROM tenant_edges WHERE edge_id = NEW.id"_s} + placingEdge(true))
            << trigger(u"tenant_nodes_follow"_s, u"AFTER INSERT ON tenant_nodes "_s + notBulk(), followingNode(true))
            << trigger(u"tenant_books_follow"_s, u"AFTER INSERT ON tenant_books "_s + notBulk(), followingBook(true));
        // Placed again, whole, by the new rules the next time Tenancy follows the books.
        all << u"UPDATE library_state SET value = -1 WHERE key = 'tenancy_one'"_s;
        return all;
    }();
    return made;
}

// The same statements as format 30's, which can be taken again: they make
// the triggers by the rules as they are now.
const QStringList &TenancySchema::endsSeenFromEveryTenant() { return writtenInTheirTenant(); }
