#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>

// The commands Omabook has, each with the keys it has by default and what it
// does: the table the help (?), keys.json and `omabook help keys` all read
// (Customization::commands), checked when the program is built.
namespace CommandTable {

// Where keys work: what a command's id begins with, and what keys.json is divided into.
inline constexpr std::array<std::string_view, 4> places{"global", "library", "reader", "graph"};

[[nodiscard]] constexpr std::string_view placeOf(std::string_view command) noexcept { return command.substr(0, command.find('.')); }

// A built-in command as it is written down below, all of it known when the
// program is built.
struct BuiltIn {
    std::string_view id;
    // The keys it has by default: three at most, the room left over empty.
    std::array<std::string_view, 3> keys;
    const char *what;
};

[[nodiscard]] constexpr auto keysOf(const BuiltIn &command) {
    return command.keys | std::views::filter([](std::string_view key) { return !key.empty(); });
}

// The descriptions stay in English here, for AGENTS.md; marking each
// with tr() puts it in the catalogs, and the help translates it when
// it is shown (see Manual::commands).
constexpr auto tr = [](const char *text) noexcept { return text; };

inline constexpr BuiltIn builtIns[] = {
    {"global.help", {"?", "F1"}, tr("Show or hide the help: every key, and how things work")},
    {"global.quit", {"Q", "Ctrl+Q"}, tr("Quit")},
    {"global.settings", {","}, tr("Open the settings")},
    {"global.change", {"Ctrl+K"}, tr("Ask the AI to change how Omabook looks or works")},
    {"global.pane_wider", {">"}, tr("Make the pane beside the shelf or the page wider")},
    {"global.pane_narrower", {"<"}, tr("Make the pane beside the shelf or the page narrower")},
    {"library.read", {"Space"}, tr("Read the selected book")},
    {"library.left", {"H"}, tr("Select the book to the left")},
    {"library.right", {"L"}, tr("Select the book to the right")},
    {"library.down", {"J"}, tr("Select the book below")},
    {"library.up", {"K"}, tr("Select the book above")},
    {"library.search", {"/", "Ctrl+F"}, tr("Search the library")},
    {"library.search_ai", {"Shift+W"}, tr("Have the AI find books for a question, such as “the books where a mentor dies”")},
    {"library.found_graph", {"Shift+I"}, tr("Show in the graph what the search found the selected book by")},
    {"library.found_moment", {"Shift+Space"}, tr("Go to where in the book the search found the selected book by")},
    {"library.add", {"A"}, tr("Add books")},
    {"library.add_folder", {"Shift+A"}, tr("Add a folder: of books, a Calibre library, or one book kept as a folder of Markdown files")},
    {"library.new", {"B"}, tr("Add a book that has no file: one on your shelf, or one to read some day")},
    {"library.edit", {"E"}, tr("Edit the selected book's details")},
    {"library.lookup", {"M"}, tr("Look the selected book's details up")},
    {"library.export", {"X"}, tr("Save a copy of the selected book elsewhere")},
    {"library.remove", {"D", "Delete"}, tr("Remove the selected book (asks first)")},
    {"library.sort", {"S"}, tr("Change the order of the shelf")},
    {"library.sort_reverse", {"Shift+S"}, tr("Turn the order of the shelf round")},
    {"library.filter", {"F"}, tr("Filter the shelf: by status, paper or digital, format, language, tag, author, series or rating")},
    {"library.filter_clear", {"Shift+F"}, tr("Take every filter off the shelf")},
    {"library.view_covers", {"1"}, tr("Show the library as covers")},
    {"library.view_spines", {"2"}, tr("Show the library as spines on a bookshelf")},
    {"library.view_carousel", {"3"}, tr("Show the library as a carousel")},
    {"library.view_list", {"4"}, tr("Show the library as a list")},
    {"library.view_stacks", {"5"}, tr("Show the library as stacks by series, author, tag or status")},
    {"library.view_next", {"V"}, tr("Show the library the next way")},
    {"library.group", {"G"}, tr("Stack by the next of series, author, tag and status")},
    {"library.libraries", {"O"}, tr("Open another library")},
    {"library.collections", {"C"}, tr("Show one collection of the library, or all books; make, change or sort collections")},
    {"library.collection_next", {"]"}, tr("Show the next collection")},
    {"library.collection_previous", {"["}, tr("Show the previous collection")},
    {"library.collection_book", {"Shift+C"}, tr("Put the selected book in collections, or take it out")},
    {"library.publish",
     {"Shift+P"},
     tr("Publish the collection shown for others to subscribe to: says first what goes in, and what never does")},
    {"library.subscribe", {"Shift+O"}, tr("Subscribe to a collection someone published, from its .omabook-space file")},
    {"library.sync", {"Y"}, tr("Sync with the computer named in the settings")},
    {"library.plugins", {"P"}, tr("Show the plugins")},
    {"library.details", {"I"}, tr("Show or hide the details beside the shelf")},
    {"library.tidy", {"Shift+T"}, tr("Look the whole library over for things to tidy")},
    {"library.sites", {"R"}, tr("Sync with Goodreads and other reading sites")},
    {"library.ask", {"W"}, tr("Ask the AI about the library")},
    {"library.suggest", {"N"}, tr("Have the AI suggest what to read next")},
    {"library.graph", {"6"}, tr("Show the library as a graph of what is known: books, people, places, ideas and how they relate")},
    {"library.graph_book", {"U"}, tr("Show what is known of the selected book as a graph")},
    {"library.graph_build", {"Shift+U"}, tr("Build the knowledge graph for books you have already read")},
    {"reader.back", {"Escape", "Backspace"}, tr("Close a pane, or go back to the library")},
    {"reader.page_down", {"Space", "Right"}, tr("Turn the page, or scroll on a screen, on into the next chapter")},
    {"reader.page_up", {"Shift+Space", "Left"}, tr("Turn the page back, or scroll back a screen")},
    {"reader.line_down", {"J", "Down"}, tr("Scroll down a few lines")},
    {"reader.line_up", {"K", "Up"}, tr("Scroll up a few lines")},
    {"reader.next", {"PgDown", "L", "N"}, tr("Go to the next chapter")},
    {"reader.previous", {"PgUp", "H", "P"}, tr("Go to the start of the chapter, or from there to the previous chapter")},
    {"reader.start", {"G", "Home"}, tr("Go to the start of the chapter")},
    {"reader.end", {"Shift+G", "End"}, tr("Go to the end of the chapter")},
    {"reader.larger", {"+", "="}, tr("Larger text, or zoom in")},
    {"reader.smaller", {"-"}, tr("Smaller text, or zoom out")},
    {"reader.contents", {"T"}, tr("Show or hide the contents beside the page")},
    {"reader.find", {"/", "Ctrl+F"}, tr("Find words in the book")},
    {"reader.bookmark", {"B"}, tr("Bookmark this place")},
    {"reader.ask", {"A"}, tr("Ask the AI about what is being read")},
    {"reader.catch_up", {"C"}, tr("Have the AI catch you up")},
    {"reader.cast", {"W"}, tr("Have the AI say who is who so far")},
    {"reader.expand", {"F", "F11"}, tr("The book and nothing else, on the whole screen")},
    {"reader.graph", {"M"}, tr("Show or hide the map of what the book has told so far, beside the page")},
    {"reader.marks", {"Shift+M"}, tr("Mark the graph's people, places and things in the text: while the map is open, always, or never")},
    {"reader.select",
     {"V"},
     tr("Select words with the keyboard: the arrows or H J K L move, Shift widens, Enter shows what to do with them")},
    {"reader.format",
     {"Shift+A"},
     tr("Show or hide how the text is set: the font, its size, the longest line, the pages side by side, the spacing, the margins and the "
        "alignment, for this book or every book")},
    {"reader.pages", {"Shift+P"}, tr("Pages side by side: as many as fit, one, two or three, and round again")},
    {"reader.layout",
     {"Shift+L"},
     tr("Read as pages that turn, as one column that scrolls on through the chapters, or automatically, and round again")},
    // The graph view, wherever it is open: over the shelf or beside the page.
    {"graph.left", {"H", "Left"}, tr("Select the nearest related thing to the left")},
    {"graph.right", {"L", "Right"}, tr("Select the nearest related thing to the right")},
    {"graph.up", {"K", "Up"}, tr("Select the nearest related thing above")},
    {"graph.down", {"J", "Down"}, tr("Select the nearest related thing below")},
    {"graph.next", {"Tab"}, tr("Select the next thing in the list")},
    {"graph.previous", {"Shift+Tab"}, tr("Select the previous thing in the list")},
    {"graph.expand", {"Return", "Enter"}, tr("Centre on the selected thing and show what is around it")},
    {"graph.back", {"Backspace"}, tr("Go back to what the graph showed before")},
    {"graph.close", {"Escape"}, tr("Close the graph")},
    {"graph.find", {"/", "Ctrl+F"}, tr("Find a thing in the graph by name")},
    {"graph.filter", {"F"}, tr("Filter the graph: by kind, book, series or world, or only what was asked about")},
    {"graph.list", {"V"}, tr("Switch between the graph as a picture and as a list")},
    {"graph.details", {"I"}, tr("Show or hide the details of the selected thing")},
    {"graph.open", {"O"}, tr("Read the selected book, or go to where the selected thing was first met")},
    {"graph.moments",
     {"R"},
     tr("Go through when what is known of the selected thing was revealed: Tab moves on, Enter goes to that moment in the text")},
    {"graph.further", {"]"}, tr("Show one step further around the centre of the graph")},
    {"graph.nearer", {"["}, tr("Show one step less around the centre of the graph")},
    {"graph.zoom_in", {"+", "="}, tr("Zoom in on the graph")},
    {"graph.zoom_out", {"-"}, tr("Zoom out of the graph")},
    {"graph.fit", {"0"}, tr("Fit the whole graph in the view")},
    {"graph.pan_left", {"Shift+H", "Shift+Left"}, tr("Move the view of the graph left")},
    {"graph.pan_right", {"Shift+L", "Shift+Right"}, tr("Move the view of the graph right")},
    {"graph.pan_up", {"Shift+K", "Shift+Up"}, tr("Move the view of the graph up")},
    {"graph.pan_down", {"Shift+J", "Shift+Down"}, tr("Move the view of the graph down")},
    {"graph.beyond", {"X"}, tr("Show or hide what a book's things connect to in other books")},
    {"graph.add", {"N"}, tr("Add a thing to the graph, related to the selected one")},
    {"graph.edit", {"E"}, tr("Rename the selected thing, or rewrite what is said of it")},
    {"graph.merge", {"Shift+M"}, tr("Fold another thing into the selected one: they are the same")},
    {"graph.remove", {"D", "Delete"}, tr("Remove the selected thing from the graph (asks first)")},
};

// A command is found by its id, which begins with the place it works in.
[[nodiscard]] constexpr bool namedApart(std::span<const BuiltIn> commands) {
    for (std::size_t one = 0; one < commands.size(); ++one) {
        if (!std::ranges::contains(places, placeOf(commands[one].id)))
            return false;
        for (std::size_t other = one + 1; other < commands.size(); ++other)
            if (commands[one].id == commands[other].id)
                return false;
    }
    return true;
}

// A key does one thing where it is pressed: no two commands of one place
// have the same key by default, and none has a key that works everywhere.
// (The graph's keys are its own: it lies over the shelf or beside the page.)
[[nodiscard]] constexpr bool keyedApart(std::span<const BuiltIn> commands) {
    for (std::size_t one = 0; one < commands.size(); ++one)
        for (std::size_t other = one + 1; other < commands.size(); ++other) {
            const std::string_view here = placeOf(commands[one].id), there = placeOf(commands[other].id);
            const bool together = here == there || here == "global" || there == "global";
            const auto shared = [&](std::string_view key) { return std::ranges::contains(keysOf(commands[other]), key); };
            if (together && std::ranges::any_of(keysOf(commands[one]), shared))
                return false;
        }
    return true;
}

static_assert(namedApart(builtIns), "every command has an id of its own, beginning with a place keys work in");
static_assert(keyedApart(builtIns), "two commands of one place have the same default key");

} // namespace CommandTable
