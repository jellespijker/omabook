#include "tenancyleave.h"

#include <QSqlQuery>
#include <QString>
#include <QStringList>

#include "changelog.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

// Whether relation `row` is learned in its book (not a connection across books, GraphEdge::across).
[[nodiscard]] QString inBook(const QString &row) { return u"substr(%1.kind, 1, 8) <> 'across: '"_s.arg(row); }

// A thing that stands for a book (Graph::bookNode).
[[nodiscard]] QString bookNode(const QString &row) { return u"(%1.kind = 'book' AND %1.book_id IS NOT NULL)"_s.arg(row); }

// The things book `B` is behind, by the rules of TenancySchema: its own node,
// those met in it, its facts' and the ends of the relations learned in it,
// and those tied by hand to its node.
[[nodiscard]] QString taught() {
    return u"SELECT id FROM graph_nodes WHERE book_id = :B "
           "UNION SELECT node_id FROM graph_seen WHERE book_id = :B "
           "UNION SELECT node_id FROM graph_facts WHERE book_id = :B "
           "UNION SELECT e.source FROM graph_edges AS e WHERE e.book_id = :B AND "_s
           + inBook(u"e"_s) + u" UNION SELECT e.target FROM graph_edges AS e WHERE e.book_id = :B AND "_s + inBook(u"e"_s)
           + u" UNION SELECT e.target FROM graph_nodes AS b JOIN graph_edges AS e ON e.source = b.id WHERE b.book_id = :B AND "_s
           + bookNode(u"b"_s) + u" AND e.book_id = 0 AND "_s + inBook(u"e"_s)
           + u" UNION SELECT e.source FROM graph_nodes AS b JOIN graph_edges AS e ON e.target = b.id WHERE b.book_id = :B AND "_s
           + bookNode(u"b"_s) + u" AND e.book_id = 0 AND "_s + inBook(u"e"_s);
}

// Whether another of tenant `T`'s books is still behind thing `c`, by the same rules.
[[nodiscard]] QString stillBehind(const QString &c) {
    const QString ofTenant = u"JOIN tenant_books AS t ON t.tenant = :T AND t.book_id = "_s;
    const QString tied = u"SELECT 1 FROM graph_edges AS e JOIN graph_nodes AS b ON b.id = e.%1 "_s + ofTenant
                         + u"b.book_id JOIN graph_nodes AS o ON o.id = %3 WHERE e.%2 = %3 AND e.book_id = 0 AND "_s + inBook(u"e"_s)
                         + u" AND "_s + bookNode(u"b"_s) + u" AND NOT "_s + bookNode(u"o"_s);
    const QStringList ways{
        u"SELECT 1 FROM graph_nodes AS n "_s + ofTenant + u"n.book_id WHERE n.id = %1"_s.arg(c),
        u"SELECT 1 FROM graph_seen AS s "_s + ofTenant + u"s.book_id WHERE s.node_id = %1"_s.arg(c),
        u"SELECT 1 FROM graph_facts AS f "_s + ofTenant + u"f.book_id WHERE f.node_id = %1"_s.arg(c),
        u"SELECT 1 FROM graph_edges AS e "_s + ofTenant + u"e.book_id WHERE e.source = %1 AND "_s.arg(c) + inBook(u"e"_s),
        u"SELECT 1 FROM graph_edges AS e "_s + ofTenant + u"e.book_id WHERE e.target = %1 AND "_s.arg(c) + inBook(u"e"_s),
        tied.arg(u"source"_s, u"target"_s, c),
        tied.arg(u"target"_s, u"source"_s, c),
    };
    QStringList any;
    for (const QString &way : ways)
        any << u"EXISTS ("_s + way + u")"_s;
    return any.join(u" OR "_s);
}

