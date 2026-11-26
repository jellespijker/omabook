# 11. The hub is a peer, not an authority

- Status: accepted (#178)
- Date: 2026-11-22
- Builds on: [ADR 0001](0001-owned-spaces-and-replicated-logs.md) (the change log), [ADR 0009](0009-a-book-is-paired-by-its-global-id.md) (pairing)

## Context

A hub is an Omabook without a window that is always on: it keeps a person's libraries for their computers,
serves them to phones, does their background work and backs them up (#179–#197). Computers sync
peer-to-peer today, over ssh, each library merging the other's change log. A hub could instead be made the
library's source of truth, with computers as its clients.

## Decision

- **A hub is a computer that's always on.** It syncs with the person's computers by the same symmetric sync
  they use with each other. No side is the leader, and no copy is the truth.
- **Computers keep syncing directly with each other.** A sync between two computers without the hub gives the
  same libraries as one through it.
- **Without a hub, nothing changes.** Turning a hub off returns things exactly to peer-to-peer sync.

## Consequences

- No leader election, quorum or split-brain handling: none of it is needed for one person's few computers,
  and a hub that sits offline itself would make an authority a bottleneck.
- A mistake a sync makes still travels to every peer. It is caught by the sync guard (#187) before anything
  is written, and undone from snapshots (ADR 0014), not by an authority deciding.
- Several hubs (#196) are just more peers.

## Alternatives considered

- **The hub as the authority** (clients and a server): simpler conflict rules on paper, but every computer
  then depends on the hub being reachable to write, and a wrong write on the hub is right by definition.

## As built in #219

- **Who built a book's graph travels as a field of the book** (`graph`, `GraphBuilt`), not as a request to the hub: the
  hub says it is building a book's graph as its job begins, then that it built it (and how far) or stopped, and a
  computer learns it by the sync like any other field, from the hub or from another computer. So a computer leaves
  the book to the hub as it is read, takes how far the graph came built for its own, and says "built on <hub>" by
  the graph's count. Nothing waits on the hub being reachable: a hub that began a book and hasn't got on with it for
  `GraphBuilt::patience` is waited for no longer, and the person asking builds it here at once.

## As built in #196

- **Several hubs are just more peers.** A computer syncs with each in turn, each sync through the guard, and two
  computers with three hubs end with one library on all five; once they have, a round of syncs moves nothing and
  grows no change log. Automatic sync (`HubAutoSync`) takes what a sync with one hub brought as a change for the
  others, never for that one, so a change goes round the hubs once and comes to rest; a hub out of reach waits
  apart, and every hub that was is tried as the network comes back.
- **A book's graph is built once** (`HubClaims`): the hub that takes a book up claims it in the library's change log
  (plugin data under `omabook-hub`), so the claim travels by the syncs; another hub that knows of a claim made
  within a day leaves the book to it. Two claims made before either hub knew of the other end as the later wherever
  they meet, and the hub whose claim was overtaken keeps nothing of its work.
