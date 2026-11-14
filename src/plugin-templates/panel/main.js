// A panel of the plugin's own beside the shelf. The script decides what it
// shows (omabook.ui.set); Panel.qml, next to this file, draws it.
omabook.ui.panel({place: "library.side", title: "Reading now", width: 260, source: "Panel.qml"});

function refresh() {
    const reading = omabook.library.search("status:reading").map((book) => {
        return {id: book.id, title: book.title, percent: Math.round(book.percent_read)};
    });
    // What is set here is backend.scripts.data["{{name}}"].reading in the panel.
    omabook.ui.set("reading", reading);
}

for (const event of ["book.added", "book.removed", "book.changed", "book.closed", "library.switched"])
    omabook.on(event, refresh);
refresh();
