#include "manual.h"

// The first-use tour: a few cards, each pointing at a real part of the
// window. It is offered once, on a first start, and is under ? ever after.
//
// A step is {id, "", title, text, spot, command, place, when} (see
// Manual::Step). `spot` is an objectName in the QML; `book` stands for the
// selected book in whichever view is showing. A step whose spot isn't on
// screen is left out, so the tour never points at nothing. Keep each to a
// sentence or two, and the whole to what fits in a minute. A test holds
// every spot and command named here to the window and the commands table.
QList<Manual::Step> Manual::tourSteps() {
    return {
        {"welcome", "", tr("Welcome to Omabook"),
         tr("Your books on a shelf you run from the keyboard, and a reader for them. This is a short walk through the "
            "window; Escape leaves it at any time."),
         "", "", "library", ""},
        {"add", "", tr("Start with a book"),
         tr("{library.add} adds book files, and {library.add_folder} a folder of them or a whole Calibre library with "
            "everything it knows. {library.new} adds a book you own on paper. Your own files are never touched."),
         "emptyState", "", "library", "empty"},
        {"book", "", tr("Your books"),
         tr("The arrow keys, or {library.left} {library.down} {library.up} {library.right}, move around. "
            "{library.read} opens the selected book where you left off."),
         "book", "", "library", "books"},
        {"details", "", tr("The selected book"),
         tr("What the library knows about it, your stars, and how far you are. {library.edit} edits the details and "
            "{library.lookup} looks them up online."),
         "details", "", "library", "books"},
        {"search", "", tr("Search"),
         tr("{library.search} searches as you type. A word such as `author:` or `tag:` in front looks in one field."),
         "search|searchChoice", "", "library", ""},
        {"views", "", tr("Five ways to look at it"),
         tr("Covers, spines, a carousel, a list, or stacks by series. {library.view_next} goes to the next one."),
         "viewSwitcher|viewChoice", "", "library", ""},
        {"filter", "", tr("Filter and order"),
         tr("{library.filter} narrows the shelf: type a few letters of a status, a tag, an author or a format. "
            "{library.sort} changes the order."),
         "filterLabel|touchFilter|libraryMore", "", "library", ""},
        {"sites", "", tr("Where you kept your reading so far"),
         tr("{library.sites} connects Goodreads, Open Library or Hardcover: what you read, are reading and want to read "
            "comes in with your stars and dates. Nothing is removed, and nothing goes back unless you turn that on."),
         "", "library.sites", "library", ""},
        {"companion", "", tr("AI, when you ask"),
         tr("{library.ask} asks about your library and {library.suggest} suggests what to read next. In a book, a "
            "companion reads along without spoiling the ending. Nothing is sent until you ask."),
         "", "", "library", ""},
        {"end", "", tr("That is the shelf"),
         tr("{global.help} shows every key, and finds one when you type what you want to do. Open a book and press it "
            "there for help with reading."),
         "", "", "library", ""},

        {"page", "", tr("The page"),
         tr("{reader.page_down} turns the page and carries on into the next chapter; {reader.page_up} goes back. "
            "{reader.larger} and {reader.smaller} change the size of the type."),
         "", "", "reader", ""},
        {"place", "", tr("Where you are"),
         tr("How far through the book you are. Your place is kept when you leave, and {reader.bookmark} keeps a "
            "bookmark besides."),
         "readerPlace", "", "reader", ""},
        {"contents", "", tr("Contents"),
         tr("{reader.contents} shows the chapters beside the page. {reader.find} finds words in the book."),
         "contents", "", "reader", ""},
        {"assistant", "", tr("The AI companion"),
         tr("{reader.ask} asks about what you are reading, {reader.catch_up} catches you up after a break, and "
            "{reader.cast} says who is who. It is sent the book only as far as you have read."),
         "assistantPane", "", "reader", ""},
        {"fingers", "", tr("For fingers"),
         tr("This button shows what the keys do. Swipe, or tap the edges, to turn the page."),
         "menuButton", "", "reader", "touch"},
        {"leave", "", tr("That is the reader"),
         tr("{reader.expand} hides everything but the book, and {reader.back} goes back to the shelf. {global.help} "
            "has the rest."),
         "", "", "reader", ""},
    };
}
