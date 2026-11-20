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
| A bookmark, a highlight | `a:` + a hash of its book's id, kind, section, fraction, words and note (then kept, though renamed) |

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

As built (#103): a sync reads of the log only what the other side lacks, in SQL, by ranges of each
space and device after its number in the other's vector (`ops` after the mark, and the snapshot
before it only for a side behind the mark, by an index on `snapshot_ops(space, device, seq)`), and
its own vector from the end of each range. Compaction runs after a sync when over a thousand
operations are of a device with no mark here, as another computer's whole log is at a pairing,
however large the snapshot: what both have seen goes from `ops` as above. The snapshot still keeps
each device's own latest of a field, the same value said by two computers included, since a sync
reads what each side's device holds (`SyncTake`).

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
compaction and replication read); since #51 a subscription travels to the subscriber's other computers (below).
What is new in a later snapshot is what the subscriber doesn't hold yet, by device, clock, thing,
field and value, as a snapshot numbers its operations anew until publishing keeps them. The
subscriber's own part is one collection, theirs like any other, linked to the space: their matched
books and wishlist entries, filed as the space files them. Unsubscribing drops the space from the log
whole (`Compaction::forgetSubscribed`, compaction still the one place that deletes from it) and keeps
the collection. Copying chosen things of the layer's graph into one's own is for later.

