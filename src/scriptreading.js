// What a plugin writes beside a book being read (#137), as its script sees
// it, built by ScriptReading (scriptreading.cpp) from its native side, which
// is an argument here and never reachable from the script: `omabook.reader`
// with its selection, `omabook.passages`, and of `omabook.library`
// `byGlobalId` and `sharedIn`. scriptapireading.cpp describes it.
(function (native) {
    "use strict";

    function aBook(book) {
        var id = Number(book && typeof book === "object" ? book.id : book);
        if (!(id > 0))
            throw new TypeError("A book is named by its number (book.id), such as 12.");
        return id;
    }

    // Given the plugin's host, for where the reader is, and the prelude's parse.
    return function (host, parse) { return {
        reader: {
            current: function () { return parse(host.reading()); },
            open: function (id, place) {
                var at = place && place.section !== undefined;
                host.openBook(Number(id) || 0, at ? Number(place.section) || 0 : -1, at ? Number(place.fraction) || 0 : 0);
            },
            reload: function () { host.reloadBook(); },
            selection: function () { return parse(native.selection()); }
        },
        passages: Object.freeze({
            make: function (book, place) {
                if (place === null || typeof place !== "object")
                    throw new TypeError("A passage is made at a place, {section, fraction} with the words chosen there (quote) "
                                        + "or not, or of a selection, as reader.selection() answers it.");
                return parse(native.make(aBook(book), JSON.stringify(place)));
            },
            find: function (book, passage) {
                if (passage === null || typeof passage !== "object" || typeof passage.from !== "object")
                    throw new TypeError("find takes a passage as passages.make answers it: {from, to, quote, before, after}.");
                return parse(native.find(aBook(book), JSON.stringify(passage)));
            }
        }),
        library: {
            byGlobalId: function (id) { return parse(native.byGlobalId(String(id || ""))); },
            sharedIn: function (book) { return parse(native.sharedIn(aBook(book))); }
        }
    }; };
})
