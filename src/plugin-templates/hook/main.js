// Hooks are functions Omabook calls when something happens. This one marks
// a book as read, with today's date, when it is closed on its last page,
// and keeps a list of the books finished that way.
omabook.on("book.closed", (event) => {
    const book = omabook.library.get(event.id);
    const list = finished();
    if (!book || event.percent < 99 || list.some((entry) => entry.id === book.id))
        return;
    const day = new Date().toISOString().slice(0, 10);
    omabook.library.setReading(book.id, {shelf: "read", finished: day});
    // omabook.settings is the plugin's own storage; it survives restarts.
    omabook.settings.set("finished", list.concat([{id: book.id, title: book.title, day: day}]));
    omabook.say("Finished " + book.title + ". That makes " + (list.length + 1) + ".");
});

// A function at the top level can be called from outside:
//   omabook plugin run {{name}} finished
// and a hook can be tried without the window:
//   omabook plugin run {{name}} event:book.closed '{"id": 1, "percent": 100}'
function finished() {
    return omabook.settings.get("finished", []);
}
