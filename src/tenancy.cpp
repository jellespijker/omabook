#include "tenancy.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QSqlQuery>
#include <QStringList>

#include "booksides.h"
#include "changelog.h"
#include "library.h"
#include "settings.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// The tenants of each book, by their numbers here.
using Placed = QHash<qint64, QSet<qint64>>;

// More books than this touched at once (an import) are placed with all the others.
constexpr qsizetype fewBooks = 64;

[[nodiscard]] qint64 stateOf(const Library &library, const QString &key) {
    QSqlQuery found = ChangeLog::query(library, u"SELECT value FROM library_state WHERE key = ?"_s, {key});
    return found.next() ? found.value(0).toLongLong() : -1;
}

void setState(const Library &library, const QString &key, qint64 value) {
    ChangeLog::exec(library, u"UPDATE library_state SET value = ? WHERE key = ?"_s, {value, key});
}

// A tenant's number here, given the first time it is asked for.
[[nodiscard]] qint64 numberOf(const Library &library, const Tenant &tenant) {
    QSqlQuery found = ChangeLog::query(library, u"SELECT id FROM tenants WHERE gid = ?"_s, {tenant.globalId()});
    if (found.next())
        return found.value(0).toLongLong();
    found.finish();
    return ChangeLog::insert(library, u"INSERT INTO tenants (gid) VALUES (?)"_s, {tenant.globalId()}).value_or(-1);
}

// Where these books belong: the top-level collections each is in (BookSides),
// Unfiled for none, or all in the one tenant of the whole library.
[[nodiscard]] Placed placed(const Library &library, const QList<Book> &books, bool one) {
    Placed placed;
    if (one) {
        const qint64 whole = numberOf(library, Tenant::wholeLibrary());
        for (const Book &book : books)
            placed.insert(book.id, {whole});
        return placed;
    }
    const BookSides sides(library.collections(), books);
    const qint64 unfiled = numberOf(library, Tenant::unfiled());
    QHash<qint64, qint64> ofTop;
    for (const Book &book : books) {
        QSet<qint64> &in = placed[book.id];
        for (const qint64 top : sides.side(book.id)) {
            if (top == BookSides::none)
                continue;
            if (!ofTop.contains(top)) {
                const QString collection = Spaces::collectionId(library, top);
                ofTop.insert(top, collection.isEmpty() ? unfiled : numberOf(library, Tenant::ofCollection(collection)));
            }
            in.insert(ofTop.value(top));
        }
        if (in.isEmpty())
            in.insert(unfiled);
    }
    return placed;
}

// Where the tables have them now.
[[nodiscard]] QSet<qint64> keptOf(const Library &library, qint64 bookId) {
    QSet<qint64> kept;
    for (QSqlQuery found = ChangeLog::query(library, u"SELECT tenant FROM tenant_books WHERE book_id = ?"_s, {bookId}); found.next();)
        kept.insert(found.value(0).toLongLong());
    return kept;
}

// Each book where it belongs: a tenant it left is taken back (which marks
// the graph for a rebuild), one it joined adds what it taught there (the
// triggers of tenant_books). Says whether any was somewhere else.
bool settle(const Library &library, const Placed &wanted) {
    bool moved = false;
    for (auto book = wanted.cbegin(); book != wanted.cend(); ++book) {
        const QSet<qint64> kept = keptOf(library, book.key());
        for (const qint64 left : kept - book.value())
            ChangeLog::exec(library, u"DELETE FROM tenant_books WHERE tenant = ? AND book_id = ?"_s, {left, book.key()});
        for (const qint64 joined : book.value() - kept)
            ChangeLog::exec(library, u"INSERT OR IGNORE INTO tenant_books (tenant, book_id) VALUES (?, ?)"_s, {joined, book.key()});
        moved = moved || kept != book.value();
    }
    return moved;
}

