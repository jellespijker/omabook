# 9. A book is paired across computers by its global id, and by an identifier only once

- Status: accepted (#158)
- Date: 2026-11-21
- Builds on: [ADR 0001](0001-owned-spaces-and-replicated-logs.md) (the change log and its same-as links),
  #137 (a book's global id), #154 (bookkeeping names no book)
- Replaces: #154's rule that two books sharing any entry key are one book

## Context

A sync has to know which book here is which book there. A book with a file is easy: the same content is
the same book, and its global id (`b:` and the file's hash) is the same on both. A book without a file (a
Goodreads entry, a paper copy, a wish) was paired by its **entry keys**: any identifier, the ISBN, else its
title and authors. Two books that shared one were linked as one book (`same_as` in the log), and from then
on a sync merged their details field by field, title, authors and ISBN among them.

Real libraries are full of shared "identifiers" that are not identities. In one library, 33 pairs of
different books share one: Calibre's custom columns stored as identifiers (`genre: Scifan`, `uri`,
`set_id`, `author_origin`), and plain errors (two different books with one ISBN or Amazon id). 0.2.0 paired
books to read by what Goodreads' last sync wrote (`goodreads:synced`, the same for all of them); #154 (0.2.1)
stopped that, but kept "two books that still share a key are one". One book had picked up another's
`barnesnoble`, `google` and `calibre` ids from the first corruption, so the link held, and the next sync
renamed it after the other again, on both computers. A wrong pairing is not a small error: a sync then
rewrites what a book is.

## Decision

- **A book's identity across computers is its global id.** Two copies with the same id, or two ids the log
  links (`same_as`) and BookLinks follows, are one book. Once paired, they stay paired by that, and nothing
  else pairs either of them again (BookPairing skips a book whose id stands for one here, and a book here
  that is paired with one of theirs already).
- **An identifier is only a first-meeting fallback**, for a book without a file whose id stands for none
  here. Only strong schemes count (BookIdentity): `isbn` (an ISBN-10 or -13 whose check digit holds, as its
  ISBN-13), `goodreads` (its number), `calibre` (a uuid; Calibre's number for a book names none). Never
  `*:synced`, `generated`, a custom column or any other scheme.
- **Only one to one.** A key that names two books on either side pairs nothing: each side sends only the
  keys no other book of its own has (`pair_keys`), and pairs only by a key that names one book of its own.
  Two keys naming two different books here pair neither.
- **And only with agreement.** The two titles must agree once case, punctuation and a subtitle (after a
  colon or a dash, or in brackets at the end) that one has and the other lacks are set aside: "The Lady of
  Crows" and "The Lady of Crows: A Novel" agree, "Dark Ages: Ravnos" and "Dark Ages: Tremere" don't. A
  mismatch pairs nothing, and the sync names the two as kept apart.
- **A sync alone never changes the title, authors or ISBN of a book paired on less than its global id** (a
  link that isn't between two files' ids, or an id made of a weak identifier by an older Omabook). Such a
  change is a **rename**: a dry run lists it from either side, and a sync does it only with
  `--accept-renames` (the newer wins, as for the other details) or `--details theirs` / `--details mine`
  (that side's, whatever the times); without, it leaves the book as it is, taking none of its details
  until then (they would be stamped with the other side's time, and its old title seem as new), and says so. Nor does such a book
  take the other's identifiers, which is how the wrong names spread.
- **Links recorded wrongly are dropped.** A link holds when it is between two files' ids, or when the two
  books' titles, as the log said them when the link was made (the last before, else the first after),
  agree. One that fails is recorded as such once (`not_same_as`, which travels and grows like `same_as`,
  and outlives compaction's dropping of the old titles), by the one-off repair of format 43 at upgrade
  (said as the library is brought up to date) and by each sync for what came in.
- **And by the person, on request.** A link an older Omabook made after it had already given one book the
  other's title holds by the rule above (the titles agreed then), and once the title is put right it shows as
  a rename no sync does. `omabook sync <computer> --unlink "<title>"` records each link between a book of
  that title and one whose title now disagrees as `not_same_as` (#160): the title a book has on this computer,
  else the one the log says it has now, which may still be the wrong one after it was put right here (#166); a dry run's
  renames say how. Nothing drops such a link by itself: "the titles disagree now" is also what a book renamed
  on purpose on one computer looks like, and that one stays paired.
- **Identifiers travel only within one book** (#164). A book paired on less than its global id takes none of
  the other's, nor are the copies of it here that such a link folds together given each other's details. An
  identifier of a strong scheme that a book shares with one kept apart from it (`not_same_as`) is stray when it is
  the other's own and not this one's (StrayIdentifiers). A book's own (#166): what its id is made of; its ISBN
  (the one it has here, else the one the log says it has now); and a key first recorded, as the log says, on a
  book whose title then agrees with its title now (the Goodreads id of its entry, its Calibre uuid). Which book
  had a key first does not say whose it is, since an older Omabook copied keys between the books it took for
  one: one that neither has as its own, or both, is kept, and named when the other had it first. A
  sync takes no stray identifier, nor one in doubt, removes the stray ones here (after `--unlink`, before
  anything else of that sync, and once what came in is stored, before it is taken in) and names them; the
  one-off repair of format 44 does the same at upgrade.
- **A new book without a file is given an id of a strong key, else its title and authors**, never one
  another book here has; one received from another computer keeps the id it has there.

## Consequences

- Most books need nothing new: books with files pair by content as before, and books to read from Goodreads
  have the same id on both computers (`e:entry:goodreads=…` or of their title), or pair once by their
  Goodreads id.
- A book without a file that shares only a weak identifier with a book there is paired with nothing. Its
  details then don't travel; it is still not fetched again, as before (the manifest sync's "already held"
  is unchanged), so nothing is duplicated.
- A book held twice on one side (an entry and the book's file of one ISBN) and once on the other is no
  longer folded into one: the ISBN names two books there. The file is paired by its content, the entry with
  nothing.
- An Omabook before 0.2.2 is sent no entry keys to pair by, and its own are not read; its books without a
  file are paired by their id alone. It keeps a `not_same_as` it doesn't know, and passes it on (KeptOps).
- Titles a wrong link already wrote stay as they are: the repair stops further damage, and the person puts a
  wrong title right once, on one computer.
