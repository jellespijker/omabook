#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringView>
#include <QtTypes>
#include <expected>
#include <optional>
#include <utility>

class Library;

// A tenant of the knowledge graph (ADR 0003): a top-level collection's own
// part of it, by the collection's global id, or Unfiled, the books in no
// top-level collection; or, when the user keeps one for the whole library,
// that one. Each thing, relation and fact is stored once, with the tenants
// that see it (TenancySchema), and to whatever reads within a tenant the
// others are not there.
class Tenant {
public:
    [[nodiscard]] static Tenant ofCollection(const QString &collectionGlobalId) { return Tenant(collectionGlobalId); }
    // A tenant as globalId() says it, as the tables keep it.
    [[nodiscard]] static Tenant fromGlobalId(const QString &globalId) { return Tenant(globalId); }
    [[nodiscard]] static Tenant unfiled() { return Tenant(QStringLiteral("unfiled")); }
    // The one tenant of a library whose user chose one for all of it (Settings::graphOneTenant).
    [[nodiscard]] static Tenant wholeLibrary() { return Tenant(QStringLiteral("library")); }
    // What no book is behind (a thing put in by hand and tied to nothing):
    // not a tenant one reads, but seen from every one.
    [[nodiscard]] static Tenant everyone() { return Tenant(QStringLiteral("any")); }

    [[nodiscard]] const QString &globalId() const noexcept { return m_globalId; }
    bool operator==(const Tenant &) const = default;

private:
    explicit Tenant(QString globalId) : m_globalId(std::move(globalId)) {}
    QString m_globalId;
};

[[nodiscard]] inline size_t qHash(const Tenant &tenant, size_t seed = 0) noexcept { return qHash(tenant.globalId(), seed); }

// What keeps a query of the graph to one tenant: a condition on a column that
// holds a thing's, a relation's, a fact's or a book's id. Each reads the
// tenant's range of its membership table, which the tenant leads, so its cost
// follows the tenant, not the library:
//
//   "SELECT … FROM graph_nodes AS n WHERE n.scope = '' AND " + within.nodes(u"n.id")
//   "SELECT … FROM graph_seen AS s WHERE " + within.books(u"s.book_id")
//
// Those read the tenant's whole range once per statement, which is what a
// scan wants. A query of a few rows (one thing, the relations of one) asks
// instead whether each row it reads is the tenant's, through the index of
// the row: the has… conditions, whose cost follows the rows read.
class TenantFilter {
public:
    explicit TenantFilter(qint64 id) : m_id(id) {}
    [[nodiscard]] QString nodes(QStringView column) const;
    [[nodiscard]] QString edges(QStringView column) const;
    [[nodiscard]] QString facts(QStringView column) const;
    // A book in the tenant: what a sighting (graph_seen) is in.
    [[nodiscard]] QString books(QStringView column) const;
    [[nodiscard]] QString hasNode(QStringView column) const;
    [[nodiscard]] QString hasEdge(QStringView column) const;
    [[nodiscard]] QString hasFact(QStringView column) const;
    [[nodiscard]] QString hasBook(QStringView column) const;
    // The tenant's number in this library's tables; -1 for one with no books here yet.
    [[nodiscard]] qint64 id() const noexcept { return m_id; }

private:
    qint64 m_id;
};

// Keeps the tenants of a library's books as its collections say, and with
// them which tenants see each thing, relation and fact. The rows themselves
// keep that by triggers as they are written; what changes here is a book's
// tenants, which follow from the collections (a book is in the tenants of
// the top-level collections it is in, through any inside them, Unfiled when
// none) or, by the user's choice, are the one tenant of the whole library.
//
// Nothing of this is in the change log: every computer derives it from what
// the log already carries (the books, the collections and their rules, and
// each fact, sighting and relation with its book), so it travels without a
// byte more.
namespace Tenancy {

// Whether the user keeps one tenant for the whole library (Settings::graphOneTenant),
// read again only when config.json changes.
[[nodiscard]] bool oneTenant();

// The library's tenants as it is now: Unfiled and each top-level collection,
// or the whole library alone.
[[nodiscard]] QList<Tenant> tenants(const Library &library);

// Brings every book's tenants up to date, and with them the graph's: at
// once when the choice of one tenant changed (the first time too, which is
// how the format step that adds tenants fills them) or a book was removed,
// otherwise only for what changed: a book that joined a tenant adds what it
// taught there, one that left takes back what it alone gave (TenancyLeave).
// Nothing is done when nothing did.
void follow(const Library &library);
// The same after a write, from the rows it touched (ChangeLog::afterWrite):
// for the books it touched, or every book when it touched a collection.
void followWrite(const Library &library, const QHash<QString, QList<qint64>> &touched);
// Computes which tenants see each thing, relation and fact again, from the
// books' tenants, as the triggers and the leaves would have from the start.
void rebuild(const Library &library);

// Which tenants see a book, a thing, a relation or a fact, by global id.
[[nodiscard]] QList<Tenant> ofBook(const Library &library, qint64 bookId);
[[nodiscard]] QList<Tenant> ofNode(const Library &library, qint64 nodeId);
[[nodiscard]] QList<Tenant> ofEdge(const Library &library, qint64 edgeId);
[[nodiscard]] QList<Tenant> ofFact(const Library &library, qint64 factId);

// How many books each tenant holds, by its global id; one with none is not there.
[[nodiscard]] QHash<QString, qint64> bookCounts(const Library &library);

// The condition that keeps a query to `tenant` (and to what no book is behind).
[[nodiscard]] TenantFilter filter(const Library &library, const Tenant &tenant);

// The books a tenant has, by their numbers here.
[[nodiscard]] QSet<qint64> booksOf(const Library &library, const Tenant &tenant);
// What a tenant is called: its top-level collection's name, "Unfiled", or
// "The whole library" (in English; the window says it in its language).
[[nodiscard]] QString nameOf(const Library &library, const Tenant &tenant);
// The tenant a collection is part of: that of the top-level collection it
// is in, or the whole library's when the user keeps one; none for a
// collection the library doesn't have.
[[nodiscard]] std::optional<Tenant> ofCollection(const Library &library, qint64 collectionId);

// The tenant a scope of its own (a world) is in: that of the books its
// things were met in; none for one of no book, or the general graph.
[[nodiscard]] std::optional<Tenant> ofScope(const Library &library, const QString &scope);
// The tenant read when nothing says which: the library's only one, or else
// the one with the most books.
[[nodiscard]] Tenant preferred(const Library &library);
// The tenant a reader of a book reads in: the book's, the first of them by
// global id for a book in two (the window lets the reader choose, #96), and
// preferred() for a book in none.
[[nodiscard]] Tenant ofReader(const Library &library, qint64 bookId);
// The tenant the command line and the MCP server read in: the one named (a
// collection's name or path, which stands for its top-level collection's,
// "unfiled", or a tenant's global id), or else the tenant of the book given
// when it has one, or else the library's only one; otherwise why not,
// naming the tenants there are, so that the caller asks rather than mixes.
[[nodiscard]] std::expected<Tenant, QString> chosen(const Library &library, const QString &named, qint64 bookId = 0);
// The membership that keeps a thing made by hand (Graph::upsert, of no
// book) in the tenant it was made in, as the thing's data says it
// ({"tenants": [global id, ...]}); none for the whole library's, whose
// things are seen from every tenant.
[[nodiscard]] inline QString madeInKey() { return QStringLiteral("tenants"); }

} // namespace Tenancy
