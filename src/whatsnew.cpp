#include "manual.h"

// What each release brought. When the version of Omabook is newer than the
// one a person last saw, a quiet note at the bottom of the window offers a
// short tour of the entries that arrived since; "What's new" in the help
// (?) and `omabook help new` show the newest release's at any time.
//
// TO ANNOUNCE A FEATURE, add one entry here, and nothing else:
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
QList<Manual::Step> Manual::features() {
    return {
        {"views", "0.1.0", tr("Five views of the shelf"),
         tr("Covers, spines on a bookshelf, a carousel, a list, or stacks by series. Each library remembers its own."),
         "viewSwitcher|viewChoice", "library.view_next", "library", ""},
        {"filter", "0.1.0", tr("Filter by typing"),
         tr("{library.filter} and a few letters of a status, a tag, an author, a format or a rating. Filters add up, "
            "and a click on a chip takes one off."),
         "filterLabel|touchFilter|touchMore", "library.filter", "library", ""},
        {"paper", "0.1.0", tr("Books on paper, and wishes"),
         tr("{library.new} adds a book that has no file: one on your shelf at home, or one to read some day. They count "
            "in what you have read."),
         "", "library.new", "library", ""},
        {"companion", "0.1.0", tr("A companion that can't spoil"),
         tr("While reading, {reader.ask} asks about the page, {reader.catch_up} catches you up, and {reader.cast} says "
            "who is who. It never sees past where you are."),
         "assistantPane", "reader.ask", "reader", ""},
        {"tidy", "0.1.0", tr("Tidy the library"),
         tr("{library.tidy} finds the same book twice, authors spelled several ways and books that belong to a series, "
            "as one list to tick. Nothing changes until you say so."),
         "", "library.tidy", "library", ""},
        {"suggest", "0.1.0", tr("What to read next"),
         tr("{library.suggest} suggests books from everything you have read, on paper too, and checks each one against "
            "the reading sites."),
         "", "library.suggest", "library", ""},
        {"change", "0.1.0", tr("Change Omabook by asking"),
         tr("{global.change} takes a wish in words and rewrites the keys, colors and settings for you. One click undoes it."),
         "", "global.change", "", ""},
        {"plugins", "0.1.0", tr("Plugins, and scripts of your own"),
         tr("{library.plugins} lists every source and format, Calibre's among them. A script in the plugins folder can "
            "add commands, panels and sources."),
         "", "library.plugins", "library", ""},
        {"sync", "0.1.0", tr("Other computers"),
         tr("{library.sync} brings this library and another computer's in step over ssh; the later reading place wins."),
         "", "library.sync", "library", ""},
        {"graph", "0.1.0", tr("A knowledge graph of what you read"),
         tr("As you finish chapters, the AI notes who and what is in the book and how they relate, never past where "
            "you are. Your questions, stars and reading days join it. {library.graph} shows it."),
         "", "library.graph", "library", ""},
        {"sites", "0.1.0", tr("Reading sites, connected"),
         tr("{library.sites} connects Goodreads, Open Library and Hardcover: paste a link to your profile and it says what "
            "it found. Open Library and Hardcover can take your changes back, once you say so; tokens stay in the keyring."),
         "", "library.sites", "library", ""},
        {"help", "0.1.0", tr("Help you can search"),
         tr("{global.help} lists every key as it is bound now. Type what you want to do, and Enter runs it."),
         "", "global.help", "", ""},
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
        {"folio-folders", "0.1.4", tr("Your library as a folder"),
         tr("In Folio, the file manager, each library is a folder of its books: open one to read it here, copy a "
            "book in to add it, or delete one to send it to the trash."),
         "", "", "", ""},
    };
}
