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
inline constexpr std::array<std::string_view, 6> places{"global", "library", "reader", "graph", "chronology", "view"};

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

inline constexpr auto builtIns = std::to_array<BuiltIn>({
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
    {"library.transcribe", {"Shift+E"}, tr("Write out the selected recording through speech to text, or stop writing it out")},
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
    {"library.stats", {"Shift+R"}, tr("Show what you read, added up: the time, the days in a row, the books finished, and the year's goal")},
    {"library.ask", {"W"}, tr("Ask the AI about the library")},
    {"library.suggest", {"N"}, tr("Have the AI suggest what to read next")},
    {"library.graph", {"6"}, tr("Show the library as a graph of what is known: books, people, places, ideas and how they relate")},
    {"library.graph_book", {"U"}, tr("Show what is known of the selected book as a graph")},
    {"library.graph_build", {"Shift+U"}, tr("Build the knowledge graph for books you have already read")},
    {"library.chronology_book", {"T"}, tr("Show when the selected book's events happen, on their timelines, as far as you have read it")},
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
    {"reader.find", {"/", "Ctrl+F"}, tr("Find words in the book and in your notes and highlights")},
    {"reader.bookmark", {"B"}, tr("Bookmark this place")},
    {"reader.annotations", {"Shift+B"}, tr("Show or hide the book's highlights beside the page, by colour, the ones no longer found too")},
    {"reader.ask", {"A"}, tr("Ask the AI about what is being read")},
    {"reader.catch_up", {"C"}, tr("Have the AI catch you up")},
    {"reader.cast", {"W"}, tr("Have the AI say who is who so far")},
    {"reader.expand", {"F", "F11"}, tr("The book and nothing else, on the whole screen")},
    {"reader.graph", {"M"}, tr("Show or hide the map of what the book has told so far, beside the page")},
    {"reader.marks", {"Shift+M"}, tr("Mark the graph's people, places and things in the text: while the map is open, always, or never")},
    {"reader.select",
     {"V"},
     tr("Select words with the keyboard, or a part of a comic's page: the arrows or H J K L move, Shift widens, Enter shows what to do "
        "with them")},
    {"reader.read_aloud", {"R"}, tr("Read the book aloud from the top of the page, pause, and go on")},
    {"reader.read_aloud_stop", {"Shift+R"}, tr("Stop reading aloud")},
    {"reader.sentence_back", {"["}, tr("While reading aloud, go a sentence back")},
    {"reader.sentence_next", {"]"}, tr("While reading aloud, go a sentence on")},
    {"reader.voice",
     {"Ctrl+M"},
     tr("Say what to do, such as “next chapter” or “go to chapter 5”: hold it while you speak, or press it once to start and again to "
        "stop")},
    {"reader.note", {"Shift+N"}, tr("Write a margin note on the selected words or part of the page, or select them first")},
    {"reader.notes", {"E"}, tr("Go through the margin notes on the page: Enter edits one, Delete removes it")},
    {"reader.fold_margin", {"Shift+E"}, tr("Fold the margin notes away, a dot beside each line that has one, or unfold them")},
    {"reader.ink", {"D"}, tr("Write and draw, with the pen or the mouse, on the pages or in the margins and between the lines, and stop")},
    {"reader.show_ink", {"Shift+D"}, tr("Show or put away the ink on the pages, to read them as they were")},
    {"reader.format",
     {"Shift+A"},
     tr("Show or hide how the text is set: the font, its size, the longest line, the pages side by side, the spacing, the margins and the "
        "alignment, for this book or every book")},
    {"reader.pages", {"Shift+P"}, tr("Pages side by side: as many as fit, one, two or three, and round again")},
    {"reader.layout",
     {"Shift+L"},
     tr("Read as pages that turn, as one column that scrolls on through the chapters, or automatically, and round again")},
    {"reader.chronology", {"Shift+T"}, tr("Show when the book's events happen, on their timelines, as far as you have read")},
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
    {"graph.chronology", {"T"}, tr("Show when the events of what the graph shows happen, on their timelines")},
    {"graph.tenant", {"C"}, tr("Choose whose graph is shown: one top-level collection's, or that of the books in none")},
    {"graph.tenant_next", {"PgDown"}, tr("Show the graph of the next top-level collection")},
    {"graph.tenant_previous", {"PgUp"}, tr("Show the graph of the previous top-level collection")},
    // The chronology: when the events happen in their stories, in place of
    // the shelf, the graph or the page.
    {"chronology.close", {"Escape"}, tr("Close the chronology")},
    {"chronology.left", {"H", "Left"}, tr("Select the event before on its lane")},
    {"chronology.right", {"L", "Right"}, tr("Select the event after on its lane")},
    {"chronology.up", {"K", "Up"}, tr("Select the nearest event on the lane above")},
    {"chronology.down", {"J", "Down"}, tr("Select the nearest event on the lane below")},
    {"chronology.zoom_in", {"+", "="}, tr("Zoom in on the chronology, down to minutes")},
    {"chronology.zoom_out", {"-"}, tr("Zoom out of the chronology, up to centuries")},
    {"chronology.fit", {"0"}, tr("Fit the whole chronology in the view")},
    {"chronology.pan_left", {"Shift+H", "Shift+Left"}, tr("Move the view of the chronology back in time")},
    {"chronology.pan_right", {"Shift+L", "Shift+Right"}, tr("Move the view of the chronology on in time")},
    {"chronology.pan_up", {"Shift+K", "Shift+Up"}, tr("Move the view of the chronology up the lanes")},
    {"chronology.pan_down", {"Shift+J", "Shift+Down"}, tr("Move the view of the chronology down the lanes")},
    {"chronology.find", {"/", "Ctrl+F"}, tr("Find an event in the chronology by name")},
    {"chronology.filter",
     {"F"},
     tr("Filter the chronology: by timeline, book, series, collection, character, place, group or theme, or only what was met in a book")},
    {"chronology.range", {"R"}, tr("Show only a stretch of story time, from one year or date to another")},
    {"chronology.filter_clear", {"Shift+F"}, tr("Take every filter off the chronology")},
    {"chronology.unplaced", {"U"}, tr("Show or hide the events of which no time or order is known")},
    {"chronology.details", {"I"}, tr("Show or hide what is known of the selected event")},
    {"chronology.page", {"O", "Return", "Enter"}, tr("Go back to the page where the selected event was revealed")},
    {"chronology.graph", {"G"}, tr("Show the selected event in the graph")},
    {"chronology.tenant", {"C"}, tr("Choose whose chronology is shown: one top-level collection's, or that of the books in none")},
    {"chronology.tenant_next", {"PgDown"}, tr("Show the chronology of the next top-level collection")},
    {"chronology.tenant_previous", {"PgUp"}, tr("Show the chronology of the previous top-level collection")},
    // A plugin's own view (#77), in place of the shelf or beside the page:
    // its keys are these and the plugin's commands for its view.
    {"view.back", {"Backspace"}, tr("Go back in a plugin's view, or close it when it has nowhere to go back to")},
    {"view.close", {"Escape"}, tr("Close a plugin's view")},
});

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
// (The graph's keys are its own: it lies over the shelf or beside the page;
// so are the chronology's, in place of the shelf, the graph or the page, and
// a plugin's view's.)
// Each command's place is found once, as its number in `places` (global is
// the first), not once for every pair: clang's constant evaluation stops at
// a number of steps a table this long reaches otherwise.
template <std::size_t Count> [[nodiscard]] constexpr bool keyedApart(const std::array<BuiltIn, Count> &commands) {
    std::array<std::size_t, Count> placed{};
    for (std::size_t at = 0; at < Count; ++at)
        placed.at(at) = static_cast<std::size_t>(std::ranges::find(places, placeOf(commands.at(at).id)) - places.begin());
    for (std::size_t one = 0; one < Count; ++one)
        for (std::size_t other = one + 1; other < Count; ++other) {
            if (placed.at(one) != placed.at(other) && placed.at(one) != 0 && placed.at(other) != 0)
                continue;
            for (const std::string_view key : commands.at(one).keys)
                if (!key.empty() && std::ranges::contains(commands.at(other).keys, key))
                    return false;
        }
    return true;
}

static_assert(namedApart(builtIns), "every command has an id of its own, beginning with a place keys work in");
static_assert(keyedApart(builtIns), "two commands of one place have the same default key");

} // namespace CommandTable
