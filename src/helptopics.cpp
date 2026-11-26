#include "manual.h"

// The help's pages, but for the AI's (aitopic.cpp) and the hub's
// (hubtopic.cpp). Their text lives only here and there: the window (?) and
// `omabook help <topic>` both show it.
//
// A command is named in braces, {library.add}, and shown as whichever key
// it has now; never write a key itself. What is in `backticks` is something
// to type in a terminal. A page lists, after its text, the commands it
// named, and in the window those can be run from there. Keep a page to
// what a person needs: the README is where the rest goes.
//
// A page of its own is a function of its own, as the two below are.

namespace {

Manual::Topic computersTopic() {
    return {"computers", Manual::tr("Other computers"), Manual::tr("The same library on every computer you read at."),
            {
                {Manual::tr("Sync"),
                 Manual::tr("Name the other computer in the settings ({global.settings}) and {library.sync} brings the two "
                            "libraries in step over ssh. Each side gets the books it lacks, and for a book both have, the "
                            "later reading place wins, and so do the details changed last. `omabook sync laptop` does the "
                            "same from a terminal.")},
                {Manual::tr("Removed books"),
                 Manual::tr("A book removed on one computer is removed on the other at the next sync too, to its trash, "
                            "unless it was read or changed there since. A file a merge gave to the book it kept stays, and so "
                            "does that book.")},
                {Manual::tr("Before the first sync"),
                 Manual::tr("`omabook sync laptop --dry-run` lists what a sync would bring, send, update and remove on each "
                            "side, and changes nothing. Books from before Omabook kept when their details changed can't say "
                            "whose are newer: where they differ, `--details mine` keeps this computer's and `--details "
                            "theirs` takes the other's.")},
                {Manual::tr("Checked before it writes"),
                 Manual::tr("A sync is planned on both computers first, and refused, with nothing changed on either, when it "
                            "would lose a book the other still has, empty a detail that had a value, or take a reading place "
                            "back to an earlier one. It names the books and why; `omabook sync laptop --accept \"<title>\"` "
                            "lets one book's change through.")},
                {Manual::tr("Clocks, names and older Omabooks"),
                 Manual::tr("Reading places are ordered as they were read, whatever the computers' clocks say. A computer is "
                            "known by the machine it runs on, so one given a new name syncs on, and a copy of its library on "
                            "another machine is refused. An Omabook from before this check leaves its side unchecked, and the "
                            "sync says so; a hub syncs with none.")},
                {Manual::tr("One person on every computer"),
                 Manual::tr("The first sync between two computers pairs them: from then on both sign what they write as you, "
                            "and a change that isn't as it was signed is refused. `omabook identity` shows your id and the "
                            "computers paired.")},
                {Manual::tr("What it needs"),
                 Manual::tr("Omabook on both computers, and ssh that logs in without asking for a password. Tailscale makes "
                            "that work from anywhere.")},
                {Manual::tr("Phones and e-readers"),
                 Manual::tr("{library.devices} serves the library to KOReader, Librera, Moon+ Reader and other apps that read "
                            "a catalog, on this computer, the home network or the tailnet. Scan its QR code with the phone, or "
                            "copy the address, the user name and the password. Its switch serves on with the window closed, as "
                            "a service of your own that starts when you log in. A hub serves them too, with no computer awake: "
                            "Phones… in {library.hubs}.")},
                {Manual::tr("Why not a shared folder"),
                 Manual::tr("A library is a live database, and two computers writing it through a network folder can "
                            "corrupt it. So each computer keeps a whole library of its own, readable offline, and a sync "
                            "brings them in step.")},
                {Manual::tr("What a sync leaves out"),
                 Manual::tr("Bookmarks stay on the computer they were made on.")},
            }};
}

Manual::Topic sharingTopic() {
    return {"sharing", Manual::tr("Sharing"), Manual::tr("Publish a collection, subscribe to one, or keep one together."),
            {
                {Manual::tr("Publishing a collection"),
                 Manual::tr("With a collection shown, {library.publish} publishes it for others to subscribe to. It says "
                            "first what goes in and what you may switch on, then saves an .omabook-space file to give them, or "
                            "sends it as a link: a gist, secret unless you say so. Publish it again after a change, and "
                            "those who follow it get what changed. `omabook collections share <path>` does the same from "
                            "a terminal.")},
                {Manual::tr("What is never shared"),
                 Manual::tr("The books' files go only when the space's owner switches them on. Your graph, questions, "
                            "stars, reading places and reading days go only when you switch each on. Nothing outside the "
                            "collection goes, not the collections it sits in, not where your files lie, and not the names "
                            "of your computers.")},
                {Manual::tr("The books' files"),
                 Manual::tr("The books you own you may share with anyone you allow. Switched on (in the publish dialog, "
                            "`--with files`, or `omabook spaces files <space> on` by a writer of a shared space), they go "
                            "to everyone the space reaches. Each is fetched only when its book is opened, checked against "
                            "its hash, and never in place of a copy of your own. A shared space's folder carries them "
                            "sealed, a published space's file beside it; a gist carries none.")},
                {Manual::tr("Subscribing"),
                 Manual::tr("{library.subscribe} takes such a file, one dropped on the window, or a link. Before anything is "
                            "written it says which of its books you have and which become wishlist entries, in a collection "
                            "of yours. The shared space stays apart, read-only, and is never merged into your library. "
                            "Unsubscribing drops it and keeps your collection.")},
                {Manual::tr("No spoilers"),
                 Manual::tr("Of a shared graph, the one who published it sends only what they have read. You see it beside "
                            "your own, marked as theirs, and only as far as you have read each book yourself: nothing of a "
                            "book you haven't begun, more as you read on.")},
                {Manual::tr("Following"),
                 Manual::tr("A collection subscribed to by its link looks there for a newer version and says what would "
                            "change. Nothing is taken in until you say so. `omabook collections import --follow` does the "
                            "same from a terminal.")},
                {Manual::tr("Shared spaces"),
                 Manual::tr("A household library or a book club is a collection several people write. `omabook spaces "
                            "create` makes one of a collection of yours, `spaces invite` writes an invite only that person "
                            "can open, `spaces join` takes it, and `spaces sync --folder` keeps it in step through a folder "
                            "something else syncs, such as Syncthing. Readers get every change; only writers make them.")},
                {Manual::tr("Removing a member"),
                 Manual::tr("Removing someone from a shared space changes its key. The members who stay get the new one, "
                            "and what the one removed writes after is refused, even when they date it before. A sync "
                            "brings every key the space has had to your other computers, so what was written before stays "
                            "readable there.")},
                {Manual::tr("On all your computers"),
                 Manual::tr("A sync between your own computers carries the shared spaces you are in and the collections you "
                            "subscribed to, sealed to you: subscribe or join on one, and the next sync brings it to the "
                            "others. Unsubscribing on one unsubscribes them all.")},
                {Manual::tr("Who you are"),
                 Manual::tr("`omabook identity --export` prints your id: give it to someone who wants to invite you to a "
                            "shared space.")},
            }};
}

Manual::Topic themesTopic() {
    return {"themes", Manual::tr("Colors and themes"), Manual::tr("Look like Omarchy anywhere, follow Aether, or pick a theme."),
            {
                {Manual::tr("Automatic"),
                 Manual::tr("Omabook takes its colors from Omarchy's current theme and follows it as it changes. Without "
                            "Omarchy it follows the theme Aether writes, and without either it looks like Omarchy's "
                            "default, Tokyo Night.")},
                {Manual::tr("Choosing a theme"),
                 Manual::tr("In the settings ({global.settings}), Choose Theme lists Omarchy's own themes, built into "
                            "Omabook, with Omarchy and Aether when they are here. The arrow keys show each one on the "
                            "window as you go; Enter keeps it and Escape puts back what there was. It changes Omabook "
                            "only, not the desktop.")},
                {Manual::tr("From a terminal"),
                 Manual::tr("`omabook theme list` shows the themes, `omabook theme use nord` picks one, and `omabook theme "
                            "use ~/colors.toml` takes any colors.toml. `omabook theme import <name>` makes a theme of your "
                            "own from an Aether blueprint.")},
                {Manual::tr("Your own colors"),
                 Manual::tr("Colors in `~/.config/omabook/style.json` stay over whichever theme is chosen, so a theme can "
                            "be the start and one color your own.")},
            }};
}

Manual::Topic chronologyTopic() {
    return {"chronology",
            Manual::tr("The chronology"),
            Manual::tr("When the events of your books happen in their stories."),
            {
                {Manual::tr("Story time"),
                 Manual::tr("The chronology puts the events you read in the order they happen in their world, not the "
                            "order you read them in: a flashback lands where it belongs. {library.chronology_book} shows "
                            "the selected book's, {reader.chronology} the open book's, and {graph.chronology} that of what "
                            "the graph shows. It is not When it was revealed in the graph, which is the order you learned "
                            "things in.")},
                {Manual::tr("Never ahead of you"),
                 Manual::tr("Every time and every order is kept with the place in the book where it was told, so the "
                            "chronology holds only what you have read, also on a timeline your book shares with another.")},
                {Manual::tr("Timelines"),
                 Manual::tr("Each lane is a timeline: a book's own, a series', a world's or real history, under the ruler "
                            "of its calendar. Lanes that line up share one axis. A real date puts an event on real history, "
                            "which is how books with nothing else in common meet.")},
                {Manual::tr("Timelines that branch"),
                 Manual::tr("A timeline can branch off another at an event: the same up to it, its own after it, as "
                            "alternate history is of real history. Its lane begins where it splits off, beside the one it "
                            "branches off, and shares what came before. When the AI reads one, it only proposes it: take "
                            "it or not over the lanes. By hand, `omabook chronology timeline branch <name> --from "
                            "<timeline> --at <event>`.")},
                {Manual::tr("Where the times come from"),
                 Manual::tr("As the AI builds a book's graph, it notes when events happen, as far as the book says, and "
                            "what comes before what; it never guesses a date. `omabook graph build <book> --again` does "
                            "it for a book built before. By hand, `omabook chronology set-when`, `order`, `place` and "
                            "`timeline add`, which win over what the books say.")},
                {Manual::tr("Unplaced, and contradictions"),
                 Manual::tr("{chronology.unplaced} shows the events no time or order is known of. An event in a "
                            "contradiction is ringed, and its card says what was said where and which was followed.")},
                {Manual::tr("The card"),
                 Manual::tr("A click or a tap on an event shows what is known of it. {chronology.page} goes back to the "
                            "page it was revealed on, and {chronology.graph} shows it in the graph.")},
                {Manual::tr("Filters and moving around"),
                 Manual::tr("{chronology.filter} filters by timeline, book, series, collection, character, place, group or "
                            "theme; {chronology.range} keeps a stretch of time, and {chronology.filter_clear} takes them "
                            "off. {chronology.zoom_in} and {chronology.zoom_out} zoom, {chronology.fit} fits it all; the "
                            "wheel, a drag and a pinch do the same.")},
                {Manual::tr("From a terminal"),
                 Manual::tr("`omabook chronology <book>` prints it, `--format csv`, `graphml` or `dot` writes it for other "
                            "tools, and the MCP tool `chronology` gives it to an agent.")},
            }};
}

// The margin folded away (#167), notes and highlights on the pages of a PDF
// or a comic, and finding what was written by its words (#115).
QList<Manual::Section> pageNoteSections() {
    return {
        {Manual::tr("Folding the margin away"),
         Manual::tr("{reader.fold_margin}, the small button at the margin's edge, or the Aa panel folds the margin notes "
                    "away for every book, and the text takes their room. A dot beside each line with notes, ink in the "
                    "margin or others' notes stands for them, in their colour, with a count where there are several: a "
                    "click, a tap or the pen opens the first on a card beside its words, and Tab and Enter do it from "
                    "the keyboard. The same dots stand beside the lines where the window has no room for the margin.")},
        {Manual::tr("Notes and highlights on pages"),
         Manual::tr("On a PDF's page, select words as in any book to highlight them or write a margin note on them. A "
                    "comic's pages are pictures: drag over a panel with the mouse, press and hold it with a finger, or press "
                    "{reader.select} and move the part with the arrows (Shift makes it larger or smaller), then choose a "
                    "colour or Write a margin note. A mark on the page opens its note.")},
        {Manual::tr("Searching your notes"),
         Manual::tr("{reader.find} finds the words in your highlights, margin notes and bookmarks too, listed before the "
                    "book's own; start with `notes:` to look only there. In the library, `notes:` finds the books you wrote "
                    "the words in. Where a model reads your handwriting (recognition, in the settings), ink on the pages is "
                    "found by its words as well; it goes only to your own servers.")},
    };
}

// Reading a book: turning pages, finding one's place, margin notes, the
// graph in the text, and making the page comfortable.
Manual::Topic readingBasics() {
    return {"reading",
            Manual::tr("Reading"),
            Manual::tr("Turn pages, find your place, and make the page comfortable."),
            {
                {Manual::tr("Turning pages"),
                 Manual::tr("{reader.page_down} turns the page, or scrolls a screen on, and carries on into the next chapter; "
                            "{reader.page_up} goes back. Nothing is shown twice: the next screen starts at the first line that wasn't "
                            "wholly in sight. {reader.next} goes to the next chapter; {reader.previous} to the start of this one, and "
                            "from there to the one before. {reader.line_down} and {reader.line_up} scroll a few lines. Where the "
                            "window has room, two or three pages stand side by side and a turn moves on by all of them; otherwise one "
                            "page scrolls.")},
                {Manual::tr("Pages or scroll"),
                 Manual::tr("{reader.layout} chooses how a book is read through, also under Layout in the Aa panel. Pages: even one "
                            "column is cut into pages, each chapter starts on a page of its own, and turning on from its last page "
                            "opens the next chapter; a PDF or comic page is shown whole, and two side by side open as a book does, "
                            "page 1 alone (First page alone). Scroll: one column that runs on, with a line at each chapter's end "
                            "saying what is next; scroll on past it, or tap it, to go on. Automatic: pages where two fit side by "
                            "side, else a column that scrolls. Your place stays where it is when you change.")},
                {Manual::tr("How many pages"),
                 Manual::tr("A line is as long as reads well: 66 characters unless you set it otherwise, in the font and size you "
                            "read in. As many pages stand side by side as each keep most of that, up to three, and none with lines "
                            "shorter than 45 characters; a single page doesn't grow past it, the rest is margin. {reader.pages} "
                            "goes from as many as fit to one, two or three pages and round again; a choice that doesn't fit shows "
                            "the most that do. A pane beside the page leaves less room, as a narrower window does.")},
                {Manual::tr("Finding your place"),
                 Manual::tr("{reader.contents} shows the chapters beside the page, and {reader.find} finds words in the book. "
                            "{reader.bookmark} keeps a bookmark. Your place is remembered when you leave with {reader.back}.")},
                {Manual::tr("Margin notes"),
                 Manual::tr("Select words and write a margin note on them: {reader.note}, or Write a margin note in the menu beside "
                            "the selection. Where the window has room, notes stand in a margin column beside the text, each at the "
                            "height of its words, and are edited where they stand; in a narrower window a mark where the words end "
                            "opens the note. {reader.notes} goes through the notes on the page: Enter edits one, Delete removes it, "
                            "and a note left without words is removed. Notes go with the book to your other computers, and stay "
                            "with their words in a new edition; those whose words are gone are listed at the foot of the page. "
                            "With a pen, write on the text or in the margin column for a note by hand on that line; the pen's "
                            "other end erases, and a click on the note gives undo, the eraser and its words, which your own AI "
                            "server reads from the handwriting so {reader.find} finds them.")},
                {Manual::tr("Ink on the pages"),
                 Manual::tr("On a PDF's or a comic's pages, and in the margins and between the lines of other books, {reader.ink} "
                            "writes and draws with a pen, a finger or the mouse: the bar above the page picks the pen, a highlighter, "
                            "the eraser or a lasso, the ink, and whether a finger writes (until a pen is used, always, never). Two "
                            "fingers scroll; the pen's other end erases. In other books each stroke stays beside its line however the "
                            "text is laid out. It goes to your other computers and is under Highlights ({reader.annotations}); Ctrl+Z "
                            "undoes it, {reader.show_ink} puts it away.")},
                {Manual::tr("The graph in the text"),
                 Manual::tr("The people, places and things the map ({reader.graph}) knows so far are marked in the text in the colors "
                            "they have there, always, unless you switch it off: {reader.marks} marks them only while the map is "
                            "beside the page, or never, and the choice is kept. Point at "
                            "one, or tap it, for what is known of it. Select words (drag, double-click, press and hold, or "
                            "{reader.select} and the arrow keys) for a menu: ask about them, who or what they are, show them in the "
                            "map, find them in the book, or a question worth asking.")},
                {Manual::tr("Moments, and no spoilers"),
                 Manual::tr("The map's details say when each thing known of someone was revealed, and each is a way back to that "
                            "moment in the text ({graph.moments} from the keyboard), where its words are lit. The card on the page goes to "
                            "where a thing was first met, "
                            "and to where what is said of it now was told. What happens past the furthest you have read stays "
                            "closed under Later in the book: it opens for that one thing only, once you confirm, until Omabook "
                            "closes. A book marked as read has nothing to hide.")},
                {Manual::tr("Making it comfortable"),
                 Manual::tr("{reader.larger} and {reader.smaller} change the size of the type. {reader.expand} shows the book and "
                            "nothing else, on the whole screen.")},
                {Manual::tr("Setting the text your way"),
                 Manual::tr(
                     "{reader.format} opens the Aa panel beside the page (Aa in the bar, with fingers): the font, its size, "
                     "the longest line in characters, the pages side by side, the space between lines and between paragraphs, a first-line "
                     "indent, the margins, "
                     "and the alignment: start, justified (filled out to both sides), centred or end. Each change shows at "
                     "once and the page stays where you were. With Only this book on, the change is for this book alone; "
                     "Reset puts back what there was when the panel opened. The defaults for every book are in the "
                     "settings ({global.settings}) too. Pages that are pictures, as in a PDF or a comic, can't be set. Their layout can.")},
                {Manual::tr("With fingers"),
                 Manual::tr("Swipe sideways or tap the left or right edge to turn the page, pinch to resize, and tap the middle of "
                            "the page for buttons to everything else.")},
                {Manual::tr("What it reads"),
                 Manual::tr("EPUB, FictionBook, Markdown, HTML and plain text flow in your own type. PDF and comics are shown page by "
                            "page. Kindle, Word and the other formats Calibre knows are converted the first time you open them, "
                            "which needs Calibre installed.")},
            }};
}

// Listening to a book (#73).
QList<Manual::Section> readAloudSections() {
    return {
        {Manual::tr("Reading aloud"),
         Manual::tr("{reader.read_aloud} reads the book aloud from the top of the page (Read aloud in the bar, with fingers), and "
                    "pauses and goes on. The sentence being read is marked, the page follows it and the next chapter opens by "
                    "itself, so your place is kept as you listen. {reader.sentence_back} and {reader.sentence_next} go a sentence "
                    "back or on, and {reader.read_aloud_stop} stops. How fast, and in which voice, is in the Aa panel "
                    "({reader.format}).")},
        {Manual::tr("The voice"),
         Manual::tr("Choose what reads in the settings ({global.settings}), on the AI page, in the row Text to speech: Piper "
                    "(piper-tts and a voice for your language, such as piper-voices-en-us) or speech-dispatcher on this computer, "
                    "a server of your own, or OpenAI's or Gemini's voices with their API key. Each keeps its own voice and speed, "
                    "and Hear It reads a sentence with them. Choosing another while a book is read goes on from the same "
                    "sentence. A server is never asked while the AI is off, nor a hosted voice unless you allow a hosted service "
                    "for text to speech; a voice on this computer reads with the AI off too.")},
        {Manual::tr("A plugin reading aloud"),
         Manual::tr("A plugin can read something to you in the same voice. The bar over the page, or over the shelf, says "
                    "which plugin reads, and {reader.read_aloud_stop} or the bar's ✕ stops it.")},
        // Saying what to do (#150).
        {Manual::tr("Saying what to do"),
         Manual::tr("Hold {reader.voice} while you say it, or press it once to start and again to stop (Speak in the bar, with "
                    "fingers or the pen): “next chapter”, “previous page”, “read aloud”, “stop”, “go to chapter 5”, “highlight "
                    "this” for the selected words, “what does this word mean” or “what does serendipity mean”, and most of what "
                    "the keys do, in any of the window's languages. It listens only once you press it, the top of the window "
                    "says that it listens, and Escape lets go of what was said. What the words don't say plainly is asked of "
                    "the AI for quick work, sent nothing but those words; nothing listens with the AI off.")},
    };
}

// The command line, the MCP server, the skill and the interface as files.
Manual::Topic agentsTopic() {
    return {"agents", Manual::tr("For agents"), Manual::tr("The command line, the MCP server, and the interface as files."),
            {
                {Manual::tr("The command line"),
                 Manual::tr("Everything the window does with the library is a command that needs no display: `omabook list`, "
                            "`omabook add`, `omabook text`, `omabook lookup` and the rest. Each takes `--json` and exits 0 or 1. "
                            "`omabook --help` lists them, and `omabook help <topic>` prints these pages.")},
                {Manual::tr("The MCP server"),
                 Manual::tr("`omabook mcp install` connects a coding agent to the library as tools: search, read, fix details, "
                            "track reading, tidy up. `omabook mcp --tools` lists them.")},
                {Manual::tr("The skill"),
                 Manual::tr("`omabook skill install` teaches your coding agents the commands.")},
                {Manual::tr("Keys, colors and views of your own"),
                 Manual::tr("The interface is files in `~/.config/omabook/`, followed while Omabook runs: `keys.json` for any key, "
                            "`style.json` for colors and corners, `ui/` for views of your own and `mods/` for panels. "
                            "`omabook customize` writes a guide to all of it there. A file with a mistake never takes the window "
                            "down: the last version that worked stays in use.")},
            }};
}

Manual::Topic readingTopic() {
    Manual::Topic topic = readingBasics();
    topic.sections << pageNoteSections() << readAloudSections();
    return topic;
}

} // namespace

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
        readingTopic(),
        aiTopic(),
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
             {tr("Collections and the knowledge graph"),
              tr("Each collection at the top of the library has a knowledge graph of its own, with those inside it, and "
                 "the books in no collection have one more, Unfiled: nothing of one is shown, linked or told to the AI in "
                 "another. A book in two top collections is in both, and a book moved takes what it told along. The graph "
                 "shows one at a time: its name at the top lists the others, as {graph.tenant} does, and {graph.tenant_next} "
                 "and {graph.tenant_previous} go to the next and the previous; the chronology follows. For one graph of the "
                 "whole library, tick “Knowledge graph: one for the whole library, not one per top-level collection” in the settings ({global.settings}).")},
             {tr("Search"),
              tr("{library.search} looks everywhere. Start a word with `author:`, `title:`, `series:`, `tag:`, `genre:`, "
                 "`format:`, `lang:`, `publisher:` or `about:` to look in one field, and type `is:paper` or `is:wishlist` "
                 "for the books that have no file. \"Quotes\" keep words together, -word leaves out, and OR joins "
                 "alternatives.")},
             {tr("Search by what you know"),
              tr("A word also finds a book by the characters, places, ideas, threads and topics you met in it, and the questions you "
                 "asked of it, as far as you have read: never a name from further on, and nothing of a book you haven't begun. `knows:` "
                 "looks only there. Its tile says briefly what found the book, marked when of a graph someone shared, and the details "
                 "say it in full; {library.found_graph} shows it in the graph and {library.found_moment} goes to where you learned it.")},
             {tr("Ask the AI to find books"),
              tr("For what a search can't put, such as “the books where a mentor dies”, start the search with ?, press "
                 "{library.search_ai} or the AI button in the search field. The AI reads your books' details and what you "
                 "know of them, never their text, and answers with books and why. It keeps to the collection shown, can be "
                 "stopped, and a question asked again of an unchanged library is answered from what it said before.")},
             {tr("Details"),
              tr("{library.edit} edits a book's details, your stars and whether you have read it. {library.lookup} looks "
                 "the details up online. {library.details} shows or hides them beside the shelf.")},
             {tr("Recordings"),
              tr("An MP3, M4B, M4A, Ogg, Opus, FLAC or WAV in the library is read once it is written out: "
                 "{library.transcribe}, or Write Out in its details, does it in the background through speech to text, and "
                 "stopped, it goes on from where it was. Its transcript reads in chapters of ten minutes, and the search "
                 "finds what is said in it.")},
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
        chronologyTopic(),
        computersTopic(),
        hubTopic(),
        sharingTopic(),
        themesTopic(),
        agentsTopic(),
    };
}
