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
    };
}

// The 0.1.10 release's entries: writing by hand, on a PDF's or a comic's pages (#83) and in a book's margin (#90);
// notes on fixed pages, and searched (#115); others' annotations in a space (#85) and asking about one's own (#86);
// and reading statistics with a goal for the year (#68).
QList<Manual::Step> inkRelease() {
    return {
        {"book-ink", "0.1.10", Manual::tr("Write by hand"),
         Manual::tr("{reader.ink} writes and draws on the pages of a PDF or a comic, with a pen or the mouse. In other books a "
                    "pen writes a margin note on the line, read by your own AI server so the book's search finds it."),
         "", "reader.ink", "reader", ""},
        {"page-notes", "0.1.10", Manual::tr("Notes on PDFs and comics"),
         Manual::tr("Highlight and write margin notes on a PDF's words, and on a part of a comic's page: drag over a panel, or "
                    "{reader.select}. {reader.find} finds the words of your notes too, and `notes:` searches them in the library."),
         "", "reader.find", "reader", ""},
        {"others-annotations", "0.1.10", Manual::tr("Notes shared, and asked about"),
         Manual::tr("A space you share can carry its members' highlights and notes, each beside yours in their colour and never "
                    "past your place. {reader.ask} is given your own up to where you are, so you can ask what you marked."),
         "", "reader.ask", "reader", ""},
        {"reading-stats", "0.1.10", Manual::tr("A year of reading"),
         Manual::tr("{library.stats} shows the time you read, the days in a row and the books finished, by month and by book, "
                    "and how far you are with a goal for the year. It is all counted on your computer."),
         "", "library.stats", "library", ""},
    };
}

// The 0.2.0 release's entries: a book searched by what it means (#72), read aloud (#73), e-readers and phones (#71),
// and the Notebooks plugin that the plugin API of this release was made for (#125 to #138).
QList<Manual::Step> listeningRelease() {
    return {
        {"meaning", "0.2.0", Manual::tr("Find by what it means"),
         Manual::tr("{reader.find} finds a passage by what it means, not only by its words: \"where do they cross the river?\" "
                    "finds them wading over the ford. The book is indexed by your own AI server first, once you say so."),
         "", "reader.find", "reader", ""},
        {"read-aloud", "0.2.0", Manual::tr("Read aloud"),
         Manual::tr("{reader.read_aloud} reads the book aloud from the top of the page in a voice on this computer, marking "
                    "each sentence and turning the pages as it goes; pressed again, it pauses. Nothing leaves the computer unless "
                    "you choose a speech server."),
         "", "reader.read_aloud", "reader", ""},
        {"devices", "0.2.0", Manual::tr("E-readers and phones"),
         Manual::tr("Under {global.settings}, Omabook can serve your books to KOReader and other e-reader and phone apps on your "
                    "tailnet, keep KOReader's reading place in step with yours, and send a book to a Kobo, a Kindle or a PocketBook."),
         "", "global.settings", "", ""},
        {"notebooks", "0.2.0", Manual::tr("Notebooks, by hand"),
         Manual::tr("The Notebooks plugin keeps notebooks on the shelf to write in with a pen, on lined or squared paper, with "
                    "recordings of what was said as you wrote. Find it in the plugins ({library.plugins}) by its name, Notebooks, "
                    "and install it."),
         "", "library.plugins", "library", ""},
    };
}

// The 0.3.0 release's entries: the AI page, with a provider for each thing a model is asked for (#143, #144), Gemini
// (#145), voxtype for recordings (#146) and voices to choose from, with a sentence to hear first (#147).
QList<Manual::Step> providersRelease() {
    return {
        {"ai-page", "0.3.0", Manual::tr("A provider for each use of AI"),
         Manual::tr("In the settings ({global.settings}), AI Settings opens a page with a row for each thing a model is asked "
                    "for: answers, quick work, handwriting, speech to text, text to speech and meaning. Each has a provider of its "
                    "own, and uses a hosted service only once you allow it in that row."),
         "", "global.settings", "", ""},
        {"gemini", "0.3.0", Manual::tr("Gemini, with a key of your own"),
         Manual::tr("Gemini can answer, read handwriting, write out recordings, read aloud and index books by their meaning, "
                    "with a key you give once on the AI page ({global.settings}). It is someone else's computer, so a row uses "
                    "it only once you allow that."),
         "", "global.settings", "", ""},
        {"voxtype", "0.3.0", Manual::tr("Recordings written out by voxtype"),
         Manual::tr("Where voxtype, the speech to text Omarchy ships for dictation, is installed, it writes out recordings on "
                    "this computer, before whisper.cpp or a server. On Omarchy without it, the AI page ({global.settings}) "
                    "offers to install it."),
         "", "global.settings", "", ""},
        {"voices", "0.3.0", Manual::tr("Voices to choose from"),
         Manual::tr("{reader.read_aloud} reads with Piper or speech-dispatcher on this computer, a server of your own, or "
                    "OpenAI's and Gemini's voices once you allow them. Choose one, its voice and its speed in the AI page's Text "
                    "to speech row, and hear a sentence first with Hear It."),
         "", "reader.read_aloud", "reader", ""},
    };
}

