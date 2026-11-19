# Looking after the library

Part of the Omabook skill (`omabook skill library`). Ask the user before anything that changes many books at once, and show them a list of proposals first.

## Details of a book

```bash
omabook set 12 --identifier goodreads=4671 --identifier isbn=   # what it is called elsewhere; "isbn=" forgets one
omabook lookup 12 --json           # candidates from every metadata plugin; each has a "candidate" number, a "candidate_id", and what it "adds" and "changes"
omabook lookup 12 --ai --json      # ...and the AI's judgement of them first, with its "why"
omabook lookup 12 --apply apple-books-1a2b3c4d   # take that candidate's details and cover, by its candidate_id
omabook lookup 12 --ai --apply 1   # or by its number in the last listing, with the same flags (refused otherwise)
```

## Tidying, suggestions, questions about the whole library

```bash
omabook tidy --json            # proposals: the same book twice, authors spelled several ways; --ai adds series
omabook tidy --ai --apply      # make them all but the duplicates with "confidence": "low"; show the user the list first and ask
omabook tidy --pick 3,7        # only these, by number (an unsure duplicate is merged only like this)
omabook ask all "Which of my books are about the sea?"   # a question about the library as a whole
omabook suggest --json         # what to read next: each suggestion looked up on Goodreads/Open Library (state, rating, in_library)
omabook catalog --format csv > books.csv  # the library as a table; --format bibtex for references; takes a search
```

## Collections

The user's own named parts of the library, one inside another if they wish (`Studie/Python`). A book can be in several; nothing is copied or moved. Never assume names: read them with `collections`, and propose new ones in the user's language and style.

```bash
omabook collections --json                     # the tree: path, parent, rule, count, books put in/out by hand
omabook collections add "Studie/Python" --rule "tag:python OR tag:programming"   # makes Studie too if needed
omabook collections include Studie/Python 12 14  # by hand, whatever the rule; exclude takes out, follow gives it back to the rule
omabook collections rename Studie Werk         # a path moves it; what is inside goes along
omabook list --collection Studie/Python        # also: ask all --collection, suggest --collection
omabook collections sort --json                # AI proposals, numbered, and "to_decide": books it can't place
omabook collections sort --pick 1,4=Werk/Regeltechniek   # from that same list, not asked again; N=path renames
omabook collections sort Werk --json           # within one: collections inside Werk for its books in none of them; they stay in Werk
omabook collections share Werk --with-graph --dry-run --json   # what would go in (counts) and stay out; writes nothing
omabook collections share Werk --with ratings --as "Ada" --output Werk.omabook-space   # publish it as a space, write its snapshot
omabook collections share Werk --gist          # send it to a secret gist through gh and print its link; again: the same gist
omabook collections import https://gist.github.com/ada/<id>   # subscribe by link (a gist, or a GitHub file); it is followed from there
omabook collections import --follow --json     # what a newer version would change, counted; taken in only with --yes
```

Sharing never sends the books' files; the graph, questions, ratings and progress go only when the user switches them on, and of the graph only what they have read. Show the dry run to the user and ask which parts and what name before sharing. A gist is secret unless the user asks for `--public`. Show what following would change and ask before `--yes`.
omabook spaces --json                                   # the spaces the user is in: kind, their role, members
omabook spaces create "Book club" --collection Club     # a shared space several people write, of a collection
omabook spaces invite "Book club" <person-id> --role reader --out invite.omabook-invite
omabook spaces join invite.omabook-invite               # on the invitee's computer
omabook spaces sync "Book club" --folder ~/Sync/club    # through a folder Syncthing or a share keeps in step
A person's id is what `omabook identity --export` prints. Writers change a shared space by changing their collection of it, then sync.

A rule is a search (`tag:`, `genre:fiction`, `-word`, `OR`, `"quotes"`); books put in or out by hand win. Show the user the sort proposals and the to-decide list before picking anything; `--apply` takes them all.

## Calibre

```bash
omabook add ~/"Calibre Library"    # a Calibre library comes with Calibre's details, covers, all formats and what was read
omabook calibre libraries --json   # the Calibre libraries found on this computer: path, books, answer (imported, later, never)
omabook calibre import             # import the ones not imported yet (or <path>, or --all); again brings the reading up to date
omabook calibre plugins --json     # Calibre's own plugins of every kind: type, usable by Omabook or why not
```

## Reading sites