// The statements of a leave, with `:T` for the tenant and `:B` for the book.
[[nodiscard]] QStringList statements() {
    const QString shared = u"(e.book_id = 0 OR substr(e.kind, 1, 8) = 'across: ')"_s;
    return {
        u"CREATE TEMP TABLE IF NOT EXISTS tenancy_leaving (id INTEGER PRIMARY KEY)"_s,
        u"CREATE TEMP TABLE IF NOT EXISTS tenancy_leaving_edges (id INTEGER PRIMARY KEY)"_s,
        u"DELETE FROM tenancy_leaving"_s,
        u"DELETE FROM tenancy_leaving_edges"_s,
        u"DELETE FROM tenant_books WHERE tenant = :T AND book_id = :B"_s,
        // The tenant's things the book was behind, and of those the ones no other book of it is.
        u"INSERT OR IGNORE INTO tenancy_leaving (id) SELECT node_id FROM tenant_nodes WHERE tenant = :T AND node_id IN ("_s + taught()
            + u")"_s,
        u"DELETE FROM tenancy_leaving WHERE "_s + stillBehind(u"tenancy_leaving.id"_s),
        u"DELETE FROM tenant_nodes WHERE tenant = :T AND node_id IN (SELECT id FROM tenancy_leaving)"_s,
        // One no book is behind any more is seen from every tenant.
        u"INSERT INTO tenant_nodes (tenant, node_id) SELECT 0, id FROM tenancy_leaving WHERE id NOT IN (SELECT node_id FROM tenant_nodes)"_s,
        // The book's facts, and those of no book of the things that left, which are where their things are.
        u"DELETE FROM tenant_facts WHERE tenant = :T AND fact_id IN (SELECT id FROM graph_facts WHERE book_id = :B)"_s,
        u"DELETE FROM tenant_facts WHERE tenant IN (:T, 0) AND fact_id IN (SELECT f.id FROM tenancy_leaving AS c "
        "JOIN graph_facts AS f ON f.node_id = c.id WHERE COALESCE(f.book_id, 0) = 0)"_s,
        u"INSERT OR IGNORE INTO tenant_facts (tenant, fact_id) SELECT m.tenant, f.id FROM tenancy_leaving AS c "
        "JOIN graph_facts AS f ON f.node_id = c.id JOIN tenant_nodes AS m ON m.node_id = c.id AND m.tenant IN (:T, 0) "
        "WHERE COALESCE(f.book_id, 0) = 0"_s,
        // The book's relations, and those of no book or across books at the things
        // that left, which are only where both their ends are.
        u"DELETE FROM tenant_edges WHERE tenant = :T AND edge_id IN (SELECT id FROM graph_edges WHERE book_id = :B)"_s,
        u"INSERT OR IGNORE INTO tenancy_leaving_edges (id) SELECT e.id FROM tenancy_leaving AS c JOIN graph_edges AS e ON e.source = c.id "
        "WHERE "_s
            + shared + u" UNION SELECT e.id FROM tenancy_leaving AS c JOIN graph_edges AS e ON e.target = c.id WHERE "_s + shared,
        u"DELETE FROM tenant_edges WHERE tenant IN (:T, 0) AND edge_id IN (SELECT id FROM tenancy_leaving_edges)"_s,
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT s.tenant, e.id FROM tenancy_leaving_edges AS x "
        "JOIN graph_edges AS e ON e.id = x.id JOIN tenant_nodes AS s ON s.node_id = e.source AND s.tenant IN (:T, 0) "
        "JOIN tenant_nodes AS t ON t.tenant = s.tenant AND t.node_id = e.target WHERE e.book_id = 0 "
        "OR (substr(e.kind, 1, 8) = 'across: ' AND s.tenant IN (SELECT tenant FROM tenant_books WHERE book_id = e.book_id))"_s,
    };
}

[[nodiscard]] qint64 shrunk(const Library &library) {
    QSqlQuery found = ChangeLog::query(library, u"SELECT value FROM library_state WHERE key = 'tenancy_shrunk'"_s);
    return found.next() ? found.value(0).toLongLong() : 0;
}

void setState(const Library &library, const QString &key, qint64 value) {
    ChangeLog::exec(library, u"UPDATE library_state SET value = ? WHERE key = ?"_s, {value, key});
}

} // namespace

void TenancyLeave::take(const Library &library, qint64 tenant, qint64 book) {
    static const QStringList made = statements();
    // The triggers that follow one row at a time stand aside, and the mark
    // that a book left a tenant (for a rebuild) is not for a leave taken
    // back here: only for one that wasn't, as a book's removal.
    const qint64 marked = shrunk(library);
    setState(library, u"tenancy_bulk"_s, 1);
    const QString t = QString::number(tenant);
    const QString b = QString::number(book);
    for (const QString &statement : made)
        ChangeLog::exec(library, QString(statement).replace(u":T"_s, t).replace(u":B"_s, b));
    setState(library, u"tenancy_bulk"_s, 0);
    setState(library, u"tenancy_shrunk"_s, marked);
}
