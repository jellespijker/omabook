// A command of its own. It gets a key, shows among the shortcuts (?), and
// can be given another key in keys.json: {"library": {"F5": "{{name}}:finished"}}.
omabook.commands.register({
    id: "finished",
    title: "Mark the selected book as read today",
    place: "library",
    key: "Ctrl+R",

    // `context.book` is the number of the selected book (the open one, in
    // the reader), or 0 when there is none.
    run(context) {
        const book = omabook.library.get(context.book);
        if (!book) {
            omabook.say("Select a book first.");
            return;
        }
        omabook.library.setReading(book.id, {shelf: "read", finished: today()});
        omabook.say("Marked " + book.title + " as read.");
    }
});

function today() {
    return new Date().toISOString().slice(0, 10);
}