// The rules of TenancySchema, for the whole graph at once.
[[nodiscard]] QStringList derived() {
    const QString inBook = u"substr(e.kind, 1, 8) <> 'across: '"_s;
    const QString bookNode = u"(%1.kind = 'book' AND %1.book_id IS NOT NULL)"_s;
    return {
        u"DELETE FROM tenant_nodes"_s,
        u"DELETE FROM tenant_edges"_s,
        u"DELETE FROM tenant_facts"_s,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT t.tenant, n.id FROM graph_nodes AS n "
        "JOIN tenant_books AS t ON t.book_id = n.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT t.tenant, s.node_id FROM graph_seen AS s "
        "JOIN tenant_books AS t ON t.book_id = s.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT t.tenant, f.node_id FROM graph_facts AS f "
        "JOIN tenant_books AS t ON t.book_id = f.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT t.tenant, e.source FROM graph_edges AS e "
        "JOIN tenant_books AS t ON t.book_id = e.book_id WHERE "_s
            + inBook + u" UNION SELECT t.tenant, e.target FROM graph_edges AS e JOIN tenant_books AS t ON t.book_id = e.book_id WHERE "_s
            + inBook,
        u"INSERT OR IGNORE INTO tenant_nodes (tenant, node_id) SELECT t.tenant, e.target FROM graph_edges AS e "
        "JOIN graph_nodes AS b ON b.id = e.source JOIN graph_nodes AS o ON o.id = e.target JOIN tenant_books AS t ON t.book_id = b.book_id "
        "WHERE e.book_id = 0 AND "_s
            + inBook + u" AND "_s + bookNode.arg(u"b"_s) + u" AND NOT "_s + bookNode.arg(u"o"_s)
            + u" UNION SELECT t.tenant, e.source FROM graph_edges AS e JOIN graph_nodes AS b ON b.id = e.target "
              "JOIN graph_nodes AS o ON o.id = e.source JOIN tenant_books AS t ON t.book_id = b.book_id WHERE e.book_id = 0 AND "_s
            + inBook + u" AND "_s + bookNode.arg(u"b"_s) + u" AND NOT "_s + bookNode.arg(u"o"_s),
        u"INSERT INTO tenant_nodes (tenant, node_id) SELECT 0, id FROM graph_nodes WHERE id NOT IN (SELECT node_id FROM tenant_nodes)"_s,
        u"INSERT OR IGNORE INTO tenant_facts (tenant, fact_id) SELECT t.tenant, f.id FROM graph_facts AS f "
        "JOIN tenant_books AS t ON t.book_id = f.book_id"_s,
        u"INSERT OR IGNORE INTO tenant_facts (tenant, fact_id) SELECT m.tenant, f.id FROM graph_facts AS f "
        "JOIN tenant_nodes AS m ON m.node_id = f.node_id WHERE COALESCE(f.book_id, 0) = 0"_s,
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT t.tenant, e.id FROM graph_edges AS e "
        "JOIN tenant_books AS t ON t.book_id = e.book_id WHERE "_s
            + inBook,
        u"INSERT OR IGNORE INTO tenant_edges (tenant, edge_id) SELECT s.tenant, e.id FROM graph_edges AS e "
        "JOIN tenant_nodes AS s ON s.node_id = e.source JOIN tenant_nodes AS t ON t.node_id = e.target AND t.tenant = s.tenant "
        "WHERE e.book_id = 0 OR (substr(e.kind, 1, 8) = 'across: ' "
        "AND s.tenant IN (SELECT tenant FROM tenant_books AS b WHERE b.book_id = e.book_id))"_s,
    };
}

// Fills the tables whole, statement by statement, with the triggers that
// follow one row at a time standing aside.
void inBulk(const Library &library, const auto &fill) {
    setState(library, u"tenancy_bulk"_s, 1);
    fill();
    for (const QString &statement : derived())
        ChangeLog::exec(library, statement);
    setState(library, u"tenancy_bulk"_s, 0);
    setState(library, u"tenancy_shrunk"_s, 0);
}

[[nodiscard]] QList<Tenant> members(const Library &library, const QString &table, const QString &column, qint64 id) {
    QList<Tenant> found;
    for (QSqlQuery rows = ChangeLog::query(
             library, u"SELECT t.gid FROM %1 AS m JOIN tenants AS t ON t.id = m.tenant WHERE m.%2 = ? ORDER BY t.gid"_s.arg(table, column),
             {id});
         rows.next();)
        found << Tenant::fromGlobalId(rows.value(0).toString());
    return found;
}

} // namespace

QString TenantFilter::nodes(QStringView column) const {
    return u"%1 IN (SELECT node_id FROM tenant_nodes WHERE tenant IN (%2, 0))"_s.arg(column.toString(), QString::number(m_id));
}

QString TenantFilter::edges(QStringView column) const {
    return u"%1 IN (SELECT edge_id FROM tenant_edges WHERE tenant IN (%2, 0))"_s.arg(column.toString(), QString::number(m_id));
}