As built (#5): the layer's graph is shown beside the receiver's own wherever that is shown or told (the
graph's window and its cards, the marks on the page, a search by what is known, `omabook graph show`
and the MCP server, what the AI is told), always cut at the receiver's own place in each book: there
is one cut, `GraphContext::layers`, by the rule the AI already kept for one's own graph (`reachOf`:
all of a book read, up to the furthest place of one being read, nothing of one not begun or only
wished for), and a place asked for further on never shows more. The layer is read from the space's
operations (`GraphLayers`), never written into the graph's rows; what it says of no book (put in by
hand) has no place to cut at and is left out. Its things have ids below zero and say whose they are.
Where each of the space's books is here is recorded when subscribing (`global_ids`, per space), so a
book matched by identifier or title, or a wishlist entry, is the same book to the cut.

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
thing's summary goes only as its facts, each with the place it was learned at. A part switched off
is taken back, not only no longer sent: each thing of it gets its removal (a reading day too, with an
id of the space's own so its removal names no day) and each field of it is said to be empty, and a
snapshot holds of a thing gone only its removal. A collection deleted at home is followed by the
removal of all its space held.

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
  a computer becomes worth it. Until then, a lost computer means a new person (still open after #38).

As built (#38), shared spaces:

- **Membership travels in the space.** A shared space (`shared:…`) is founded by an operation (entity
  `space`, field `founded`: the founder's id, a nonce, the name and parts) and its id is a hash of the
  founder's id and the nonce, so nobody else can found it. Each change of a member is an operation too
  (entity `member`, the person's id, field `role`: `writer`, `reader` or `removed`), signed and ordered by
  its clock like everything else, so membership goes wherever the space goes (`sharedspace.h`).
- **The role rules.** An operation is taken only when it is signed by its author and the author was a
  writer at its time: by the founding and the role changes before it, in order of clock, then device,
  then number. Anything else is refused and said, never kept or applied; a reader gets everything and
  writes nothing. The rules are applied again to everything held each time the space is read, so a
  removal that arrives late still refuses what came after it; compaction keeps every operation of a
  shared space, since which of them count depends on the history.
- **Removal doesn't take back.** A member removed has every later operation refused, and what they wrote
  before stays (no retroactive rejection). It doesn't protect what they already had: their copy of the
  space and its old keys stay theirs. A clock is what its writer says it is, so a removed writer could
  date an operation before their removal; since #49 removal changes the key and such an operation is
  refused (below).
- **Inviting is handing over the key.** `omabook spaces invite` records the member and writes an invite
  file (`spaceinvite.h`): a sealed box (`crypto_box_seal`) to the invitee's Ed25519 key converted to
  X25519 (`crypto_sign_ed25519_pk_to_curve25519`), holding the space's id, name, role and key, signed by
  the inviter, so only the invitee opens it and knows whom it is from. A space's key is kept per user
  beside the person's key (`spaces/<id>.key`, owner-only, written whole), never in a library folder.
- **A layer, like a published space.** Its operations are in `ops` under its id, and `live_ops` leaves
  `shared:` spaces out (format 19), so they never make a member's own rows; nor does ssh replication's
  log carry them to one's other computers (since #51 they go beside it, below). What a member gets is a collection of
  their own linked to it, filed as a subscription is (`Subscriptions::file`). A writer's edits go the
  other way: their linked collection, read as publishing reads it, and of each field what they changed
  later than the space, written as theirs (`SharedCollection::contribute`).
- **The folder transport.** `omabook spaces sync <space> --folder <dir>`: in `<dir>/<id>/` each computer
  writes only its own append-only batches, `<device>-<first>-<last>.omaseal`, each its new operations
  signed and sealed with the space's key (secretstream, the space's id as additional data), written
  whole under a temporary name and renamed; the folder never holds plaintext. Reading opens the batches
  with numbers past what is held of each device (a version vector); one changed, cut short or of another
  space is refused whole. WebDAV and S3 would carry the same files; they are not built.

As built (#50), what a computer was as a person it gave up at a pairing:

- **The key given up is kept.** Pairing keeps the person it gives up beside the person, in
  `former-persons/<id>.key` (owner-only, written whole, never over one there), before the new key
  takes its place; when that can't be done the computer stays who it was. With it what that person
  was a member of goes on: an invite sealed to them still opens (`spaces join` tries each).
- **A writer hands itself over, by the space's own rules.** In each shared space where a person given
  up is a writer and the person now is not, the computer writes a role change making the person now a
  writer, signed by the one given up: any writer may, so every member's Omabook takes it and
  everything after it, with no one else to ask. It does so at the pairing and again at each sync
  and join of the space, so the order doesn't matter: an invite to who it was, joined after the
  pairing, hands over once the inviter's role change for it arrives. The one given up is not removed:
  another of one's computers not yet paired may still be writing as it, and a removal would refuse
  that for good.
- **A reader stays the reader it was.** A reader makes nobody a member, and asking a writer to
  confirm needs a way to ask that is not built. A reader writes nothing, and reading needs only the
  space's key, which is per space, not per person; so the computer is that member as the person it
  gave up (`SharedSpace::memberAs`) until a writer makes the person now one, or the one given up a
  writer, which then hands over as above.
- **A published space's owner moves.** Its id stays, since those who follow it know it by that, though
  it was made from the person who first published it (`PublishedSpace::spaceFor` finds it again).
  Its log says, as the last it writes as the one given up, that it is handed to the person now
  (entity `owner`, the one before as its id, field `handed_to`, a field of its own per hand-over, so
  compaction keeps each), and the person now is its owner and writes it from then on. A subscriber
  takes the operations of the owner now and of each who handed it on to them, and refuses a space
  whose owner changed without that. An Omabook from before refuses a snapshot after a hand-over, as
  not all its operations are its owner's; it is updated to follow on.
- **A subscription stays as it is**: its reader is this computer (`space_members`, by device), not a
  person.

As built (#51), what one's own computers carry to each other of the spaces one is in:

- **Shared spaces and subscriptions go wherever one is.** The ssh replication between two of one's
  computers carries, beside the log, each shared space one is in (its name, its key, the collection
  linked to it here, and the operations the other lacks, by a version vector per space) and each
  subscription (its operations, owner, linked collection and the place it is followed from, sent
  when what the other holds of it differs). On arrival nothing is trusted for coming from oneself: a
  shared space's operations go through its role rules and signatures (`SharedSpace::take`), a
  subscription through the check of a snapshot (`Subscriptions::subscribe`), and each is filed into
  a collection of the receiver's, matched to its own books, so a layer's graph is cut where the
  receiver is in each book (`SpaceCarry`). It goes in the sync's last exchange of the log
  (`SyncLog::finish`), once books have moved, so what is filed finds them there rather than making
  wishes of them; and after any pairing in the same sync, as each side's answer says who it is by
  then.
- **Only to the same person, sealed and signed.** Each side says it carries and, sealed to its
  person, what it holds. What is carried goes only to a side that says it is the same person, as one
  sealed box to that person (`Person::sealFor`), signed by them; the receiver takes it only when the
  signature is its own person's. So it opens only where the person's key is, and is taken only from
  where it is: a peer that only claims the id opens nothing, and one not paired (its person another,
  as at the first sync before the key goes over, or an Omabook from before, which doesn't say it
  carries) is sent nothing of it. A key arrives for its owner alone and whole (`SecretFile`), never
  over one there. Every generation of a space's key is a list's entry of its own, with its generation
  (0 for the founding one) and its id, and each is kept that isn't there yet (#49).
- **Not a choice.** A subscription made on one computer is made on all, with no per-subscription
  switch: one's computers are one person with one personal space, which carries everything else of
  theirs without asking, and the collection a subscription fills and its wishlist entries travel in
  it anyway, so a layer kept on one computer only would leave the others a collection without its
  graph or its updates.
- **Unsubscribing travels too, as an operation.** Whether one follows a subscription is said in the
  personal space (entity `subscription`, the space's id, field `followed`, `1` or `0`), written at
  subscribing and unsubscribing, the latest winning like any field. A computer that holds a
  subscription the latest word says is given up drops it (`SubscribedSpace::drop`); one carried a
  subscription given up doesn't take it; subscribing again later wins over the giving up. A
  subscription from before says nothing and counts as followed. Kept as a field, it merges in any
  order and survives compaction, which keeps the latest of each field; an Omabook from before
  ignores it.
- **Still open.** Leaving a shared space is not built, so there is nothing of it to carry. A member of
  a space as a person given up at a pairing (#50) is carried the space and its key but not that
  person: the computer it reaches is a member as the person now, once a writer makes it one.

As built (#49), changing a shared space's key when a member is removed:

- **A rotation is an operation.** `spaces remove` writes the removal, then a rotation (entity `key`, the
  new key's id, a BLAKE2b hash of it, field `rotated`; `keyrotation.h`): its generation (one more than the
  newest its writer knew), its cut-off (of each device the last number its writer held, a version
  vector, and of its own device itself), and the new key sealed (`crypto_box_seal`) to each member left,
  the writer included. Signed and judged by the role rules like any other. Old keys are kept, never
  replaced, beside the founding one (`spaces/<id>.<key id>.key`, owner-only, written whole;
  `SpaceKeys::all` gives every generation of a space, for whatever carries spaces to one's other
  computers), so old batches stay readable.
- **The cut-off rule.** An operation past a rotation's cut-off on its device (or of a device it doesn't
  name) is taken only from someone the new key was handed to, or made a member past the cut-off by one
  who was. The new key is the only thing that changed: past the cut-off, whoever it wasn't handed to can
  only have sealed under an old one, so their operation is refused and said, whatever its clock. It is a
  rule over the operations, not over which key opened a batch, so it is applied again on every read like
  the role rules, and a rotation that arrives after the operation still refuses it. A member left who
  wrote under the old key just before learning of the rotation is not refused: their batches can't be
  rewritten, and nobody else can sign as them. What the remover hadn't taken yet of the removed one is
  past the cut-off too, and refused: sync before removing.
- **Which key seals what.** A computer seals its operations with the newest key it holds, except what
  it wrote up to a rotation of its own (with the key before, so a member away can read up to it) and a
  rotation itself, which goes under the founding key: every member holds that one (an invite now hands
  over every key), and a rotation gives nothing away to the removed, its key being sealed to each member.
  A member away reads up to the rotation, takes the new key from it and reads on; the folder is read
  per device in order up to the first batch that doesn't open, so nothing is taken past a gap.
- **Rotations at once.** Two writers removing at once each write a generation of the same number. Both
  hold: each has its cut-off, each key opens its batches, and new writes go under the newer by
  generation, then key id, so every member picks the same. When the newest was handed to someone who is
  not a member now (Ada removed Bo while Dee removed Cy, each handing a key to the one the other
  removed), or a writer doesn't hold it, the next writer to sync rotates again, to the members now; two
  doing so at once hand theirs to the same members, so it settles. A rotation stands only by what its
  writer held (the operations within its cut-off), so an operation dated before it can't unseat it, and
  of two at once where each leaves the other's writer out (each removed the other), the one whose writer
  has been a writer longer prevails. A removed writer who backdates their remover's removal, or a
  rotation of their own, is refused by the remover's rotation; between two writers who really remove
  each other at once, the senior stays, and a removed senior can claim to have done so: a limit of
  self-asserted clocks.
- **What it doesn't protect.** Whatever the removed member held before the rotation stays theirs, and a
  rotation's own batch (and the founding key) stays readable to them: it holds only sealed keys. A
  reader invited at the very time someone rotates may lack the newest key until a writer rotates again
  or invites them anew.

### Book files

Files are referenced by their hash and fetched only when wanted. Between one's own computers they
travel as today. To other people a space carries a book's details, and its file only when the
space's owner says so: a reader who has the book is matched by hash or identifiers, and one who
doesn't gets a wish. A household space can say its files travel too.

As built (#63), a space's files, when its owner allows (owner's decision: books one owns may be
shared with anyone the people sharing the space allow):

- **A switch per space, off until turned on.** In a shared space any writer turns it, as an
  operation (entity `space`, its id, field `files`, `1` or `0`) judged by the role rules, the latest by
  clock winning (`SharedSpace::State::files`); in a published space its owner, as a part of it
  (`PublishedSpace::Parts::files`, the part `files` in the snapshot, which a reader from before ignores).
  Whoever turns it on is told that the files go to everyone the space reaches (`SpaceFiles::said`).
- **Files by their hash.** With it on, the space says of each book its `formats` (each file's format,
  hash and size, never where it lies: `PublishedSpace::filesSaid`), as the owner's log says them, or a
  writer's (`SharedCollection::contribute`, which never says a book has none: a member who lacks it
  says nothing of the others' files). Off, a published space clears the field, as any part switched off.
- **Fetched only when wanted, never in place of one's own.** A member's book of the space (matched or a
  wish) is given a file when it is opened, or with `spaces fetch` (`SpaceFiles::fetch`), and only when it
  has none: a copy matched by file, identifier or title stays as it is. What arrives is taken only when
  its content has the hash the space names.
- **Per transport.** A shared space's folder keeps the files in `files/` beside the batches, each
  sealed with the newest key as a batch is (the space's id and the file's hash its additional data),
  named by the file's hash keyed with the founding key (`SpaceKey::nameFor`), so the folder says nothing
  of which file it is; each member writes there, once, the files of the space's books they have, at their
  sync, and the folder last synced through is where a member fetches from (`SpaceLinks::Role::Files`).
  A published space's `.omabook-space` file stays as it was: the files go beside it, as they are, in
  `<name>.omabook-files/<hash>.<format>`, so the snapshot stays small enough to mail and a subscriber
  from the file fetches from beside it. A gist carries no files: it is no place for large binaries;
  sending one says so, and a subscription that follows one is told to ask for the space's file. A
  repository release could carry them later.
- **A wish filled by its file.** Adding the very file of a book a space made a wishlist entry of fills
  that entry (the space knows it by `b:` and the file's hash, `SubscribedSpace::wishedFiles`), whatever
  the file's own details say, as an identifier or title and authors already did (`importBooks`).
- **Still open.** Switching the files off doesn't take back what members fetched; nothing could. A
  member removed from a shared space can still read the files sealed before the key changed, as with
  everything else, and can tell from the names whether a file they know was written later.

As built (#25): a book in the format of a plugin that isn't installed (a Storyteller story) still
travels between one's own computers and is kept there, saying which plugin reads it (the plugins'
registry lists the formats each reads). The graph of its story world (its `world:…` space) goes
with it by the log, between one's own computers only, and is taken there as the world's own, never
matched with the general graph. The manifest sync with an Omabook from before the log carries no
world.

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

   As built (#54), the baseline is a snapshot:

   - **Written as compaction leaves the log.** The baseline goes into `snapshot_ops`, with a mark
     per device in `snapshot_marks`, not into `ops`: the same operations, of the same global ids,
     values and times (a row's own time, `hlc` 0 for undecided details), numbered from 1, and the
     log carries on after the marks. A baseline holds one operation per field and every operation
     of what only grows, which is what a compaction would keep of it. Everything that reads the
     log reads the snapshot with it already (`live_ops`, `space_ops`, a version vector up to the
     mark), so replication, pairing's one-time matching, the reducers, reconcile and compaction
     treat it as they treat a compacted log, and a peer that has nothing of it is sent all of it.
     Since nothing of it was ever in `ops`, compaction has nothing of it to drop.
   - **Unsigned, and taken only from one's own computers.** Signing each operation of it was
     6.3 MB of a real library, and most of the time of its first start. One signature over the
     snapshot as a whole would not hold either: compaction makes the snapshot anew, keeping some
     operations and leaving others out, and a peer keeps what it takes in its own `ops`, so the
     bytes signed would be gone after the first compaction on either side. So its operations are
     what the operations from before signing are: unsigned, their author this computer. The rule
     for those holds for them: an unsigned operation is taken only from one of one's own computers
     (a device paired into the personal space, `Replication::admit`), which can write the library
     anyway, and refused from anyone else. Ssh pairs a peer before taking anything of it, so the
     first sync of two of one's computers carries both baselines. Only the personal space and
     story worlds are baselined, and they travel only between one's own computers; a published,
     shared or subscribed space never has a baseline, so nothing unsigned of it is ever taken.
   - **Kept as it is looked up** (format 21): the snapshot's key is the thing, its field, then
     space, device and number, in place of an index beside it, which came to a sixth of the file.
   - **What it costs still.** The log keeps every value the rows hold, and each thing's global id
     twice (`global_ids`, `log_ids`), so a baseline costs about as much again as the library's own
     data and more: a format-12 library made of 1,560 copies of the samples goes from 1.9 MB to
     8.7 MB (15.1 MB with a signed operation each), its first start from 13.3 s to 4.6 s. Smaller
     than that needs a log that doesn't repeat what the rows say, which this doesn't attempt.
   - **Said while it runs.** The window opens a library in an older format on a worker first,
     saying "Bringing your library up to date…" (`LibraryUpgrade`), and a command says so on
     standard error (`Library::onBringingUpToDate`).

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

   As built (#66): the bookmarks and highlights are the first kind to move over after a release
   that speaks the log, as one entity, `annotation`, of every kind (a highlight, and later a margin
   note, needs no kind of its own). An Omabook of the release before takes none of them, though its
   vector then holds what came after them, so each side also says the last of each device it holds
   of them alone (`"annotations"`, `Replication::annotationVector`), and is sent those after it,
   whatever its vector says. One that doesn't say it is from before, and takes none.

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
