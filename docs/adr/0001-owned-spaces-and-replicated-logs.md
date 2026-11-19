# 1. Owned spaces, replicated as change logs, in place of state sync

- Status: accepted, to be built in parts (#33)
- Date: 2026-11-16
- Replaces: the manifest sync (`sync.h`, `graphsync.h`, `Collections::take`)

## Context

Each computer keeps its own whole library in its own SQLite database, and only its own Omabook writes
it. That stays: reading works offline, and a database is never opened by two computers at once.

What keeps two computers in step today is a **state comparison**. One computer asks the other for its
whole library (`omabook manifest`: every book, collection and the whole knowledge graph, several
megabytes), compares it with its own, and merges each kind of thing by a rule of its own: a book's
details by the newer `details_at`, a collection whole by its `updated`, the graph by a union keyed on
names and other names, removals by a separate table. This has costs that grow with every kind of thing
that travels:

- **A merge per kind of data**, each with its own code, tests and corner cases, and removals kept
  apart from everything else.
- **The cost is the size of the library**, not of what changed, and every pair of computers does it.
- **It can take more than one round to settle** (#31): a thing that merges under another name on one
  side comes back as a name of the other's.
- **There is no person in it.** A sync is ssh access to another computer's whole library: fine for
  one's own computers, impossible to give to someone else, and all or nothing.

And what is wanted next does not fit it at all: sharing a collection with other people (#1–#8),
a library several people share, collaborating on a list or a graph, and choosing what of one's own
reading (progress, questions, stars) others may see.

## Decision

### Spaces: every piece of data has an owner

Everything that travels belongs to a **space**, and a space decides who may write it.

| Space | Written by | Holds |
|---|---|---|
| **Personal** (one per person) | that person's own computers | the catalog, reading places, stars, shelves, finished days, questions, notes, the graph learned while reading |
| **Published** | its owner | what was chosen to share: a collection, and with it, as chosen, its graph, ratings or progress |
| **Shared** | its members with the writer role | a household library, a book club, a list several people keep |
| **A story world** | the plugin's story | what Storyteller keeps of a made-up world (today the `world:…` scopes) |

**Nothing personal leaves by accident.** Sharing "my progress in this book with the book club" is an
explicit *projection*: that fact is written into the club's space. Stopping it stops new projections,
and members drop their copy.

### A change log per space

Every write to a table that travels also appends an **operation** to its space's log, in the same
transaction (a transactional outbox). An operation is

```
(space, device, seq, hlc, author, entity, id, field, value)
```

- `device` and `seq`: which computer wrote it, and its place in that computer's log.
- `hlc`: a **hybrid logical clock**, the wall-clock time with a counter, so that writes order correctly
  when computers' clocks disagree.
- `entity`, `id`, `field`, `value`: what changed, by a **global id** (below), never a row number.

Each kind of change merges by a rule that gives the same result in any order, so computers that have
the same operations hold the same state (the data types of CRDTs):

| Data | Rule |
|---|---|
| A field: a title, a rating, a reading place, a collection's rule | the latest `hlc` wins; a tie goes to the larger `device` |
| A set: tags, a collection's books, a book's other names | add wins over a concurrent remove |
| A graph fact, where a thing was met | a growing set; of the same fact, the earliest revealed moment |
| A removal | a tombstone with its `hlc`; a later change brings the thing back |

The graph already merges this way today (#30); it is the model the rest follows.

### Global ids, derived from content where they can be

A row number differs on every computer, so operations name things by a **global id**, kept in a table
beside the rows. Ids are derived from what a thing *is* wherever that is stable, so two computers that
make the id of the same thing independently make the same one:

| Thing | Global id |
|---|---|
| A book with a file | `b:` + the smallest of its files' hashes, when it is first given an id (then kept, though formats come and go) |
| A book without a file | `e:` + its first entry key (`Library::entryKeys`) |
| A collection | `c:` + its path |
| A graph thing | `n:` + a hash of its scope, kind and normalised name |
| A relation, a fact | a hash of its ends and kind; of its thing, kind, value and book |

Where content can't decide, as when one computer knows a thing by a name the other knows as another
name, a **same-as** operation folds one id into the other, and both sides follow it.

### Replication: only what is new

Two peers exchange, per space they share, what they have (`{laptop: 812, desktop: 77}`, a version
vector) and send each other the operations after that. Applying an operation twice changes nothing.
The cost is the size of the change. The rows stay what the app reads; the log is how they travel.

**Compaction**: a snapshot of a space plus the operations after it. Operations every member has are
dropped behind the snapshot. A published space's snapshot is what a share file is (#2).

As built (#37): `ops` stays append-only, and nothing is dropped from it. Compaction writes, per space,
a snapshot (`snapshot_ops`) of the operations that still matter up to a mark per device
(`snapshot_marks`): the latest of each field a device wrote, and every operation of what only grows
(sightings, facts, relations, reading days, same-as links). What merges or sends the log reads the
snapshot and the operations after the mark (`live_ops`), which merge to the same state as all of
`ops`, for every device. Dropping covered operations from `ops` itself, which is what shrinks the
file, waits for a decision to let compaction, and only compaction, delete from the log.

### Transports are adapters

How operations move is not the model. They are append-only and can be encrypted with the space's
key, so any store that keeps files can carry them, and none of them is trusted with the content:

- ssh over Tailscale, between one's own computers (today's `Remote`)
- a folder that something else syncs, a gist or a repository (#7), WebDAV or S3
- an always-on peer (a small home server), later, as one more member and nothing more

### People and keys

A person is a key pair; a computer joins a person's personal space by **pairing** with one that is
already in it (over ssh, as sync is today). A shared space has members (person, role: reader or
writer) and a key; inviting someone is giving them the key, and the operations they write are signed.

### Book files

Files are referenced by their hash and fetched only when wanted. Between one's own computers they
travel as today. To other people a space carries a book's details, never its file: a reader who
has the book is matched by hash or identifiers, and one who doesn't gets a wish. A household space
can say its files travel too.

## Migration of existing installs

Every install today has a library written by the manifest sync, and some are kept in step with each
other by it. None of that may be lost, and a computer that has been updated must keep syncing with
one that hasn't.

1. **Back up, then add.** The schema migration that adds the spaces (`spaces`, `space_members`,
   `ops`, `seen`, `global_ids`) backs the database up first (`Library::backUp`, as every migration
   does) and only adds tables. An older Omabook refuses a library in a newer format and says where
   the backup is, as for every format change, so a library is never written by a version that
   doesn't know the log.

2. **Baseline.** On the first start after the migration, each computer gives every row its global id
   and writes **baseline operations** for the library as it is, into a personal space it creates. A
   baseline operation's `hlc` is the time the row already keeps (`details_at`, a reading place's
   `updated`, a collection's `updated`, a graph thing's `updated`, a removal's time), with counter 0.
   - Two computers that the old sync kept in step hold the same rows with the same times, so their
     baselines say the same thing, and taking each other's changes nothing.
   - Where they differ, the later time wins, which is what the old sync would have decided.
   - Rows without a time (details from before times were kept) get `hlc` 0 and are *undecided*
     exactly as today: the user settles them with `--details mine|theirs`.

3. **The first replication pairs.** Two updated computers that sync for the first time each have a
   personal space of their own making. Because they already sync over ssh they trust each other, so
   that first replication **pairs** them: they keep the space with the smaller id, and the other's
   operations move into it. The content-derived ids line their books, collections and most graph things
   up without any matching; what is left (things known by different names) is matched once with the
   rules today's sync uses (hashes, entry keys, names and other names) and recorded as same-as
   operations. That matching code is kept for this and for older peers, and nothing else.

4. **Old and new peers together.** Each side says in its manifest whether it speaks the log
   (`"log": 1`). With an Omabook from before, the manifest sync runs as today, unchanged. Between
   two that speak it, the log carries what has moved over to it (reading places first, then the rest,
   one kind at a time) and the manifest the rest, with those kinds left out of it. Each kind that
   moves deletes its manifest merge.

5. **Writes that bypass the log.** Something other than this Omabook that writes the database (a
   backup put back, a tool, a bug that writes a row without its operation) leaves rows the log doesn't
   know. On opening, the change counters (`library_state`) are compared with what the log last saw;
   where they differ, the rows are compared with what the log says and the difference is written as
   operations.

6. **Retiring the manifest sync.** It stays for older peers for three minor releases after the log
   carries everything. After that an Omabook that only speaks the manifest is told what it is told
   today when versions don't match: update Omabook on that computer.

7. **Proving it.** A characterization test syncs two copies of the library samples in `tests/data/`
   (from 0.1.0 on) once the old way and once by migrating both and replicating, and expects the same
   libraries. It stays until the manifest sync is gone.

## Rollout

| Release | Part | What changes for a user |
|---|---|---|
| 0.1.6 | #31: hybrid clocks and field-wise merges in today's sync | a sync settles in one round |
| 0.1.6 | #35 Spaces 1: global ids, the log written alongside, the migration (steps 1, 2, 5) | nothing visible |
| 0.1.6 | #36 Spaces 2: reading places through the log (steps 3, 4) | nothing visible; a sync is faster |
| 0.1.7 | #37 Spaces 3: details, collections, removals and the graph through the log | nothing visible; the manifest shrinks |
| 0.1.7 | Sharing (#1–#8): a published space, subscribed to by others | sharing a collection |
| later | #38 Spaces 5: shared spaces with several writers, membership and keys | a household library, a book club |

## Consequences

- **Gone**: a merge per kind of data, the separate removal bookkeeping, the settling rounds, and the
  cost of comparing whole libraries.
- **New**: global ids, a clock, an operations table per space, and a rule that a row that travels is
  only ever written together with its operation.
- **The rows stay the source the app reads**, so the window, the commands and the MCP server don't
  change. If the log turns out wrong, writing it stops and the manifest sync, still there for older
  peers, carries on.

## Alternatives

- **A home server that holds the library** (as Kavita, Komga or Audiobookshelf): simple for several
  people, but something must always be on, writing offline needs a queue (a sync again), and it isn't
  how a desktop app is used. It remains possible as an always-on member of spaces.
- **A general CRDT engine** (cr-sqlite for SQLite; Automerge, Yjs): merging for every table for free,
  but every table must fit its constraints (no unique keys besides the primary one), the whole database
  migrates at once, and everything becomes multi-writer, including what only its owner should write.
  Omabook's data is a few simple shapes (fields, sets, growing sets) that a small log covers. Worth
  another look if shared spaces with many writers become common.
- **A git repository of the metadata**: merges are textual, and conflicts land on the user.
- **Fixing the state sync in place**: done where it is cheap (#31), but it can't give people,
  permissions or partial sharing.
- **Standards** (OPDS for a catalog others can browse, KOReader's progress sync, W3C Web Annotation for
  notes): good to offer from a space, as exports, not as how Omabook keeps itself.

## Guardrails

- **Order doesn't matter**: a property test applies random operations in random orders and to copies,
  and expects the same state every time.
- **One way to write**: an ast-grep rule allows writes to the tables that travel only in the
  operations layer.
- **Layers**: the transports (`remote`, and those to come) stay in `services` and never reach into
  the library's tables (`.agents/architecture.toml`).
- **The migration**: the characterization test of step 7.
