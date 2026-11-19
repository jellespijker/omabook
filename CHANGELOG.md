# Changelog

What each release of Omabook brought. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
`project(omabook VERSION ...)` in `CMakeLists.txt`. What a person sees in the
window is also in the app's own *What's new* (`src/whatsnew.cpp`); keep the two
in step.

Every change that someone using Omabook would notice gets a line under
*Unreleased* in the commit that makes it, with its issue number. A release
moves those lines under its version.

## [Unreleased]

### Added

- `omabook identity` prints your id and its fingerprint: the public half of a key pair made the first time it is asked for and kept in your Omabook data folder (`~/.local/share/omabook/person.key`), readable only by you, which signs what you write (#47, #48). A key an earlier build kept beside the library is moved there once, so a library folder synced elsewhere no longer carries it (#48).
- `omabook identity` also lists your computers paired into your personal space (#48).
- `omabook collections import <file> [--as <name>] [--dry-run]` (and the MCP tool `collection_import`) subscribes to a collection someone shared as an `.omabook-space` file: the shared space is kept read-only beside your library, never merged into it. Its books you have are matched (the same file, an identifier or ISBN, or title and authors, as the tidy finds them), the others become wishlist entries, in a collection of yours of its name or the one you give, with a collection inside it for each of the shared ones; one of that name already there is added to, never emptied. A newer file of the same space brings only what is new, the same file twice changes nothing. Its knowledge graph stays in the space, apart from yours. `omabook collections unsubscribe <name>` (`collection_unsubscribe`) drops the space and keeps your collection. The library's format goes up by one for it (a copy is kept first) (#4).
- Share a collection by link (#7): `omabook collections share <path> --gist` sends its snapshot to a gist through GitHub's command line, `gh`, and prints the link; the gist is secret unless you add `--public`, and sharing again updates the same gist, so the link stays. `omabook collections import <link>` takes a gist's link or id, or a file of a GitHub repository (its page or raw link), and follows it: `omabook collections import --follow [<name>]` checks it for a newer version and shows what it would change (books, collections and the graph's things, facts and relations, new, changed and gone), taking it in only with `--yes`, and then only the operations you don't have yet. Omabook keeps no GitHub account or token of its own: `gh` sends, and reading asks GitHub as plugin installs do. The MCP tools `collection_share` (`gist`, `public`) and `collection_import` (a link, `follow`, `yes`) do the same. The library's format goes up by one for it (a copy is kept first).
- `omabook collections share <path>` (and the MCP tool `collection_share`) publishes a collection as a space others can follow, and with `--output <file>` writes its snapshot, an `.omabook-space` file to give them. It first shows what goes in, counted by kind, and what stays out: the books' files always, and your graph (`--with-graph`), questions, stars and reading (`--with questions,ratings,progress`) unless you switch them on; `--dry-run` shows only that. Of the graph only what you have read goes, nothing past your place in a book, and what you read later follows. The space follows the collection: renamed, books in or out, details changed, the next snapshot says so, and its operations keep their numbers. Switching a part off later takes it back: the next snapshot holds none of it, and those following the space are told to drop it, as they are when the collection is deleted. The library's format goes up by one for it (a copy is kept first) (#3).
- A collection you subscribed to shows its knowledge graph beside yours: in the graph's window and its cards, as marks on the page, in a search by what is known (`knows:`), in `omabook graph show` and the MCP server, and in what the AI is told. Always only as far as you have read each book yourself: nothing of a book you haven't begun or only wish for, more as you read on, and never more for a place asked for further on. What comes from it says whose it is ("From Ada's shared graph"), and none of it is written into your own graph; unsubscribing takes it away (#5).
- `omabook spaces` lists the spaces you are in, with their kind, your role and their members. Shared spaces, several people writing one collection (a household library, a book club): `omabook spaces create <name>` founds one of a collection of yours, `spaces invite <space> <person-id> [--role reader|writer]` makes someone a member and writes an invite only they can open, `spaces join <invite-file>` joins, and `spaces sync <space> --folder <dir>` brings it in step through a folder something else syncs (Syncthing, a mounted share), which holds only sealed batches. Writers change the space by changing their collection of it; readers get every change and can't write; whatever isn't signed by someone who was a writer at its time is refused and said; `spaces remove` removes a member. The space never changes your own books, only your collection of it. MCP tools `spaces_list`, `space_create`, `space_invite`, `space_join`, `space_sync` and `space_remove_member`. The library's format goes up by one for it (a copy is kept first) (#38).
- Publishing and subscribing in the window: **Shift+P**, the ⋯ menu, the list of collections (**C**) and a collection's own dialog publish a collection, saying first what goes in (counted as `collections share` counts it), what never does (the books' files) and each part of yours with its switch, all off until switched on, and asking the name you go by the first time and where to save the `.omabook-space` file, or that it goes as a link too (a gist, secret unless you say so, its link copied for pasting). **Shift+O** or the ⋯ menu picks such a file to subscribe to, one dropped on the window is offered too, and a link can be pasted: before anything is written it says which of its books you have, which become wishlist entries, the collection they go in (its name yours to change) and that its graph stays a layer of its own. A collection a shared space fills is marked as someone else's on its tab and in the list, and its dialog unsubscribes from it, keeping the collection; one followed by its link looks there for updates and says what changed, taking it in only when you say so. "From Ada's shared graph" is translated now, and unsubscribing by a command or an agent while the window runs takes the shared graph out of its search at once (#6).
- `omabook sync` between your own paired computers now carries the shared spaces you are in and the collections you subscribed to: join a space or subscribe on one computer, and after the next sync your other computers are in it too, with the space's key (kept readable only by you), your collection of it, and where a subscription is followed from. What arrives is checked as anything from elsewhere is (a shared space's writes by its members' roles and signatures), and a subscription's graph is shown as far as you have read on each computer. Unsubscribing on one computer unsubscribes the others at their next sync. None of it goes to a computer that isn't you: it is sealed to your key and signed with it, and an Omabook from before gets none of it (#51).

### Changed

- Each library keeps global ids, a hybrid logical clock and a change log alongside its tables; `omabook sync` uses the clock across computers whose wall clocks differ and merges book details field by field (#35).
- Between two computers with this Omabook, `omabook sync` carries reading places through the change log: the first sync makes their personal spaces one, only the places that changed travel, and the manifest leaves them out. With an older Omabook the sync works as before. A place that arrives this way is no longer counted as a day's reading on the computer it arrives at (#36).
- Between two computers with this Omabook, `omabook sync` also carries books' details and what is said of their reading (shelf, stars, finished day, paper copy, names elsewhere) through the change log: each computer takes what is newer on the other itself, only a cover is still given as a file, and the manifest leaves the details out. Details from before their times were kept stay undecided, as before, until `--details mine` or `--details theirs` (#37).
- The collections go through the change log too between two computers with this Omabook, merged field by field: a collection renamed or moved on one computer is the same one on the other, not a second (#37).
- A book's removal goes through the change log too between two computers with this Omabook: the other computer removes it itself, by the same rule as before (all of it removed, and unchanged there since), and keeps the time it was removed; the manifest leaves removals out (#37).
- The knowledge graph goes through the change log too between two computers with this Omabook, so the manifest a sync asks for now names only each book and its files. A thing known by another name on the other computer is matched once, by the rules the graph sync always used, and stays matched (#37).
- Every change written to the library's change log is signed by you, and a sync between two computers with this Omabook carries the signatures and checks each before taking anything in: a change that is not as it was signed is refused, never applied, and the sync says so. Changes from an older Omabook, which are unsigned, are taken only from your own computers. The library's format goes up by one for it (a copy is kept first) (#48).
- The first sync between two computers with this Omabook pairs them into one personal space with one person: the computer whose id is not kept takes on the other's key, over the same ssh the sync runs on, so all your computers sign as you (#48).
- The library's format goes up by one (a copy is kept first, as for every format change): the change log gains a snapshot per space, so a sync reads the operations that still matter instead of all of them, and those it no longer needs, that every computer of the space has seen, are dropped (#37).

- Compacting the change log now makes the library smaller: the snapshot of a space takes the place of the operations it covers instead of keeping a copy beside them, so of what every computer of the space has seen only what came after the snapshot stays. The library's format goes up by one for it (a copy is kept first) (#2).

### Fixed

- No `QSqlDatabase requires a QCoreApplication` warning after library commands (#31).
- A computer that a sync paired into another person keeps what it was in spaces as the person it was: that person's key is kept (`former-persons/` in your Omabook data folder, readable only by you), so an invite sealed to it still opens; in a shared space where it was a writer it makes you, as you are now, a writer, and goes on writing; where it was a reader it goes on reading; and a collection it shared stays the same space, now yours as you are, so those who follow it take what is new (#50).
- `omabook sync`, the window's sync status and the sync log now report knowledge graph changes taken and sent (#31).
- Knowledge graph sync now settles in a single sync when two libraries call the same thing by different names, without merging separate local things that share another name (#31).

## [0.1.5.2] - 2026-11-16

### Fixed

- A sync carries the knowledge graph: what was noted while reading a book on
  one computer is on the other too, merged with what is known there, each
  fact at the moment it was revealed (#30).

### Added

- A harness that checks every change the same way, whoever makes it: modern
  C++23 (clang-tidy, ast-grep rules), the layers of `src/`, file-size and
  function-complexity budgets, formatting, secret scanning
  (Talisman) and an issue number in every commit message. It runs in the
  agents' edit hooks, on commit, and in CI (#29).

## [0.1.5.1] - 2026-11-16

### Changed

- `omabook plugins enable` and `disable` take a plugin's plain name, and say
  what a name could mean when it is unclear (#27).

### Fixed

- A plugin's mark stays out of a sync, so a story's names arrive and the
  sync settles.

## [0.1.5] - 2026-11-16

### Added

- Pages or scroll: a book is read as pages, each chapter starting on a page of
  its own and a PDF page shown whole, or as one column that runs on from
  chapter to chapter. A turn never shows a line twice.

## [0.1.4.3] - 2026-11-15

### Added

- Sort within one collection: `omabook collections sort <name>`.

### Fixed

- A book is read again when its file moved while it was being reread.

## [0.1.4.2] - 2026-11-15

### Fixed

- Sync takes books without a file (paper books, wishes, reading-site shelves),
  and every book's shelf and stars.
- The README downloads the package before installing it, as pacman wants a
  signature for a link.

## [0.1.4.1] - 2026-11-15

### Fixed

- The page-width tests measure in characters of the reading font, whatever
  fonts the machine has.

## [0.1.4] - 2026-11-15

### Added

- One, two or three pages side by side, with lines as long as read well (66
  characters in your font at your size).
- Back to the moment, without spoilers: the map says when each thing was
  revealed and takes you to that line.
- Your library as a folder in Folio, the file manager.
- Write a story from the shelf, with the Storyteller plugin.
- Collections: named parts of one library, as tabs above the shelf.
- Search what you know, or ask the AI to find books for a question.
- Plugins to try: a list to install from with one click, each checked first.
- Every key a click or a tap away: the ⋯ menus and a book's right-click menu.

## [0.1.3] - 2026-11-15

### Added

- The Aa panel beside the page: font, size, line width, spacing, indent,
  margins and alignment, for every book or only this one.

## [0.1.2] - 2026-11-15

### Added

- Omarchy's themes everywhere, followed as they change, and a theme chooser.
- The graph in the text: the people, places and things the map knows are
  marked on the page.

## [0.1.1]

Not tagged on its own; it shipped in 0.1.2.

### Added

- A knowledge graph for books already read, with what it will cost first.
- Plugins from GitHub that keep themselves up to date.
- Panes as wide as you like.

## [0.1.0] - 2026-11-14

The first release: five views of the shelf, filtering by typing, books on
paper and wishes, a reading companion that can't spoil, tidying the library,
suggestions of what to read next, changing Omabook by asking, plugins and
scripts, sync with other computers, a knowledge graph of what you read,
reading sites (Goodreads, Open Library, Hardcover), and help you can search.

[Unreleased]: https://github.com/jellespijker/omabook/compare/v0.1.5.2...HEAD
[0.1.5.2]: https://github.com/jellespijker/omabook/compare/v0.1.5.1...v0.1.5.2
[0.1.5.1]: https://github.com/jellespijker/omabook/compare/v0.1.5...v0.1.5.1
[0.1.5]: https://github.com/jellespijker/omabook/compare/v0.1.4.3...v0.1.5
[0.1.4.3]: https://github.com/jellespijker/omabook/compare/v0.1.4.2...v0.1.4.3
[0.1.4.2]: https://github.com/jellespijker/omabook/compare/v0.1.4.1...v0.1.4.2
[0.1.4.1]: https://github.com/jellespijker/omabook/compare/v0.1.4...v0.1.4.1
[0.1.4]: https://github.com/jellespijker/omabook/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/jellespijker/omabook/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/jellespijker/omabook/compare/v0.1.0...v0.1.2
[0.1.0]: https://github.com/jellespijker/omabook/releases/tag/v0.1.0
