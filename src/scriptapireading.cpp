// The part of the plugin API table (scriptapi.cpp) for what a plugin writes
// beside a book being read (#137): the reader's selection, passages of a
// book, and books by the id that is the same on every computer.
// scriptreading.js builds them.

#include "scriptapi.h"

namespace ScriptApi {

QList<Entry> readingEntries() {
    return {
        {"reader.selection", "reader.selection()", "",
         "The words selected in the reader, as a passage of the open book: `{book, global_id, passage, confidence}`, "
         "or `null` when none are. `passage` is what `passages.make` answers, its words placed in the book's own text "
         "nearest where the reader is; `confidence` (0 to 1) is how sure that is. The event `reader.selected` says "
         "when it changes."},
        {"passages.make", "passages.make(book, place)", "",
         "A passage of a book, to refer to and find again later (as Omabook keeps a highlight's): `{from: {section, "
         "fraction}, to, quote, before, after}`, its words (`quote`) with a few just before and after them, and where "
         "they are. `place` is `{section, fraction}`: the words that start there, twelve of them; with `quote`, those "
         "words where they are nearest the place (it throws when they aren't in the book); or a passage, or a "
         "selection as `reader.selection()` answers it. A page without words (a comic's) is its place alone."},
        {"passages.find", "passages.find(book, passage)", "",
         "A passage found again in the book's text as it is now: `{passage, confidence}`, or `null` when too little "
         "of it is there. By its words first, those around them and its place telling one occurrence from another, "
         "nearest its own section; then by most of its words, when a few changed (a new edition, a typo fixed). "
         "`confidence` is 1 when all its words and those around them are there, less as fewer are."},
        {"library.byGlobalId", "library.byGlobalId(id)", "",
         "The book of a global id, as `library.get` answers it without `files`, or `null` when this library has none. "
         "Every book a plugin is told of (`library.list`, `get`, `search`, the events, `reader.current()`) carries "
         "`global_id`, which is the same on every computer the book is on, where its number (`id`) differs: keep that "
         "to name a book in what travels."},
        {"library.sharedIn", "library.sharedIn(id)", "",
         "The spaces a book goes into beyond the person's own computers: `[{space, kind, name, collection}]`, a shared "
         "space (`kind` `\"shared\"`) or a published one (`\"published\"`) whose collection holds it; `[]` when it "
         "stays the person's own. A book of a kind a plugin keeps private is in none unless the person let it go. "
         "What a plugin writes beside a book in one is for others to read: say nothing past where they read to."},
    };
}

} // namespace ScriptApi
