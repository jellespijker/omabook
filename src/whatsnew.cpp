#include "manual.h"

// What each release brought. When the version of Omabook is newer than the
// one a person last saw, a quiet note at the bottom of the window offers a
// short tour of the entries that arrived since; "What's new" in the help
// (?) and `omabook help new` show the newest release's at any time.
//
// TO ANNOUNCE A FEATURE, add one entry at the end of features(), and nothing
// else (the first release's entries are in firstRelease(), to keep it short):
//
//   {"id", "version", tr("Title"), tr("One or two sentences."), "spot", "command", "place", ""},
//
//   id        a name of its own, never reused
//   version   the release it arrives in, as in CMakeLists.txt ("0.2.0")
//   title     a few words
//   text      one or two sentences; name a command as {library.add} and its
//             key is filled in, whatever the user bound it to
//   spot      optional: the objectName of what to point at in the window
//             (a|b takes the first that shows); "" puts the card in the middle
//   command   optional: the command the card is about; its key is shown
//   place     "library" or "reader" when the spot only exists there, else ""
//
// Then run bin/i18n and translate the two texts in src/i18n/. A test
// (whatIsSaidStillExists in tests/ui_tests.cpp) fails when an entry names a
// command or an objectName that isn't there, so a rename can't leave the
// tour pointing at nothing. Entries are never removed: someone who skips
// three releases is shown all three.
namespace {

// What the first release, 0.1.0, brought.
QList<Manual::Step> firstRelease() {
    return {
        {"views", "0.1.0", Manual::tr("Five views of the shelf"),
         Manual::tr("Covers, spines on a bookshelf, a carousel, a list, or stacks by series. Each library remembers its own."),
         "viewSwitcher|viewChoice", "library.view_next", "library", ""},
        {"filter", "0.1.0", Manual::tr("Filter by typing"),
         Manual::tr("{library.filter} and a few letters of a status, a tag, an author, a format or a rating. Filters add up, "
                    "and a click on a chip takes one off."),
         "filterLabel|touchFilter|libraryMore", "library.filter", "library", ""},
        {"paper", "0.1.0", Manual::tr("Books on paper, and wishes"),
         Manual::tr("{library.new} adds a book that has no file: one on your shelf at home, or one to read some day. They count "
                    "in what you have read."),
         "", "library.new", "library", ""},
        {"companion", "0.1.0", Manual::tr("A companion that can't spoil"),
         Manual::tr("While reading, {reader.ask} asks about the page, {reader.catch_up} catches you up, and {reader.cast} says "
                    "who is who. It never sees past where you are."),
         "assistantPane", "reader.ask", "reader", ""},
        {"tidy", "0.1.0", Manual::tr("Tidy the library"),
         Manual::tr("{library.tidy} finds the same book twice, authors spelled several ways and books that belong to a series, "
                    "as one list to tick. Nothing changes until you say so."),
         "", "library.tidy", "library", ""},
        {"suggest", "0.1.0", Manual::tr("What to read next"),
         Manual::tr("{library.suggest} suggests books from everything you have read, on paper too, and checks each one against "
                    "the reading sites."),
         "", "library.suggest", "library", ""},
        {"change", "0.1.0", Manual::tr("Change Omabook by asking"),
         Manual::tr("{global.change} takes a wish in words and rewrites the keys, colors and settings for you. One click undoes it."),
         "", "global.change", "", ""},
        {"plugins", "0.1.0", Manual::tr("Plugins, and scripts of your own"),
         Manual::tr("{library.plugins} lists every source and format, Calibre's among them. A script in the plugins folder can "
                    "add commands, panels and sources."),
         "", "library.plugins", "library", ""},
        {"sync", "0.1.0", Manual::tr("Other computers"),
         Manual::tr("{library.sync} brings this library and another computer's in step over ssh; the later reading place wins."),
         "", "library.sync", "library", ""},
        {"graph", "0.1.0", Manual::tr("A knowledge graph of what you read"),
         Manual::tr("As you finish chapters, the AI notes who and what is in the book and how they relate, never past where "
                    "you are. Your questions, stars and reading days join it. {library.graph} shows it."),
         "", "library.graph", "library", ""},
        {"sites", "0.1.0", Manual::tr("Reading sites, connected"),
         Manual::tr("{library.sites} connects Goodreads, Open Library and Hardcover: paste a link to your profile and it says what "
                    "it found. Open Library and Hardcover can take your changes back, once you say so; tokens stay in the keyring."),
         "", "library.sites", "library", ""},
        {"help", "0.1.0", Manual::tr("Help you can search"),
         Manual::tr("{global.help} lists every key as it is bound now. Type what you want to do, and Enter runs it."),
         "", "global.help", "", ""},
    };
}

// The 0.1.6 release's entries: sharing a collection, by publishing it, subscribing to it, or keeping it together.
QList<Manual::Step> sharingRelease() {
    return {
        {"publish", "0.1.6", Manual::tr("Share a collection"),
         Manual::tr("{library.publish} publishes the collection shown, as a file or a link, and says first what goes in. The "
                    "books' files never do, and nothing of your own reading goes unless you switch it on."),
         "", "library.publish", "library", ""},
        {"subscribe", "0.1.6", Manual::tr("Subscribe to one"),
         Manual::tr("{library.subscribe} takes a collection someone shared: the books you have are matched, the others become "
                    "wishes, and its graph shows only as far as you have read each book yourself."),
         "", "library.subscribe", "library", ""},
        {"shared-spaces", "0.1.6", Manual::tr("A collection kept together"),
         Manual::tr("For a household or a book club, `omabook spaces` makes a collection several people write, kept in step "
                    "through a folder you already sync."),
         "", "", "", ""},
    };
}

// The 0.1.8 release's entries: a graph for each top-level collection, and a large library made quick.
QList<Manual::Step> collectionsRelease() {
    return {
        {"graph-per-collection", "0.1.8", Manual::tr("A graph for each collection"),
         Manual::tr("Each top-level collection has a knowledge graph of its own, and so do the books in none. In the graph, "
                    "{graph.tenant} picks whose is shown, and {graph.tenant_next} and {graph.tenant_previous} step through them; "
                    "one graph for the whole library is a choice in the settings ({global.settings})."),
         "", "library.graph", "library", ""},
        {"quick-library", "0.1.8", Manual::tr("A large library, quick"),
         Manual::tr("Search, the graph's window, syncing and the first opening after an update are much quicker on a library of "
                    "thousands of books. A search word now finds what starts with it: mara finds Mára."),
         "", "library.search", "library", ""},
    };
}

// The 0.1.9 release's entries: highlights and margin notes in books, found again by their words (#67), and
// justified text hyphenated (#69).
QList<Manual::Step> annotationsRelease() {
    return {
        {"highlights", "0.1.9", Manual::tr("Highlights in five colours"),
         Manual::tr("Select words, with the mouse or {reader.select} and the arrows, and press 1 to 5 for a colour, or pick one "
                    "in the menu beside them; a pen's line under them does it too. {reader.annotations} lists them by colour."),
         "", "reader.annotations", "reader", ""},
        {"margin-notes", "0.1.9", Manual::tr("Notes in the margin"),
         Manual::tr("{reader.note} writes a note on the selected words, beside them where the window has room, and {reader.notes} "
                    "goes through the page's notes. Notes and highlights are found again by their words when a book's file changes."),
         "", "reader.note", "reader", ""},
        {"hyphenation", "0.1.9", Manual::tr("Justified, without wide gaps"),
         Manual::tr("Justified text breaks a long word at the end of a line by the book's language, as LibreOffice does. "
                    "Hyphenate in the Aa panel ({reader.format}) turns it off."),
         "", "reader.format", "reader", ""},
        {"handwritten-notes", "0.1.10", Manual::tr("Margin notes by hand"),
         Manual::tr("With a pen, write on the text or in the margin beside it: a note by hand on that line, read by your own "
                    "AI server so the book's search finds it."),
         "", "reader.notes", "reader", ""},
    };
}

// The 0.1.10 release's entries: book ink (#83).
QList<Manual::Step> inkRelease() {
    return {
        {"book-ink", "0.1.10", Manual::tr("Ink on the pages"),
         Manual::tr("Write and draw on the pages of a PDF or a comic with a pen or the mouse: {reader.ink}. The ink stays where you "
                    "put it at every zoom, and goes to your other computers."),
         "", "reader.ink", "reader", ""},
    };
}

} // namespace

