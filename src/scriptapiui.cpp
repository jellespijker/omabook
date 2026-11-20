// The part of the plugin API table (scriptapi.cpp) that is the window:
// `omabook.ui`, what a plugin puts there (panels, views, launchers) and what
// it asks there (scriptui.js).

#include "scriptapi.h"

namespace ScriptApi {

QList<Entry> uiEntries() {
    return {
        {"ui.panel", "ui.panel({place, title, width, source, when})", "",
         "Puts a QML file of the plugin's folder in the window: `place` is `library.side` or `reader.side`. With `when: "
         "\"generated\"` it is only there, and only takes room, while a book this plugin made is open or selected. "
         "The panel sees what a view sees (`backend`, `reader`, `theme`, ...) and what the script set as "
         "`backend.scripts.data[\"<plugin>\"]`. It calls the script three ways: "
         "`backend.scripts.call(\"<plugin>\", \"fn\", [args])` answers at once what `fn` returned; "
         "`backend.scripts.request(\"<plugin>\", \"fn\", [args])` answers a number, and the signal "
         "`answered(number, result, error)` comes when `fn`, or the promise it returned, is done; "
         "`backend.scripts.run(\"<plugin>:<command id>\", bookId)` runs a command."},
        {"ui.setVisible", "ui.setVisible(source, visible)", "",
         "Hides one of the plugin's panels, by the file it was given as, or shows it again. A hidden panel takes no room."},
        {"ui.view", "ui.view({id, title, source, key, width})", "",
         "Gives the plugin a view of its own (#77): a QML file of its folder, as a panel's is, shown in place of the "
         "shelf, or beside the page while a book is open, where it is a pane as wide as the reader drags it (`width`, "
         "480 when not given, is the width it comes with). Omabook puts it under a header with the way back, its "
         "`title` and a ⋯ of what can be done in it, for a pointer and a finger, in the theme. It comes with two "
         "commands of its `id`, `library.<plugin>:<id>` and `reader.<plugin>:<id>`, with `key` as their key, which open "
         "the view and put it away again: in the help, keys.json and the shelf's and the page's ⋯. In it, Backspace "
         "goes back (`view.back`) and Escape closes it (`view.close`), and commands of `place: \"view\"` work while it "
         "has the keyboard. The view sees what a panel sees, and `import Omabook` gives it `InkSurface`, the pen's page "
         "(#78). Omabook sets these on its root item, where it declares them: `beside` (true beside the page), `args` "
         "(what `ui.showView` was given; `{}` from a key). It calls `back()` on it, where there is one, before the view "
         "is closed: answering true means it went back within itself; and `takeFocus()` when the view gets the keyboard. "
         "The same `id` again replaces the view."},
        {"ui.showView", "ui.showView(id, args)", "",
         "Shows one of the plugin's views: beside the page while a book is open, in place of the shelf otherwise, as "
         "`ui.showGraph` does. `args` is any JSON, set as the view's `args`. From a launcher's `open`, a plugin's own "
         "item on the shelf opens in its view. Nothing happens where there is no window."},
        {"ui.closeView", "ui.closeView(id)", "", "Puts one of the plugin's views away, wherever it is shown."},
        {"ui.showGraph", "ui.showGraph({scope, book})", "",
         "Opens the knowledge graph in the window: of `book` (a number) when given, else of `scope` (a scope of "
         "`omabook.graph`, a world of the plugin's own say; `\"\"` is the library's). Beside the page while a book is "
         "open, in place of the shelf otherwise. Nothing happens where there is no window."},
        {"ui.set", "ui.set(key, value)", "", "Sets a value the plugin's panels and views are bound to, and that others can read."},
        {"ui.ask", "ui.ask({title, placeholder, text})", "",
         "Asks the reader for words in a small dialog: `title` is the question, `text` what is filled in to start with. "
         "A promise of what they typed, or of `null` when they left it (Escape) or there is no window to ask in (a "
         "command). Questions are put one at a time."},
        {"ui.form", "ui.form({title, intro, accept, fields})", "",
         "Asks the reader to fill in a form, drawn by Omabook in its own style and language, for the keyboard and for "
         "fingers. `intro` is a few words under the title, `accept` the caption of the button that sends it (Done when "
         "not given). Each field is `{id, label, type, help, value, ...}`, where `type` is `text` (one line; "
         "`placeholder`), `textarea` (several), `choice` (one of `choices`), `multi` (any number of `choices`, its value a "
         "list), `number` (with `min`, `max`, `step`) or `toggle` (true or false). A choice is a string, or `{value, "
         "label, help}`. `required: true` keeps the form from being sent while that field is empty, and `when: {other: "
         "value or [values]}` shows a field only while a field above it has one of those values. A promise of `{id: "
         "value}` for every field, each of its type and within its bounds (a hidden field at its starting value), or of "
         "`null` when the reader left it (Escape) or there is no window (a command). With no fields it asks to confirm: "
         "`{}` for yes. A form that can't be drawn (no title, two fields with one id, a choice with nothing to choose) "
         "throws at once. Forms and questions are put "
         "one at a time."},
        {"ui.launcher", "ui.launcher({id, title, author, description, cover, open(context), removed(context)})", "",
         "Puts an entry on the shelf that is not a book: it is shown in every view of the library and found by a search "
         "like a book, and opening it (Space, Enter, a double click or a tap, as for any book) calls `open({launcher})` "
         "instead of the reader. `cover` is a picture in the plugin's folder; without one Omabook draws a cover from the "
         "theme, marked as something to start rather than to read. It is in the window only: not in the library, so "
         "never in the counts, the reading statistics, suggestions, the knowledge graph, tidying, sync, catalogs, the "
         "exports or the file manager, and it goes when the plugin is switched off or removed. When the reader takes it "
         "off the shelf (Remove), `removed({launcher})` is called and it stays off until the script puts it back; keep "
         "that choice in `settings` and offer the way back. The same `id` again replaces it; at most three a plugin."},
        {"ui.removeLauncher", "ui.removeLauncher(id)", "", "Takes one of the plugin's launchers off the shelf."},
    };
}

} // namespace ScriptApi
