#pragma once

#include <QStringList>

// The format step that gives the knowledge graph its tenants (ADR 0003): which
// tenants see each thing, relation and fact, kept beside the rows and never
// in the change log, and the triggers that keep it as the rows are written,
// by whoever writes them (the graph, a sync replaying the log, a merge).
//
// What each row is in follows from the books it came from (tenant_books,
// which Tenancy keeps from the collections):
//
// - a thing: the tenants of its own book (a book's node), of the books it was
//   met in, of its facts' books and of the books its relations were learned
//   in (not those across books), and, tied to a book's node by a relation of
//   no book (its authors, series, tags, a question), of that book;
// - a fact: its book's tenants; of no book, its thing's;
// - a relation: its book's tenants; across books, or of no book, the tenants
//   both its ends are in (and its book is in, if it has one), an end in
//   tenant 0 counting as in each;
// - a thing made by hand in a tenant (its data's "tenants", Tenancy::madeInKey)
//   is in that tenant, and in the whole library's; a relation of no book to a
//   book's node gives it nothing more, so that a question asked of the
//   library stays with the tenant it was asked in, whatever books it names;
// - a fact or a relation written by hand in a tenant (a fact's `tenant`, the
//   "tenants" of a relation's notes) is in that tenant and in the whole
//   library's, and gives its thing or its ends nothing (#109);
// - a relation said in another book than its own (the "books" of its notes)
//   is in that book's tenants too, where both its ends are (#109);
// - a thing no book is behind and no tenant made (made while the library
//   was one tenant, or before tenants) is in tenant 0, which every tenant sees.
//
// The triggers only ever add: a row removed doesn't take back what it gave,
// until Tenancy rebuilds (when a book is removed, or the choice of one tenant
// changes), which recomputes it all by the same rules. A book that leaves a
// tenant takes back what it gave there by the same rules too (TenancyLeave).
namespace TenancySchema {

// The step's statements: the tables, their indexes, led by the tenant, and the triggers.
[[nodiscard]] const QStringList &step();
// The step after it (#95): the triggers again, with the things made by hand
// kept in the tenant they were made in, and the whole membership placed
// again by those rules the next time Tenancy follows the books.
[[nodiscard]] const QStringList &madeInTheirTenant();

// The step after that (#101): each thing's degree in each tenant it is in
// (tenant_nodes.degree), the relations of it that tenant holds, kept by
// triggers as the membership and the relations change, with an index led by
// the tenant that gives the graph window its most connected things first.
[[nodiscard]] const QStringList &degrees();
// Counts every degree whole, as Tenancy does after filling the tables whole
// (with tenancy_bulk 2, while the triggers that count one row at a time stand aside).
[[nodiscard]] QString countDegrees();
// The steps after that (#109): the columns in which a fact of no book keeps
// the tenant it was written in by hand, and a relation what it was noted in
// each book that said it and in each tenant that wrote it by hand (format
// 29); and the triggers again by those rules, with the membership placed
// again whole (format 30, which can be taken again).
[[nodiscard]] const QStringList &writtenColumns();
[[nodiscard]] const QStringList &writtenInTheirTenant();
// The step after those (#112, format 32): the triggers again, with an end in
// tenant 0 counted as in each tenant, and the membership placed again whole.
[[nodiscard]] const QStringList &endsSeenFromEveryTenant();

// Whether the thing of `row` (a table's alias, or NEW) was made by hand in a tenant.
[[nodiscard]] QString madeHere(const QString &row);
// The notes of relation `row` (graph_edges.notes), as JSON functions take them.
[[nodiscard]] QString notesOf(const QString &row);
// Whether relation `row` was written by hand in a tenant.
[[nodiscard]] QString writtenHere(const QString &row);
// Whether relation `row` was said in book `book` (a column or NEW's) other than its own.
[[nodiscard]] QString saidIn(const QString &row, const QString &book);
// Whether relation `row` is in `tenant` (a column) where both its ends are
// in it: across books and in a tenant of its book, of no book and written by
// no tenant, or said in another book of the tenant.
[[nodiscard]] QString whereEndsAre(const QString &row, const QString &tenant);
// Whether two ends' memberships (`one` and `other`, rows of tenant_nodes) are
// seen together in a tenant: the same one, or one of them tenant 0, which
// every tenant sees; and which tenant that is (the other's, for an end in 0).
[[nodiscard]] QString seenTogether(const QString &one, const QString &other);
[[nodiscard]] QString tenantOfBoth(const QString &one, const QString &other);

} // namespace TenancySchema