QList<Manual::Step> Manual::features() {
    return firstRelease() + QList<Step>{
        {"graphbuild", "0.1.1", tr("A graph for books already read"),
         tr("{library.graph_build} notes the people, places and ideas of books you read before, or elsewhere, and says what it will cost first. "
            "Books are now linked to each other where they share an idea."),
         "", "library.graph_build", "library", ""},
        {"pluginsources", "0.1.1", tr("Plugins from GitHub that keep up to date"),
         tr("In the plugins ({library.plugins}), add one by its GitHub address. It updates by itself, unless an update asks for more than it had."),
         "", "library.plugins", "library", ""},
        {"panes", "0.1.1", tr("Panes as wide as you like"),
         tr("Drag the inner edge of a pane beside the page or the shelf, or use {global.pane_narrower} and {global.pane_wider}. A double click puts it back."),
         "", "global.pane_wider", "", ""},
        {"themes", "0.1.2", tr("Omarchy's themes, everywhere"),
         tr("Automatic follows Omarchy, or Aether, as they change. In the settings ({global.settings}), Choose Theme shows "
            "each of Omarchy's own themes on the window as you move through them, also on a computer without Omarchy."),
         "", "global.settings", "", ""},
        {"marks", "0.1.2", tr("The graph, in the text"),
         tr("With the map open ({reader.graph}), the people, places and things it knows are marked on the page in their "
            "colors; point at one for what is known of it. Select words, or press {reader.select}, to ask about them."),
         "", "reader.select", "reader", ""},
        {"format", "0.1.3", tr("Set the text your way"),
         tr("{reader.format} opens the Aa panel beside the page: the font and its size, the widest line, the spacing, an "
            "indent, the margins, and text justified, centred or to either side. For every book, or only this one."),
         "", "reader.format", "reader", ""},
        {"pages", "0.1.4", tr("One, two or three pages"),
         tr("Lines are as long as read well, 66 characters in your font at your size, and the window shows as many "
            "pages side by side as it has room for, up to three. {reader.pages} chooses one, two or three yourself."),
         "", "reader.pages", "reader", ""},
        {"moments", "0.1.4", tr("Back to the moment, without spoilers"),
         tr("The map ({reader.graph}) says when each thing was revealed and takes you to that very line. What happens "
            "past your furthest page stays closed until you ask, for that one thing."),
         "", "reader.graph", "reader", ""},
        {"folio-folders", "0.1.4", tr("Your library as a folder"),
         tr("In Folio, the file manager, each library is a folder of its books: open one to read it here, copy a "
            "book in to add it, or delete one to send it to the trash."),
         "", "", "", ""},
        {"shelf-story", "0.1.4", tr("Write a story from the shelf"),
         tr("With the Storyteller plugin switched on, \"Write a new story\" is the first thing on the shelf. Open it like "
            "a book to say what it should be, how long, and whether it lives in the world of one of your stories."),
         "", "library.plugins", "library", ""},
        {"collections", "0.1.4", tr("Collections"),
         tr("Parts of one library, named as you like and one inside another if you wish, as tabs above the shelf: "
            "{library.collections} makes one from a search, each keeps its own view and reading format, and the AI "
            "can propose where your books belong."),
         "collectionStrip|filterLabel|touchFilter", "library.collections", "library", ""},
        {"search-known", "0.1.4", tr("Search what you know, or ask"),
         tr("The search finds books by the characters, places and ideas you met in them, never past where you are, and "
            "says which. Start with ? or press {library.search_ai} to have the AI find books for a question."),
         "search|searchChoice", "library.search_ai", "library", ""},
        {"find-plugins", "0.1.4", tr("Plugins to try"),
         tr("{library.plugins}, then Install: a list of plugins to find and install with one click, each checked and "
            "showing what it asks to be allowed before anything runs."),
         "", "library.plugins", "library", ""},
        {"pointer", "0.1.4", tr("Every key, a click or a tap away"),
         tr("The ⋯ above the shelf, over the page and in the graph lists all they can do, each with its key; a right "
            "click on a book lists what can be done with it. {global.help} shows the keys a plugin adds under its "
            "name, and the keys of each panel."),
         "libraryMore|touchBar", "global.help", "library", ""},
        {"layout", "0.1.5", tr("Pages or scroll"),
         tr("{reader.layout} reads a book as pages, each chapter starting on a page of its own and a PDF page shown whole, "
            "or as one column that runs on from chapter to chapter. A turn never shows a line twice."),
         "", "reader.layout", "reader", ""},
    } + sharingRelease() + QList<Step>{
        {"ai-servers", "0.1.7", tr("Your own AI server"),
         tr("Under {global.settings}, the AI can be a server: Omarchy's local models, Ollama on this computer or another "
            "one, OpenAI, or any other by its address. Its models are listed to pick from."),
         "", "global.settings", "", ""},
        {"book-files", "0.1.7", tr("The books' files, when you allow"),
         tr("{library.publish} can send the books' files too, beside the file you give, to everyone it reaches; a shared "
            "space carries them once a writer says so. A file comes only when its book is opened, and never in place of "
            "your own copy."),
         "", "library.publish", "library", ""},
        {"chronology", "0.1.7", tr("When it happens"),
         tr("{library.chronology_book} shows when a book's events happen in its story, on its timelines and with the "
            "dates it gives, as far as you have read. A tap on an event goes back to the page that told it."),
         "", "library.chronology_book", "library", ""},
    } + collectionsRelease() + annotationsRelease() + inkRelease();
}
