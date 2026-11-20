# 3. Top-level collections are tenants of the knowledge graph

- Status: accepted, to be built in parts (#92)
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

Each top-level collection is a **tenant**: its own part of the graph, stored apart, with nothing shared,
merged or read across tenants. Books in no top-level collection are the **Unfiled** tenant.

- A book in two top-level collections belongs to both: what is learned from it is extracted once and
  written into each tenant, which then stand alone.
- Every node is per tenant, authors, series, tags and themes included.
- Every reader of the graph takes a tenant; there is no merged view. A setting, "one tenant for the
  whole library", keeps the old behaviour for those who don't want tenants.
- Moving a book between top-level collections moves its knowledge.
- The existing general graph is split once, after the backup every migration makes.

## Considered options

- **Filtering at read time** (until now): one graph, no duplication; but every reader has to remember
  the filter, and the ones that didn't leaked. Shared nodes can't be filtered at all.
- **One home tenant per book**: no duplication; but a book in two collections would be missing from the
  graph of one of them.
- **Tenants in storage, a book written into each of its tenants** (chosen): separation that holds by
  construction; the cost is storage for books in several top-level collections, and work when a book
  moves.

## Consequences

- Storage grows by the knowledge of the books that are in more than one top-level collection.
- A tenant is part of every graph query and of the change log (ADR 0001); a harness check flags graph
  queries without one.
- Published, subscribed and shared spaces and story worlds keep scopes of their own, inside the tenant
  they belong to.
