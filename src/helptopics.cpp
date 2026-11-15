#include "manual.h"

// The help's pages. This is the one place their text lives: the window (?)
// and `omabook help <topic>` both show it from here.
//
// A command is named in braces, {library.add}, and shown as whichever key
// it has now; never write a key itself. What is in `backticks` is something
// to type in a terminal. A page lists, after its text, the commands it
// named, and in the window those can be run from there. Keep a page to
// what a person needs: the README is where the rest goes.
QList<Manual::Topic> Manual::topicList() {
    return {
        {"start", tr("Getting started"), tr("Add your books, find one, and read it."),
         {
             {tr("Add books"),
              tr("{library.add} adds book files and {library.add_folder} a folder of them; you can also drop files on the "
                 "window. Books are copied into the library folder, and the file you added is never touched. {library.new} "
                 "adds a book that has no file: one on your shelf at home, or one you want to read some day.")},
             {tr("Coming from Calibre"),
              tr("Omabook finds your Calibre libraries and offers to import them, with their details, covers and what you "
                 "have read. {library.libraries} lists them at any time. Calibre's own folder is only read.")},
             {tr("Find and read"),
              tr("{library.search} searches as you type. The arrow keys, or {library.left} {library.down} {library.up} "
                 "{library.right}, move around the shelf, and {library.read} opens the selected book where you left off.")},
             {tr("Finding your way"),
              tr("{global.help} opens this help anywhere. Type what you want to do, and Enter runs it. The tour is a short "
                 "walk through the window, and it is always here.")},
         }},
        {"reading", tr("Reading"), tr("Turn pages, find your place, and make the page comfortable."),
         {
             {tr("Turning pages"),
              tr("{reader.page_down} turns the page and carries on into the next chapter; {reader.page_up} goes back. "
                 "{reader.next} and {reader.previous} jump a chapter, {reader.line_down} and {reader.line_up} scroll a few "
                 "lines. Where the window has room, two or three pages stand side by side and a turn moves on by all of "
                 "them; otherwise one page scrolls.")},
             {tr("How many pages"),
              tr("A line is as long as reads well: 66 characters unless you set it otherwise, in the font and size you "
                 "read in. As many pages stand side by side as each keep most of that, up to three, and none with lines "
                 "shorter than 45 characters; a single page doesn't grow past it, the rest is margin. {reader.pages} "
                 "goes from as many as fit to one, two or three pages and round again; a choice that doesn't fit shows "
                 "the most that do. A pane beside the page leaves less room, as a narrower window does.")},
             {tr("Finding your place"),
              tr("{reader.contents} shows the chapters beside the page, and {reader.find} finds words in the book. "
                 "{reader.bookmark} keeps a bookmark. Your place is remembered when you leave with {reader.back}.")},
             {tr("The graph in the text"),
              tr("With the map beside the page ({reader.graph}), the people, places and things it knows so far are marked "
                 "in the text in the colors they have in the map; {reader.marks} marks them always, or never. Point at "
                 "one, or tap it, for what is known of it. Select words (drag, double-click, press and hold, or "
                 "{reader.select} and the arrow keys) for a menu: ask about them, who or what they are, show them in the "
                 "map, find them in the book, or a question worth asking.")},
             {tr("Moments, and no spoilers"),
              tr("The map's details say when each thing known of someone was revealed, and each is a way back to that "
                 "moment in the text ({graph.moments} from the keyboard), where its words are lit. The card on the page goes to where a thing was first met, "
                 "and to where what is said of it now was told. What happens past the furthest you have read stays "
                 "closed under Later in the book: it opens for that one thing only, once you confirm, until Omabook "
                 "closes. A book marked as read has nothing to hide.")},
             {tr("Making it comfortable"),
              tr("{reader.larger} and {reader.smaller} change the size of the type. {reader.expand} shows the book and "
                 "nothing else, on the whole screen.")},
             {tr("Setting the text your way"),
              tr("{reader.format} opens the Aa panel beside the page (Aa in the bar, with fingers): the font, its size, "
                 "the longest line in characters, the pages side by side, the space between lines and between paragraphs, a first-line indent, the margins, "
                 "and the alignment: start, justified (filled out to both sides), centred or end. Each change shows at "
                 "once and the page stays where you were. With Only this book on, the change is for this book alone; "
                 "Reset puts back what there was when the panel opened. The defaults for every book are in the "
                 "settings ({global.settings}) too. Pages that are pictures, as in a PDF or a comic, can't be set.")},
             {tr("With fingers"),
              tr("Swipe sideways or tap the left or right edge to turn the page, pinch to resize, and tap the middle of "
                 "the page for buttons to everything else.")},
             {tr("What it reads"),
              tr("EPUB, FictionBook, Markdown, HTML and plain text flow in your own type. PDF and comics are shown page by "
                 "page. Kindle, Word and the other formats Calibre knows are converted the first time you open them, "
                 "which needs Calibre installed.")},
         }},
        {"ai", tr("The AI companion"), tr("It reads along with you, and looks after the library when asked."),
         {
             {tr("While you read"),
              tr("{reader.ask} opens the companion beside the page. Ask about the chapter in front of you, or select a "
                 "passage first and ask about that. It is sent the book only as far as you have read, so it can't spoil "
                 "the ending.")},
             {tr("Coming back to a book"),
              tr("{reader.catch_up} tells you where things stand, in a few lines. {reader.cast} lists the people and "
                 "places that matter so far. Both keep notes with the book, so a later book in a series builds on the "
                 "earlier ones.")},
             {tr("For the whole library"),
              tr("{library.ask} asks about your library: “which of these are about the sea?”. {library.suggest} suggests "
                 "what to read next. {library.tidy} looks for duplicates, authors spelled several ways and missing "
                 "series, and changes nothing until you say so. {library.lookup} looks a book's details up and has the "
                 "AI weigh what the catalogs found.")},
             {tr("Changing Omabook itself"),
              tr("{global.change} takes a wish in words, such as “make it warmer” or “bigger covers”, and rewrites the "
                 "interface's files for you. One click puts back what the last change replaced.")},
             {tr("Which model, and what is sent"),
              tr("The settings ({global.settings}) pick the model: Omarchy's default agent, Claude with a key, any "
                 "command such as a local model, or none. Nothing is sent until you ask; then it is your question, the "
                 "book's title and authors, and the part of the text you asked about.")},
         }},
        {"library", tr("Your library"), tr("Views, order, filters, collections, details, and books on paper."),
         {
             {tr("Five views"),
              tr("{library.view_covers} to {library.view_stacks} show the shelf as covers, spines, a carousel, a list or "
                 "stacks, and {library.view_next} goes to the next. In stacks, {library.group} piles by series, author, "
                 "tag or reading status.")},
             {tr("Order and filter"),
              tr("{library.sort} changes the order and {library.sort_reverse} turns it round. {library.filter} filters: "
                 "type a few letters of a status, a format, a language, a tag, an author, a series or a rating, and Enter "
                 "takes the match. Filters add up and show as chips; {library.filter_clear} takes them all off.")},
             {tr("Collections"),
              tr("Parts of the library of your own, named as you like, one inside another if you wish: a book can be in "
                 "several, and nothing is copied or moved. {library.collections} lists them, to show one, make one or "
                 "change one, and {library.collection_next} and {library.collection_previous} step through them as tabs "
                 "above the shelf. Each is filled by a rule, a search, and by the books you put in or take out with "
                 "{library.collection_book}, which win over the rule. Each keeps its own view, order and reading format, "
                 "and the AI can propose where books belong, as changes to tick in the tidy ({library.tidy}).")},
             {tr("Search"),
              tr("{library.search} looks everywhere. Start a word with `author:`, `title:`, `series:`, `tag:`, `genre:`, "
                 "`format:`, `lang:`, `publisher:` or `about:` to look in one field, and type `is:paper` or `is:wishlist` "
                 "for the books that have no file. \"Quotes\" keep words together, -word leaves out, and OR joins "
                 "alternatives.")},
             {tr("Details"),
              tr("{library.edit} edits a book's details, your stars and whether you have read it. {library.lookup} looks "
                 "the details up online. {library.details} shows or hides them beside the shelf.")},
             {tr("Paper books and wishes"),
              tr("{library.new} adds a book without a file. Tick “On my shelf” for one you own on paper, and leave it off "
                 "for one you only want to read. Both are ordered, filtered and counted like any other book.")},
             {tr("Keeping and removing"),
              tr("{library.export} saves a copy of a book elsewhere. {library.remove} removes it, after asking; its files "
                 "go to the trash. {library.libraries} opens another library: each is a folder of its own.")},
             {tr("In the file manager"),
              tr("Folio, Omarchy's file manager, lists your libraries in its sidebar. Each opens as a folder with a file "
                 "per book: open one to read it in Omabook, copy a book file in to add it, delete one to send its book "
                 "to the trash, where Undo brings it back. The settings ({global.settings}) switch this off, and can "
                 "make Omabook the app that opens ebooks.")},
         }},
        {"plugins", tr("Plugins"), tr("What Omabook is made of, and how to add to it."),
         {
             {tr("What is there"),
              tr("{library.plugins} lists what Omabook is made of and what was added to it: sources of book details, "
                 "formats, importers, and your own. Each can be switched off.")},
             {tr("Sources of details"),
              tr("Open Library, Google Books, Apple Books, Crossref and Goodreads are built in. With Calibre installed, "
                 "its metadata plugins are asked as well.")},
             {tr("Your own"),
              tr("A plugin is a folder in `~/.config/omabook/plugins/`. A script plugin, written in JavaScript, can add "
                 "sources of details, commands with keys of their own, panels in the window, and hooks on what happens "
                 "to a book; its commands appear in this help under its name, and in the lists the window's ⋯ opens. "
                 "`omabook plugin new <name>` starts one and "
                 "`omabook plugin check <name>` tests it.")},
         }},
        {"calibre", tr("Calibre and Goodreads"), tr("Bring in what you already have, and keep it in step."),
         {
             {tr("Importing from Calibre"),
              tr("A Calibre library found on this computer is offered once, at the bottom of the shelf. "
                 "{library.libraries} lists them all and imports one whenever you like, and {library.add_folder} takes "
                 "any Calibre library folder. Details, covers, every format, your stars and how far you read come "
                 "along. Importing again brings the library up to date without adding anything twice.")},
             {tr("Calibre as a helper"),
              tr("With Calibre installed, Omabook uses its converter for the formats it doesn't read itself, and its "
                 "metadata plugins for look-ups. All of that is one plugin, which {library.plugins} can switch off.")},
             {tr("Reading sites"),
              tr("{library.sites} lists Goodreads, Open Library and Hardcover, each not connected, connected as "
                 "somebody, or needing attention. Paste a link to your profile, a shelf, or your user name: Omabook "
                 "looks at once and says what it found, or exactly why not. Sync brings your shelves in; books you own "
                 "on paper become paper books, and the rest books you know of.")},
             {tr("Goodreads"),
              tr("Goodreads has no way in for apps and shows shelves to few who aren't signed in. The row opens to "
                 "three steps instead: open its export page, download the file, drop it on the card. Omabook watches "
                 "your Downloads folder meanwhile and offers the file when it arrives. The way back is a file too, "
                 "which you upload there.")},
             {tr("Signing in, and writing back"),
              tr("Open Library takes the two keys of your Internet Archive account, and Hardcover a token you make in "
                 "your account there; each row has a button to the page. They are kept in the desktop's keyring, never "
                 "in a file. With \"Also update\" on, status, stars and finished dates you change here go to the site: "
                 "the first time you are shown what would be sent. What changed on both sides stays as it is here, and "
                 "the sync says so.")},
             {tr("Nothing is lost"),
              tr("A sync with a reading site never removes a book, here or there, and what you changed in Omabook since "
                 "the last one stays as you left it. Disconnecting forgets who you are on the site and its token, and "
                 "leaves every book.")},
         }},
        {"computers", tr("Other computers"), tr("The same library on every computer you read at."),
         {
             {tr("Sync"),
              tr("Name the other computer in the settings ({global.settings}) and {library.sync} brings the two libraries "
                 "in step over ssh. Each side gets the books it lacks, and for a book both have, the later reading place "
                 "wins. `omabook sync laptop` does the same from a terminal.")},
             {tr("What it needs"),
              tr("Omabook on both computers, and ssh that logs in without asking for a password. Tailscale makes that "
                 "work from anywhere.")},
             {tr("Why not a shared folder"),
              tr("A library is a live database, and two computers writing it through a network folder can corrupt it. So "
                 "each computer keeps a whole library of its own, readable offline, and a sync brings them in step.")},
             {tr("What a sync leaves out"),
              tr("Bookmarks and the AI's notes stay on the computer they were made on.")},
         }},
        {"themes", tr("Colors and themes"), tr("Look like Omarchy anywhere, follow Aether, or pick a theme."),
         {
             {tr("Automatic"),
              tr("Omabook takes its colors from Omarchy's current theme and follows it as it changes. Without Omarchy it "
                 "follows the theme Aether writes, and without either it looks like Omarchy's default, Tokyo Night.")},
             {tr("Choosing a theme"),
              tr("In the settings ({global.settings}), Choose Theme lists Omarchy's own themes, built into Omabook, with "
                 "Omarchy and Aether when they are here. The arrow keys show each one on the window as you go; Enter keeps "
                 "it and Escape puts back what there was. It changes Omabook only, not the desktop.")},
             {tr("From a terminal"),
              tr("`omabook theme list` shows the themes, `omabook theme use nord` picks one, and `omabook theme use "
                 "~/colors.toml` takes any colors.toml. `omabook theme import <name>` makes a theme of your own from an "
                 "Aether blueprint.")},
             {tr("Your own colors"),
              tr("Colors in `~/.config/omabook/style.json` stay over whichever theme is chosen, so a theme can be the "
                 "start and one color your own.")},
         }},
        {"agents", tr("For agents"), tr("The command line, the MCP server, and the interface as files."),
         {
             {tr("The command line"),
              tr("Everything the window does with the library is a command that needs no display: `omabook list`, "
                 "`omabook add`, `omabook text`, `omabook lookup` and the rest. Each takes `--json` and exits 0 or 1. "
                 "`omabook --help` lists them, and `omabook help <topic>` prints these pages.")},
             {tr("The MCP server"),
              tr("`omabook mcp install` connects a coding agent to the library as tools: search, read, fix details, "
                 "track reading, tidy up. `omabook mcp --tools` lists them.")},
             {tr("The skill"),
              tr("`omabook skill install` teaches your coding agents the commands.")},
             {tr("Keys, colors and views of your own"),
              tr("The interface is files in `~/.config/omabook/`, followed while Omabook runs: `keys.json` for any key, "
                 "`style.json` for colors and corners, `ui/` for views of your own and `mods/` for panels. "
                 "`omabook customize` writes a guide to all of it there. A file with a mistake never takes the window "
                 "down: the last version that worked stays in use.")},
         }},
    };
}