```bash
omabook goodreads import <csv> # bring in a Goodreads export file: shelves, stars, dates; adds paper/wishlist books, removes nothing
omabook goodreads sync         # the same from a public profile's shelves (goodreads_user in config.json, or --user)
omabook goodreads shelves --json   # what the profile lists, changing nothing
omabook goodreads export <csv> # the library's reading as a file for goodreads.com/review/import
omabook openlibrary sync --user <name>   # a public Open Library reading log
omabook sites --json           # Goodreads, Open Library, Hardcover: state (none/connected/attention), as whom, last sync; never a token
omabook sites sync --dry-run   # what a sync of every connected site would change here and send there; changes nothing
omabook sites sync hardcover   # bring shelves in; sends changes only if the user turned that on (first write needs --send)
omabook sites disconnect <site>   # forgets the user there and the token; books stay
```

Connecting is the user's to do (`omabook sites connect <site>`, token on standard input with `--token-stdin`): never ask for or handle their token.

## Reading place, page layout and other computers

```bash
omabook progress 12 --json                       # section, fraction, percent, when
omabook progress 12 --section 4 --fraction 0.2   # set it
omabook format --json                            # how every book is set: font, size, line width, spacing, indent, margins, alignment, with their bounds
omabook format 12 --align justify --line-height 170  # one book's own, over the defaults; without the id, the defaults
omabook format 12 --reset                        # the book follows the defaults again
omabook format --layout scroll                   # every book one column that runs on through the chapters; pages: turned pages; auto: pages where two fit
omabook format 12 --layout pages                 # this book as pages: each chapter starts on a page of its own, a PDF page shown whole
omabook format --first-page paired               # two PDF pages side by side as 1|2 instead of a book's 1, 2|3 (alone; auto by the first page's shape)
omabook sync laptop                              # exchange books and reading places with another computer over ssh
omabook sync laptop:Books/Fiction                # ...with a named library folder there
omabook sync laptop --dry-run                    # what a sync would receive, send, update and remove on each side; changes nothing
omabook sync laptop --details mine               # where details differ and no time says which are newer: keep this side's (or theirs)
```

The layout and the first page are reading-format options like the rest: a book's own lies over its collection's, which lies over the defaults, and the reading place (section and how far through it) is kept when the layout changes. In the window `Shift+L` goes round auto, pages and scroll.

`sync` needs Omabook installed on both computers and ssh access (a Tailscale name works as the computer). Each side gets the books it lacks, books without a file included (matched by identifiers, else title and authors), with their shelf, stars and finished day, and the newer reading place and the newer details win; a book removed on one side is removed on the other too (to the trash) unless it was read or changed there since. Run `--dry-run` first and show the user what would be removed. Books from before details had a time are counted when their details differ, and only `--details mine|theirs` settles them: ask the user which side is right.

## Plugins

```bash
omabook plugins --json                    # every plugin: kind, origin (built-in, calibre, folder), fields it brings, enabled
omabook plugins disable builtin:Crossref  # switch a metadata source off (or enable)
omabook plugins disable calibre           # everything Calibre at once: its libraries, plugins and converter
omabook plugin install ./plugin.zip --json  # install someone else's plugin (a folder or a .zip): checked, and off until enabled
omabook plugin add owner/repo --json      # install a plugin from a public GitHub repository (or a link to a release, tag, branch or folder); off until enabled
omabook plugin sources --json             # where each plugin came from, what it follows, what is installed, whether an update waits
omabook plugin update --all               # install what is newer; one that asks for a new permission waits for `update <name> --yes` after the user agreed
omabook plugin remove reading-log         # move one of the user's own plugins to the trash; ask the user first
```

To write a plugin, read the plugins part instead: `omabook skill plugins`.

## How Omabook looks and works

The interface is files in `~/.config/omabook/`, followed live: `config.json` (settings), `keys.json` (any key), `style.json` (the theme, colors, corners, font; `omabook theme list` and `omabook theme use <name>` pick the theme), `ui/*.qml` (replace a view), `mods/<name>/` (add a panel). Run `omabook customize` to write the guide `~/.config/omabook/AGENTS.md`, and read it before editing: it lists every command, setting and color, and the objects a view can use. `omabook customize ReaderView` copies a built-in view into `ui/` to edit. After an edit, check for `keys.error`, `style.error` or `ui.error` in that folder; one appears when a file couldn't be used. `omabook change "make it warmer"` has the AI rewrite those files; `omabook change --undo` puts them back.
