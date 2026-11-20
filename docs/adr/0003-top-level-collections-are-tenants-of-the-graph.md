# 3. Top-level collections are tenants of the knowledge graph

- Status: accepted, to be built in parts (#92); amended 2026-11-20, after the performance analysis (#94),
  and for what is made by hand (#95)
- Date: 2026-11-20

## Context

A library can be split into top-level collections, such as fiction and the books one studies. Until now
they were kept apart in the knowledge graph only by filtering at read time: every book wrote into one
general graph, and readers dropped the relations that cross from one collection's books to another's.
Everything not filtered was shared: a theme, a topic, an author, a series or a tag became one node for
the whole library, and the readers that forgot to filter (the whole view, the walk around a thing,
search, `knows:`, the library-wide picture the AI is given, the command line and MCP tools, export,
shared layers, sync) showed one collection's knowledge in another's.

The owner wants each top-level collection to be a world of its own.

## Decision

Each top-level collection is a **tenant**: its own part of the graph, with nothing of another tenant
read, merged or shown within it. Books in no top-level collection are the **Unfiled** tenant.

- **Stored once, with a membership.** Each thing, fact and relation is stored once, with the tenants
  that see it. A book in two top-level collections is in both tenants; what is learned from it is
  extracted and stored once, and seen from both.
- **Membership follows the books.** A thing learned only from tenant A's books is only A's. A fact is
  its book's tenants'; a relation its book's, and one across books or of no book only where both its
  ends are. A thing is in the tenants of its own book (a book's node), of the books it was met in, of
  its facts' and its relations' books, and, tied by hand to a book's node (its authors, series, tags,
  a question), of that book. So an author who wrote in two tenants, or a theme two tenants' books
  share, is one node seen from both, and each of its facts only from the tenant whose book said it.
  A thing put in by hand belongs to the tenant it was made in, and a relation of no book to a book's
  node gives it no other (so a question asked of the library stays with the tenant it was asked in,
  whatever books it names); only what no book is behind and no tenant made (made while the library
  was one tenant, or before tenants) is seen from every tenant (decided 2026-11-20, #95).
- **Every reader takes a tenant**, and filters by it; there is no merged view. A harness check flags
  graph queries without one (#95). A setting, "one tenant for the whole library", keeps the old
  behaviour for those who don't want tenants.
- **Moving a book changes membership, not data**: its things, facts and relations stay where they are
  stored, and the tenants that see them change (#97).
- **The tenant leads every new index** of the graph (#99, #101, #102).

### As built (#94)

- **Tables beside the rows, never in the log.** `tenants` gives each tenant a number here by its global
  id: a top-level collection's (`Spaces::collectionId`, which a collection keeps when renamed),
  `unfiled`, `library` (the one tenant of the whole library) and `any` (0, what no book is behind).
  `tenant_books`, `tenant_nodes`, `tenant_edges` and `tenant_facts` hold (tenant, row), keyed by the
  tenant first, so a reader of one tenant reads one range, and indexed by the row for what follows a
  row and for its removal (a cascade). `graph_seen`, `graph_edges` and `graph_facts` have no scope column:
  a sighting is in its book's tenants (`tenant_books(tenant, book_id)`, then the book's sightings by
  #99's index), and a relation and a fact have rows of their own.
- **Kept by triggers, by whoever writes.** The graph, a sync replaying the log and a merge of books all
  write the same rows, so the rules above are triggers on them (`TenancySchema`). They only add; a row
  removed doesn't take back what it gave until the membership is rebuilt (`Tenancy::rebuild`, the same
  rules over the whole graph at once), which is done when a book is removed or the choice of one
  tenant changes. A test holds the rebuild equal to what the writes kept.
- **A book's tenants follow its collections** (`Tenancy::follow`): the top-level collections it is in,
  through any inside them (by the same reading of rules as `BookSides`), Unfiled for none; all in
  `library` with the setting `graph_one_tenant` (which replaces `graph_across_collections`, still read).
  It runs as a library is set up, after each write that touched a book or a collection, and after a
  sync replays the log.
- **It travels by being derived, not logged.** Everything membership is made of already travels: the
  books, the collections and their rules, and each fact, sighting and relation with its book. So each
  computer derives the same membership from the same rows, and the change log carries not a byte more
  for it. (A computer whose own writes removed rows may keep a thing in a tenant a little longer than
  one that took the same rows from the log, until its next rebuild; never in a tenant none of its
  books are in.)
- **A move takes back only what the book gave** (#97, `TenancyLeave`). A book that leaves a tenant
  (taken out of a collection, moved to another, its top-level collection removed) takes the same rules
  back for the rows it was behind and that one tenant: its facts and the relations learned in it leave;
  a thing it taught leaves unless another of the tenant's books is still behind it, and with it its
  facts of no book and the relations that were there because both ends were. What it costs follows the
  book, not the library, and tests hold it equal to a rebuild, here and on a computer the move reached
  through the log.
- **The split of the existing graph** is the format step that adds the tables and triggers (format 25),
  after the copy every step keeps, and the first `Tenancy::follow` as the library is then set up, which
  fills the membership of the whole graph at once.

### As built (#96)

- **The window shows one tenant at a time.** `GraphTenants` (one for the window) lists the tenants to switch
  between, the top-level collections and then Unfiled while a book is in it, keeps the one shown, and
  remembers it per library (`graph_tenants` in `config.json`). The graph and the chronology take it as
  their `tenant` property, which their readers keep to (#95); a book's graph or chronology opens in a
  tenant of the book's own. With one tenant for the whole library there is that one and no switch.

### As built (#95)

- **Every reader reads through a tenant's graph.** `Graph::forTenant` (and `ofReader` for a book's
  reader, `ofCollection` for a collection's view, `ofLibrary` for what names none) keeps every read of
  `Graph` to its tenant: things, relations, facts, sightings, search, the views and the scopes. A thing
  both tenants see is told as the tenant knows it, from its facts there, so the summary another
  tenant's book gave it never shows. A `Graph` of no tenant reads the whole graph: what writes it, keeps
  it and carries it to other computers. The read-time filtering by book sides is gone from the readers;
  what builds a book's graph still draws only on the books it shares a tenant with (`Graph::together`).
- **Where a reader doesn't say its tenant** it is that of the book, collection or view it was opened
  from; the command line and the MCP tools take `--collection` (`collection`), and otherwise use the
  book's or the thing's tenant, or the library's only one, and else say which tenants there are and ask
  rather than mix (`Tenancy::chosen`). The window chooses among them (#96).
- **Made by hand in a tenant**: a thing's data keeps the tenants it was made in (`"tenants"`), which
  travels with it, and the membership follows from it as from the rest (format 27, which places the
  whole graph again by the amended rules). It is in the whole library's tenant too, so the one-tenant
  setting shows it.
- **A harness rule** (`rules/cpp/graph-reads-keep-to-a-tenant.yml`) flags a query that reads the graph's
  tables without the tenant's condition, outside the storage and the log.

## Considered options

- **Filtering at read time** (until now): one graph, no duplication; but every reader has to remember
  the filter, and the ones that didn't leaked. Shared nodes can't be filtered at all.
- **One home tenant per book**: no duplication; but a book in two collections would be missing from the
  graph of one of them.
- **Tenants in storage, a book written into each of its tenants**: the first choice. Separation holds by
  construction, and a reader can't forget a filter. Rejected after the performance analysis of
  2026-11-20: every thing, fact and relation of a book in k top-level collections is stored k times,
  and so is its part of the append-only change log, which is most of the file (about 1.9 KB per thing
  and 500 B per relation in the log, against 170 B and 83 B in the graph). At 1.5 collections per
  book a library of 75,000 books grows from about 2.3 to 3.4 GB, the first open (baseline) and
  reconcile grow k-fold, and moving a book is a copy and a removal in the log. A log can't be shrunk
  afterwards, so this had to be decided before it shipped.
- **Stored once, with a tenant membership** (chosen): no duplication in the graph or the log, a move
  changes only membership, and the membership tables are small and lead with the tenant. The cost is
  that separation is the readers' to keep: every reader filters by tenant, which the harness check of
  #95 enforces.

## Consequences

- A tenant is part of every graph query (#95). Membership is not in the change log (ADR 0001): it is
  derived on each computer from what the log carries.
- Storage grows by a few integers per thing, relation and fact for each tenant that sees it.
- Published, subscribed and shared spaces and story worlds keep scopes of their own, inside the tenant
  they belong to.
