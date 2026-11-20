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
//   both its ends are in (and its book is in, if it has one);
// - a thing made by hand in a tenant (its data's "tenants", Tenancy::madeInKey)
//   is in that tenant, and in the whole library's; a relation of no book to a
//   book's node gives it nothing more, so that a question asked of the
//   library stays with the tenant it was asked in, whatever books it names;
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

// Whether the thing of `row` (a table's alias, or NEW) was made by hand in a tenant.
[[nodiscard]] QString madeHere(const QString &row);

} // namespace TenancySchema
