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

- `omabook identity` prints your id and its fingerprint: the public half of a key pair made the first time it is asked for and kept beside the library, readable only by you, which will sign what you write in a space shared with others (#47).

### Changed

- Each library keeps global ids, a hybrid logical clock and a change log alongside its tables; `omabook sync` uses the clock across computers whose wall clocks differ and merges book details field by field (#35).
- Between two computers with this Omabook, `omabook sync` carries reading places through the change log: the first sync makes their personal spaces one, only the places that changed travel, and the manifest leaves them out. With an older Omabook the sync works as before. A place that arrives this way is no longer counted as a day's reading on the computer it arrives at (#36).
- Between two computers with this Omabook, `omabook sync` also carries books' details and what is said of their reading (shelf, stars, finished day, paper copy, names elsewhere) through the change log: each computer takes what is newer on the other itself, only a cover is still given as a file, and the manifest leaves the details out. Details from before their times were kept stay undecided, as before, until `--details mine` or `--details theirs` (#37).
- The collections go through the change log too between two computers with this Omabook, merged field by field: a collection renamed or moved on one computer is the same one on the other, not a second (#37).
- A book's removal goes through the change log too between two computers with this Omabook: the other computer removes it itself, by the same rule as before (all of it removed, and unchanged there since), and keeps the time it was removed; the manifest leaves removals out (#37).
- The knowledge graph goes through the change log too between two computers with this Omabook, so the manifest a sync asks for now names only each book and its files. A thing known by another name on the other computer is matched once, by the rules the graph sync always used, and stays matched (#37).
- The library's format goes up by one (a copy is kept first, as for every format change): the change log gains a snapshot per space, so a sync reads the operations that still matter instead of all of them, and those it no longer needs, that every computer of the space has seen, are dropped (#37).

### Fixed

- No `QSqlDatabase requires a QCoreApplication` warning after library commands (#31).
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