QString TenantFilter::facts(QStringView column) const {
    return u"%1 IN (SELECT fact_id FROM tenant_facts WHERE tenant IN (%2, 0))"_s.arg(column.toString(), QString::number(m_id));
}

QString TenantFilter::books(QStringView column) const {
    return u"%1 IN (SELECT book_id FROM tenant_books WHERE tenant = %2)"_s.arg(column.toString(), QString::number(m_id));
}

bool Tenancy::oneTenant() {
    struct Choice {
        QString file;
        QDateTime modified;
        qint64 size = -1;
        bool one = false;
    };
    thread_local Choice choice;
    const QFileInfo config(Settings::defaultFile());
    if (config.absoluteFilePath() != choice.file || config.lastModified() != choice.modified || config.size() != choice.size)
        choice = {config.absoluteFilePath(), config.lastModified(), config.size(), Settings().graphOneTenant()};
    return choice.one;
}

QList<Tenant> Tenancy::tenants(const Library &library) {
    if (oneTenant())
        return {Tenant::wholeLibrary()};
    QList<Tenant> all{Tenant::unfiled()};
    for (const Collection &collection : library.collections())
        if (collection.depth == 0)
            all << Tenant::ofCollection(Spaces::collectionId(library, collection.id));
    return all;
}

void Tenancy::follow(const Library &library) {
    const Library::Batch batch(library);
    const bool one = oneTenant();
    const qint64 books = library.changes().books;
    const bool chosenAsBefore = stateOf(library, u"tenancy_one"_s) == static_cast<qint64>(one);
    if (chosenAsBefore && stateOf(library, u"tenancy_books"_s) == books && stateOf(library, u"tenancy_shrunk"_s) == 0)
        return;
    const Placed wanted = placed(library, library.books(), one);
    if (!chosenAsBefore) {
        inBulk(library, [&] {
            ChangeLog::exec(library, u"DELETE FROM tenant_books"_s);
            for (auto book = wanted.cbegin(); book != wanted.cend(); ++book)
                for (const qint64 tenant : book.value())
                    ChangeLog::exec(library, u"INSERT INTO tenant_books (tenant, book_id) VALUES (?, ?)"_s, {tenant, book.key()});
        });
    } else {
        const bool moved = settle(library, wanted);
        if (stateOf(library, u"tenancy_shrunk"_s) != 0)
            rebuild(library);
        // Every book where it was: nothing is written, so that only looking
        // at a library (a dry run of a sync) leaves it as it was.
        else if (!moved)
            return;
    }
    setState(library, u"tenancy_one"_s, one ? 1 : 0);
    setState(library, u"tenancy_books"_s, books);
}

void Tenancy::followWrite(const Library &library, const QHash<QString, QList<qint64>> &touched) {
    const QList<qint64> books = touched.value(u"book"_s);
    if (touched.contains(u"collection"_s) || books.size() > fewBooks || stateOf(library, u"tenancy_one"_s) != (oneTenant() ? 1 : 0)) {
        follow(library);
        return;
    }
    if (books.isEmpty())
        return;
    const Library::Batch batch(library);
    QList<Book> those;
    for (const qint64 id : books)
        if (const auto book = library.book(id))
            those << *book;
    settle(library, placed(library, those, oneTenant()));
    if (stateOf(library, u"tenancy_shrunk"_s) != 0)
        rebuild(library);
}

void Tenancy::rebuild(const Library &library) {
    const Library::Batch batch(library);
    inBulk(library, [] {});
}

QList<Tenant> Tenancy::ofBook(const Library &library, qint64 bookId) { return members(library, u"tenant_books"_s, u"book_id"_s, bookId); }
QList<Tenant> Tenancy::ofNode(const Library &library, qint64 nodeId) { return members(library, u"tenant_nodes"_s, u"node_id"_s, nodeId); }
QList<Tenant> Tenancy::ofEdge(const Library &library, qint64 edgeId) { return members(library, u"tenant_edges"_s, u"edge_id"_s, edgeId); }
QList<Tenant> Tenancy::ofFact(const Library &library, qint64 factId) { return members(library, u"tenant_facts"_s, u"fact_id"_s, factId); }

TenantFilter Tenancy::filter(const Library &library, const Tenant &tenant) {
    QSqlQuery found = ChangeLog::query(library, u"SELECT id FROM tenants WHERE gid = ?"_s, {tenant.globalId()});
    return TenantFilter(found.next() ? found.value(0).toLongLong() : -1);
}
