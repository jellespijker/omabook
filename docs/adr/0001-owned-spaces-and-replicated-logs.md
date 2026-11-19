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

As built (#37): compaction writes, per space, a snapshot (`snapshot_ops`) of the operations that still
matter up to a mark per device (`snapshot_marks`): the latest of each field a device wrote, and every
operation of what only grows (sightings, facts, relations, reading days, same-as links). What merges
or sends the log reads the snapshot and the operations after the mark (`live_ops`), which merge to the
same state as all of them, for every device; a peer behind the snapshot is sent the snapshot. The
operations the snapshot covers and leaves out are dropped from `ops`, but only those every member of
their space has seen, as each said when it last synced (`member_vectors`). Compaction is the one place
that deletes from the log (rule `ops-deleted-only-by-compaction`); nothing ever updates an operation
(`ops-append-only`). The freed pages go back to the file at the next VACUUM.

Since #2 the snapshot takes the place of the operations it covers: those every member has seen are
dropped from `ops` whether the snapshot keeps them or not, so only what came after the marks (and what
a member hasn't seen yet) stays there, and the library is smaller after compacting than before. What
looks up a thing's operations or a device's last number reads the snapshot and its marks too. A
published space's snapshot is a file, `.omabook-space` (`spacesnapshot.h`): JSON, versioned
major.minor, with the space's id and kind, its owner's chosen name and person id, the snapshot's clock,
the Omabook that wrote it, and its operations compacted (`PublishedSpace`); samples of it in
`tests/data/` must keep reading.

**Subscribing** (#4): importing a published space's snapshot is subscribing to it. Its operations are
kept in `ops` under the space's id as its owner wrote them, the space recorded as published with its
owner a writer and this computer a reader (`subscribedspace.h`). What the library's rows are made of
(`live_ops`) leaves every published space out by its id, so a layer never merges into the
subscriber's books, collections or graph, and is read by itself from `space_ops` (every space, which
compaction and replication read); a subscription doesn't travel to the subscriber's other computers.
What is new in a later snapshot is what the subscriber doesn't hold yet, by device, clock, thing,
field and value, as a snapshot numbers its operations anew until publishing keeps them. The
subscriber's own part is one collection, theirs like any other, linked to the space: their matched
books and wishlist entries, filed as the space files them. Unsubscribing drops the space from the log
whole (`Compaction::forgetSubscribed`, compaction still the one place that deletes from it) and keeps
the collection. The layer's graph is not shown until it follows the receiver's reading place (#5);
copying chosen things of it into one's own graph is for later.

As built (#3): on the owner's computer a published space has a log of its own, `published_ops`,
apart from `ops`, though `live_ops` now leaves published spaces out. In `ops` it would be a space like
those its members sync, compacted by snapshot marks it doesn't keep and sent by replication to the
owner's other computers, each then appending its own copy, while its numbers must stay as
subscribers hold them; and which collection it publishes, with which parts, needs a table anyway.
The log follows the collection by being brought in step with what the space says now
(`PublishedLog::follow`): a field's new value, what only grows and is new, a thing back or gone, a part
switched off said to be empty, appended after what is there and never changed, written by the
owner's person id under a name of this computer's own in that space. It is brought in step whenever
the space is published or its snapshot written, and a transport does the same before it sends; what
it appends then is what following every write would have appended, less values overwritten since,
which can't win anyway. So its numbers stay: a snapshot is the log compacted with each operation's
number in the log, and one who holds it needs only the operations after it. What the space says of
the graph is cut where the owner is in each book (`GraphContext::reachOf`): a sighting, a fact or a
relation learned further on stays out until they get there, nothing of a book not begun goes, and a
thing's summary goes only as its facts, each with the place it was learned at.

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

As built (#47, #48):

- **One key per person, kept per user.** The key pair (Ed25519) is in the user's Omabook data folder
  (`$XDG_DATA_HOME/omabook/person.key`, owner-only), never in a library folder, which may itself be
  synced somewhere. Every library of the user signs with it.
- **Every operation is signed.** `ops` gains `signer` (the person's id, in `log_ids`) and `signature`
  (format 16, only added columns). An operation written now has the person as its `author` and this
  computer as its `device`, and is signed in exactly the bytes it is kept as (`SignedOps`, canonical
  bytes `omabook-op` 1). Operations from before carry neither and are trusted as one's own. A snapshot
  (compaction) keeps each operation it holds as it was, signature and all, so what it holds can still
  be verified by whoever it is sent to; nothing is re-signed. A published space's operations are its
  own (its space, its devices, its numbers), which is not what was signed at home, so they go without
  those signatures. Signing them by the owner as the space has them, and carrying that in the
  `.omabook-space` file (a minor version of it may add to each operation's end), is still to come.
- **Verified before replaying.** Replication sends an operation's signer and signature after its nine
  parts to a peer that says it signs (`"signs": 1`), and only the nine to an Omabook from before. The
  side that takes operations verifies every signed one first: one that is not as its author signed it
  is refused, never stored or applied, and the sync's summary counts and says it. An **unsigned**
  operation is taken only from one of one's own computers (a device paired into the personal space),
  for Omabooks from before signing and the operations written before it. Such an operation could be a
  signed one with its signature stripped; that is accepted because one's own computers can write the
  library anyway. Every ssh peer is paired at its first replication, so for ssh this rule changes
  nothing yet; it holds the line for the transports to come, where it will matter.
- **Pairing gives one person by copying the key.** When two updated computers replicate, they keep the
  personal space with the smaller id (as before) and the person with the smaller id: the other computer
  is given that person's key, in the replication request (a file in a scratch folder only the user
  can read, removed after) or in its answer, over the ssh the sync runs on, and keeps it in place of
  its own. Since the smaller id always wins, any of one's computers that meet
  settle on one person, also two groups paired apart, and none goes back to a key it gave up. What was
  signed with a key given up stays signed by it and verifies. `omabook identity` shows the computers
  paired.

  *Why copying the key, and not a key per computer vouched for by the person.* The alternative keeps
  a key on each computer and makes the person a set of device keys, each added by a signed membership
  operation. It would let a lost computer be shut out without the person changing. But every verifier
  then needs the person's membership history, in order, before it can check anything; every shared
  space must learn of each new computer; and revoking needs rules of its own (what of what the lost
  computer wrote before?). Copying the key adds no trust that isn't there: ssh to a computer already
  reads every file of the user there, the key file included, and the sync already hands it the whole
  library. One person is one id, which is what others will name in a shared space's members. The
  `signer` column is apart from `author` so that a person can later sign for device keys, if revoking
  a computer becomes worth it. Until then, a lost computer means a new person (open, with #38).

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
