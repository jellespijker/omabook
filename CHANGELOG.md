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

### Fixed

- A book whose details are the same on two computers is no longer reported as taken at every sync from one of them: the details taken keep their time to the clock's count within a second, as the other computer's log says it, so a cover set in a second the clock ticked in more than once is newer there no more. And both computers compare a book's details as their logs say them, so a book is undecided from both sides or from neither (#106).
- The graph centred on a thing in a large library counts what is around it at once: four steps out took over a second on a library of 1,500 books, and now takes a few hundredths of one (#100).
- An answer from a model that wraps it in HTML-like tags (`<panel>…</panel>`, `<div>`s, a stray `<span>`) is shown without them: in the reading companion, the library's answers, `omabook ask` and the MCP tools that answer questions, whichever AI answers. Markdown stays as it is, and so does a tag written in a code span or a code block. The companion and the librarian are also told to answer in plain text or Markdown (#105).

### Changed

- Syncing a large library is quicker: each sync reads of the change log only what the other computer lacks, and what it says it holds, not all of it each time; and the log another computer brings in when it is paired is compacted in that same sync, of what both have seen, so it no longer sits beside the snapshot that says the same (#103).

## [0.1.7.1] - 2026-11-20

### Fixed

- A book a plugin made and marked to travel with its world (`file: true` from `omabook.sharing.register`'s `worlds`) now carries its file, also when the space's files switch is off: only that book's file, and only of a book the plugin made, never a person's. It goes as the space's book files do, sealed in a shared space's folder or beside a published space's file, checked against its hash, and comes to a member with the space, at their `omabook spaces sync` or as they import the space's file, when their copy of the book has no file of its own. `omabook graph show --scope` and the window's graph show a world shared with you by its layer's name (`<space>/<scope>`), cut where you are in its book, as a plugin reads it (#107).

## [0.1.7] - 2026-11-20

### Added

- As the AI builds a book's knowledge graph, it notes when the events happen, as far as the book says: a date, a year or a span on the real calendar or the story's own, "around" where the book hedges, or only the book's words for it ("three years before the war"); which events come before or after others, as a flashback tells it, with an order it concluded marked as inferred; and the timeline each event is on: the book's own, its series', a world several books share, and real history for real dates, where books with nothing else in common meet. A series' or a world's timeline is shared only by books on the same side of a library split into top collections. Each is revealed where the book says it, so nothing shows before the reader gets there, and what the model gives that is no time or no order is dropped, never kept. `omabook graph build` (and the MCP tool) counts them under `"chronology"`: the times, orders and placings kept, and how many were dropped; `--again` reads them into the graph of a book built before (#42).
- The AI can be a server that speaks the OpenAI-compatible API: **A server (OpenAI-compatible)** in Settings, `ai_provider: "openai"` with `ai_endpoint` in `config.json`. Omarchy's own local models (`omarchy agent local`, the one running here or one paired from the tailnet, with the key Omarchy keeps, read again for every question), Ollama on this computer or another one, OpenAI with `OPENAI_API_KEY` or a key of your own, or any other by its address (OpenRouter, Groq, LM Studio, llama.cpp, vLLM, ...). Answers stream, the system prompt and the conversation go as they are, and a thinking model's working is never shown, also where a server sends it inside the answer between `<think>` tags. Both model rows list the server's models, kept for each server; Ollama's `…:cloud` models are marked *runs elsewhere* and never used unless picked. A refused model is asked again of the main one, a server that can't be reached says so, and Settings says when `http://` sends the text unencrypted beyond this computer and the tailnet. Keys are kept in the keyring, never in `config.json` or the log. Automatic is unchanged: it never switches to a server by itself. `omabook ai use|models|key` does the same on the command line, and `omabook info --json` says which server and whether a key is set (#32).
- `omabook chronology` tells the events of the books you read in the order they happen in their world, not the order you read them in: of every timeline (a book's own, a series', a world's, real history) its events in order, with each time as the book says it (`1942`, `1939-09-01 – 1945-09-02`, `about 3019 (Third Age)`, “long ago”) and where it was said, which timelines line up, the events no time or order is known of yet, and each contradiction with where every statement in it was said and which was set aside. It is cut where you are in each book; `--upto` cuts one book at a section and `--whole` takes everything, read or not. `<book>`, `--series` and `--collection` choose the books, `--timeline` one timeline, `--from` and `--to` a stretch of time (on `--calendar`, or each timeline's own), with how many of the events are shown; `--json` for scripts, and `--format json|csv|graphml|dot [--output file]` for other tools: CSV one row per placed event with its timeline, rank, time as said, extent and where it was said, GraphML and Graphviz with the order as edges. `omabook graph chronology` is the same command. A library kept apart by its top collections answers a part for each. By hand: `omabook chronology set-when <event> <time>`, `order <event> before|after|during|same <other>`, `place <event> on <timeline>` and `timeline add <name> [--kind world]`, each checked and refused with why, and marked as put in by hand, which wins over what the books say. The MCP tools `chronology`, `chronology_set_when`, `chronology_order`, `chronology_place` and `chronology_timeline_add` do the same for an agent (#45).
- The chronology: when the events of the books happen in their stories, not the order they are read in. **T** on the shelf shows the selected book's, **Shift+T** the open book's, and **T** in the graph that of what it shows (a book, a series, a collection or everything read). One lane for each timeline (a book's own, a series', a world's, real history) under the ruler of its calendar, from centuries down to minutes; lanes that line up are on one axis, by date where the books give one and in order where they don't. A moment is a mark, a span a bar, an event during another sits under it, and *you are here* marks the event you came to last. What has no time or order is listed apart, and an event in a contradiction is ringed, its card saying what was said where and which was followed. A tap on an event shows its card, with **Back to the page** it was revealed on and **Show in the graph**. Filters by timeline, book, series, collection, character, place, group or theme, only what was met in a book, and a stretch of time; zoom and pan with the keys, the wheel, a drag or a pinch. Everything is cut where you are in each book; the graphs of collections you subscribe to are laid in, hollow, and a library split into top collections shows each apart (#44).
- The chronology is described in the README, in a help page of its own (`omabook help chronology`, and under `?` in the window), and in the skill for coding agents (#46).
- Bookmarks travel between your own computers: a sync carries each one made, renamed or removed on either, at the same place in the book, and the highlights and bookmarks taken in from Calibre with them. A sync with an Omabook from before 0.1.7 carries none of them, as before, and sends them once that computer is updated. `omabook bookmarks <book> --rename <bookmark> --as <words>` renames one (#66).
- A space can carry its books' files, when its owner allows: books you own you may share with anyone you allow (#63). Off until switched on, by the owner of a published collection (*The books' files* in the publish dialog, `omabook collections share --with files`, `omabook spaces files <space> on|off`) and by any writer of a shared space (`spaces files`, the MCP tool `space_files`); whoever turns it on is told that the files go to everyone the space reaches, and `omabook spaces` says which spaces carry them. The space names each file by its format, hash and size, never where it lies. A file is fetched only when wanted, when its book is opened in the window or with `omabook spaces fetch <book>` (`space_fetch_file`), taken only when it has that hash, and only for a book with no file: a copy of your own stays as it is. A shared space's folder holds the files sealed with its key beside its batches, under names that say nothing of them, written by whoever has each at their sync; a published space's file has them beside it, in `<name>.omabook-files`, never inside it; a gist carries none, and says so.

### Changed

- A long press on a book with a finger lists what can be done with it, as a right click does, beside where it was pressed, in every view of the shelf; its details are one of what is listed. Before, holding a book only opened its details, and the list was reached only from the details' *More* (#9).
### Added

- A book the search found by what is known of it says which thing found it on the shelf too, not only in the details: along the foot of its cover in the covers and the carousel, on the book pulled out of the spines, and after its title in the list (*Mara Venn (Character)*), by the name you know it by at your place in the book. A thing of a graph someone shared carries the mark of a shared graph, and says whose to a screen reader (#10).
### Fixed

- The graph of everything read, in a library with more than the 2,000 things the window draws at once, no longer passes them off as all there is: its header says how many there are, `2,000 of 6,965 things · 8,685 of 17,192 relations`, and a word over the picture says the most connected are shown and that finding a thing, centring on one or picking a book under *Filter* reaches the rest. Centred on a thing, past the 400 nearest it says so the same way. `omabook graph show` and `omabook graph node` (and the MCP tools `graph_scope` and `graph_node`) answer a `"total"` of nodes and edges, and a `"cut"` when not all are answered, which the plain output says on its last line; `omabook graph scopes` counts each scope's things without loading them (#64).
- A plugin's worlds go with a collection you publish or share (#65). A plugin says what of its own a collection takes with `omabook.sharing.register({worlds(context)})`, asked when the collection is published (`omabook collections share`) or a shared space is founded (`omabook spaces create`); of what it answers only a world (a scope of the graph) the plugin made goes, of a book it made, and only with the graph: never a book by a person, never a scope it didn't name, such as a story's plan. What goes is cut at your place in the book. Whoever gets it sees it as a layer, read-only and cut at their own place: `omabook.graph.layers()` lists them, `whole`, `scopes` and `copyScope` know them. `omabook.spaces.add(space, book)` puts a book the plugin made into a shared space you write, with its world, signed as you at the next sync; it throws for a reader. A world's `file: true` is kept with it for the space's book files (#63). The library's format goes up by one for it (a copy is kept first).

### Fixed

- A book whose reading place was set without a percent (`omabook progress <book> --section 3`, or a plugin's book in a format the command hadn't loaded) counts as begun: what you know of it is no longer taken for nothing, in the graph you share and in what a shared graph shows you (#65).
- Adding the very file of a book a subscribed or shared space made a wishlist entry of fills that entry, also when the file's own details differ from the space's (another title, another ISBN), rather than adding the book a second time beside it (#63).

## [0.1.6.1] - 2026-11-19

### Fixed

- `omabook sync --dry-run` between two computers that keep the change log says what a sync would do and keeps nothing on either side: its journal says how many operations *would* be sent and taken, and before the two computers ever synced it no longer reports every operation as refused, which a sync, pairing them first, takes. The details left undecided are counted once per book, not again at the end of a sync that moved books, and only where the words differ: a cover saved apart on each computer, with no time to say which is newer, no longer makes a book undecided, as before 0.1.6; `--details mine` or `theirs` still settles those that differ, cover and all (#59).
- The first command after an update, with a plugin installed, no longer takes two minutes: bringing the library up to date ran the plugins, which opened the library and waited for the lock their own program held, until it gave up and stopped them. A command that only lists books runs no plugin, and a library opened again by the program setting it up is not waited for (#60).
- A copy of a book you removed on one computer, a duplicate of one you kept there, no longer removes the other computer's copy of that book at the first sync after updating to 0.1.6: a book is removed by a sync only when the other computer no longer has it, by its files or, for a book without a file, by its names (an identifier, its ISBN, its title and authors), as before 0.1.6. A computer that lost its copy that way gets the book back at the next sync, with its details and reading, and that removal never travels back to remove the copy that was kept. A book arriving that way is no longer named after its file, without its details (#58).
- Opening a library another program wrote to, or a copy of it put back from a backup, no longer counts as removed the books only another computer had, such as one whose file didn't arrive at the last sync, or one that came after that copy was made; the next sync could remove them there. Only a book this computer had and no longer has is taken as removed; the others are left to the next sync, which brings them (#61).

## [0.1.6] - 2026-11-19

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
- A page on sharing in the help (`?`, `omabook help sharing`), the README and the skill: publishing a collection as a file or a link, subscribing to one, following it, shared spaces, and keeping your own computers one person, with the spaces and subscriptions a sync carries between them; and plainly what is never shared (the books' files; your graph, questions, stars and reading unless you switch each on; anything outside the collection; your computers' names) and how spoilers are kept. *Other computers* in the help says what a sync leaves out as it is now. What's new for the next release says it too (#8).

### Changed

- Each library keeps global ids, a hybrid logical clock and a change log alongside its tables; `omabook sync` uses the clock across computers whose wall clocks differ and merges book details field by field (#35).
- The first start after updating to this version brings a library up to date about three times faster, and it grows far less: the change log starts with the library as it was written as one snapshot, rather than every detail as a signed change of its own, and the snapshot is kept without an index beside it. On a library made of 1,560 copies of the test samples, from 1.9 MB to 8.7 MB instead of 15 MB, in 4.6 s instead of 13.3 s. While it runs the window says "Bringing your library up to date…", where it used to look frozen, and a command says so on standard error. The library's format goes up by one for it (a copy is kept first) (#54).
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

- `omabook spaces --help` prints its usage once, as `omabook collections --help` does, with the ellipsis no longer garbled (#52).
- A plugin installed from a folder or a .zip gets the name its repository on GitHub gives it: `omabook plugin install ~/dev/omabook-storyteller` installs `storyteller`, as `omabook plugin add jellespijker/omabook-storyteller` does, so `omabook plugin run storyteller …` works either way. One installed earlier under the folder's name (`omabook-storyteller`) is the one a new install replaces (with `--replace`, or in the window), keeping what it saved in its `data` folder, so it isn't there twice (#20).
- A book a plugin made, synced to another computer, is that plugin's own there too once the plugin runs: a sync still never carries the plugin's mark, but a book whose every file is in a format that plugin reads (a Storyteller `.story`) is claimed by it when it loads and when the book arrives, so Storyteller can continue, fork and branch a story there. A book in a format Omabook reads itself (an EPUB, a PDF) is never any plugin's, nor one two plugins read (#28).
- A book in a plugin's format (a Storyteller story) syncs to a computer where that plugin isn't installed, and from one, instead of failing with "book.story is not a book format Omabook knows": it is kept there with its world (the story's knowledge graph), its details say "Needs the Storyteller plugin to be read here" with a button that installs it, and opening it says how to install it. The plugins' registry says which formats a listed plugin reads. An Omabook from before this one, which can't keep such a book, is skipped with the reason and the plugin to install there (#25).
- Removing a member from a shared space (`omabook spaces remove`, `space_remove_member`) changes the space's key: the new key is handed, sealed, to each member left, in the space itself, so that one who was away takes it at their next sync and reads on, and the one removed can't open what is written after. What they write past what the remover held of them then is refused and said, even when they date it before their removal. Older keys are kept (readable only by you) so what was written before stays readable, an invite hands over all of them, so does `omabook sync` to your other computers, and when two writers remove someone at the same time every member settles on the same key (#49).
- No `QSqlDatabase requires a QCoreApplication` warning after library commands (#31).
- Opening a library from two places at once (the window and a command, or two commands), the first time or the first time after an update, no longer fails with "the library can't be set up" or "a copy of it couldn't be kept first": one sets it up, brought up to date with one copy kept, and the other waits and opens it when it is ready (#11).
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

[Unreleased]: https://github.com/jellespijker/omabook/compare/v0.1.7.1...HEAD
[0.1.7.1]: https://github.com/jellespijker/omabook/compare/v0.1.7...v0.1.7.1
[0.1.7]: https://github.com/jellespijker/omabook/compare/v0.1.6.1...v0.1.7
[0.1.6.1]: https://github.com/jellespijker/omabook/compare/v0.1.6...v0.1.6.1
[0.1.6]: https://github.com/jellespijker/omabook/compare/v0.1.5.2...v0.1.6
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