// The 0.4.0 release's entries: dictation (#148), asking by voice (#149), voice commands (#150), recordings written out
// (#151), ink in the margins (#152), the margin folded to dots (#167), each AI row tested (#170) and a missing model
// found or fetched (#172), OpenAI's voices over its Realtime API (#169), and sound following the desktop (#173).
QList<Manual::Step> speechRelease() {
    return {
        {"dictation", "0.4.0", Manual::tr("Say a note instead of typing it"),
         Manual::tr("A margin note's microphone, or Ctrl+M in it, listens while you hold it and writes out what you say where "
                    "the cursor is; a quick press listens until the next. It goes through speech to text as you chose it on "
                    "the AI page ({global.settings}), voxtype first, and the top of the window says while it listens."),
         "", "reader.note", "reader", ""},
        {"spoken-answers", "0.4.0", Manual::tr("Ask by voice, and hear the answer"),
         Manual::tr("Hold the companion's microphone, or Ctrl+M in its question, and say what you want to know: it is asked "
                    "when you let go, and the answer is shown and read aloud. Asking again, Escape or {reader.read_aloud_stop} "
                    "stops it. {library.ask} takes a spoken question too."),
         "", "reader.ask", "reader", ""},
        {"voice-commands", "0.4.0", Manual::tr("Say what to do while you read"),
         Manual::tr("Hold {reader.voice}, or Speak in the bar, and say “next chapter”, “previous page”, “read aloud”, “go to "
                    "chapter 5”, “highlight this” or “what does this word mean”, in any of the window's languages. It listens "
                    "only while you ask it to, and the top of the window says so."),
         "", "reader.voice", "reader", ""},
        {"transcripts", "0.4.0", Manual::tr("Recordings, written out"),
         Manual::tr("An MP3, M4B or other recording in the library becomes a book to read once it is written out: "
                    "{library.transcribe}, or Write Out in its details, does it in the background through speech to text, "
                    "and the search then finds what is said in it."),
         "", "library.transcribe", "library", ""},
        {"margin-ink", "0.4.0", Manual::tr("Ink in the margins"),
         Manual::tr("{reader.ink} writes and draws in the margins and between the lines of an EPUB, a FictionBook, Markdown "
                    "or a text, as on a PDF's pages. Each stroke stays beside its line however the text is laid out."),
         "", "reader.ink", "reader", ""},
        {"fold-margin", "0.4.0", Manual::tr("Fold the margin notes away"),
         Manual::tr("{reader.fold_margin} folds the margin column away for every book, and the text takes its room. A dot "
                    "beside a line stands for its notes; a click, a tap or the pen opens them beside the words."),
         "", "reader.fold_margin", "reader", ""},
        {"ai-tests", "0.4.0", Manual::tr("Each use of AI, tested"),
         Manual::tr("Each row of the AI page ({global.settings}) says what Omabook uses it for, and its Test sends one small "
                    "request and shows what came back, or why not. Handwriting can have a server of its own."),
         "", "global.settings", "", ""},
        {"missing-model", "0.4.0", Manual::tr("A missing model, found or fetched"),
         Manual::tr("Where a model isn't on its server, Omabook offers one the server has that can do the work, or downloads "
                    "a small, well-known one onto Ollama with one press."),
         "", "global.settings", "", ""},
        {"realtime-voices", "0.4.0", Manual::tr("OpenAI's voices read on"),
         Manual::tr("When OpenAI shuts its speech API down in January 2027, {reader.read_aloud} goes on with its voices over "
                    "its Realtime API, from the same sentence."),
         "", "reader.read_aloud", "reader", ""},
        {"sound-follows", "0.4.0", Manual::tr("Sound follows the desktop"),
         Manual::tr("Switch to headphones or a Bluetooth speaker, or unplug them, and what is read aloud or played moves "
                    "along as it plays; dictation listens on the new microphone."),
         "", "", "", ""},
    };
}

// The 0.4.1 release's entries: timelines that branch (#171), a phone connected by its camera (#177), a fallback for
// each use of AI (#174) and a model found on Ollama's library or Hugging Face (#175).
QList<Manual::Step> branchesRelease() {
    return {
        {"branches", "0.4.1", Manual::tr("Timelines that branch"),
         Manual::tr("{library.chronology_book} draws an alternate history, an alternative universe or a reboot as a branch "
                    "beside the timeline it splits off: the same up to the split, its own after it. Where the AI reads one, it "
                    "proposes it, for you to take or not."),
         "", "library.chronology_book", "library", ""},
        {"phones", "0.4.1", Manual::tr("Your library on your phone"),
         Manual::tr("{library.devices} shows a QR code of your library's catalog for KOReader, Librera or Moon+ Reader to scan, "
                    "serves it on your tailnet or your home network, and on with the window closed if you like."),
         "", "library.devices", "library", ""},
        {"ai-fallback", "0.4.1", Manual::tr("A second server for each use of AI"),
         Manual::tr("When a row's server on the AI page ({global.settings}) can't be reached at all, it can fall back to this "
                    "computer or another server, and says so while it does. Omabook goes back to the first once it answers."),
         "", "global.settings", "", ""},
        {"find-model", "0.4.1", Manual::tr("Find a model for the work"),
         Manual::tr("Find a Model… in a row of the AI page ({global.settings}) searches Ollama's library and Hugging Face for "
                    "models that can do that work, says whether each fits this computer, and installs the one you choose."),
         "", "global.settings", "", ""},
    };
}

// The 0.5.0 release's entries: the hub (#179, #180, #188, #189), snapshots and what changed since one (#181, #183,
// #185), destinations and their warnings (#182, #184), the upgrade's repair behind a moment snapshot (#208, #213), and
// a sync checked before it writes (#187, #209).
QList<Manual::Step> hubRelease() {
    return {
        {"hub", "0.5.0", Manual::tr("A hub that is always on"),
         Manual::tr("A computer that stays on can keep your libraries without a window: `omabook hub run` there, and "
                    "{library.hubs} here links this computer to it over your ssh, by a key that opens nothing but Omabook. "
                    "{library.sync} then syncs with the hub as with another computer."),
         "", "library.hubs", "library", ""},
        {"snapshots", "0.5.0", Manual::tr("Snapshots to go back to"),
         Manual::tr("{library.snapshots} keeps encrypted snapshots of the library, and takes one by itself just before a "
                    "sync, an import or a tidy changes something. It shows what changed since any of them, and puts a book's "
                    "details or notes back as they were, after asking."),
         "", "library.snapshots", "library", ""},
        {"destinations", "0.5.0", Manual::tr("A second disk, and one away from home"),
         Manual::tr("Under Destinations, {library.snapshots} copies the snapshots to another disk and to a place away from "
                    "home. When the newest is old, a check failed or a disk is missing, it says so in red, with what to do."),
         "", "library.snapshots", "library", ""},
        {"upgrade-repair", "0.5.0", Manual::tr("Books synced earlier, set right"),
         Manual::tr("This upgrade takes a moment snapshot first, where snapshots are set up, and then sets right, once, the "
                    "books a computer got by a sync with an earlier Omabook: a change made to one reaches your other "
                    "computers again, with all its details. Nothing else changes."),
         "", "", "", ""},
        {"sync-guard", "0.5.0", Manual::tr("A sync checked before it writes"),
         Manual::tr("A sync is planned on both computers first, and refused, with nothing changed on either, when it would "
                    "lose a book or empty a detail. It names the books and why, and a shelf, stars or a finished day changed "
                    "on one computer now reach the other."),
         "", "library.sync", "library", ""},
    };
}

// The 0.5.1 release's entries: the plugins' files through a hub (#212), a hub's library paired once (#214, #217),
// and a sync that no longer refuses for a clock behind or a new name (#211, #215).
QList<Manual::Step> hubSyncRelease() {
    return {
        {"hub-plugin-files", "0.5.1", Manual::tr("Notebooks through the hub too"),
         Manual::tr("A sync with the hub now carries the plugins' files as well, such as Notebooks' recordings and "
                    "Storyteller's stories, and gives them only to your own computers."),
         "", "library.hubs", "library", ""},
        {"hub-pair", "0.5.1", Manual::tr("A hub's library, paired once"),
         Manual::tr("A library on the hub syncs only with the one it is paired with. Where a sync was refused for that, Pair "
                    "in {library.hubs} asks first, then pairs the two and syncs them."),
         "", "library.hubs", "library", ""},
        {"sync-clocks-names", "0.5.1", Manual::tr("Fewer refusals for nothing"),
         Manual::tr("A computer whose clock is behind, or that was given a new name, now syncs on: reading places go in the "
                    "order they were read, and a computer is known by the machine it runs on."),
         "", "library.sync", "library", ""},
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
    } + collectionsRelease() + annotationsRelease() + inkRelease() + listeningRelease() + providersRelease()
           + speechRelease() + branchesRelease() + hubRelease() + hubSyncRelease();
}
