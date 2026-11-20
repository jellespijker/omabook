import QtQuick

// Where the chronology (#44) is opened from and shown: in place of the
// shelf, the graph or the page, of the selected book (from the shelf), of
// the book being read (from the page), or of whatever the graph shows. It
// is built the first time it is asked for, and what it was opened over is
// there again when it closes.
Item {
    id: layer
    readonly property bool shown: loader.active && loader.item && loader.item.visible
    readonly property bool popupOpen: shown && loader.item.popupOpen
    readonly property alias pane: loader.item
    function takeFocus() { if (shown) loader.item.takeFocus() }
    // "book" with a book's number, "series" or "collection" with a name,
    // "scope" with a graph's ("" for everything read); in the tenant shown
    // (GraphTenants), or for a book one of its own, which is then the one shown.
    function show(what, value) {
        loader.active = true
        const chronology = loader.item
        chronology.visible = true
        chronology.browser.tenant = what === "book" ? graphTenants.tenantOf(value) : graphTenants.current
        graphTenants.current = chronology.browser.tenant
        if (what === "book") chronology.browser.showBook(value)
        else if (what === "series") chronology.browser.showSeries(value)
        else if (what === "collection") chronology.browser.showCollection(value)
        else chronology.browser.showScope(value || "")
        chronology.fitSoon()
        chronology.takeFocus()
    }
    // Of what the graph shows, taken from the graph itself.
    function showGraphs(graph) {
        if (graph.showing === "book") show("book", graph.bookId)
        else if (graph.showing === "series" || graph.showing === "collection")
            show(graph.showing, graph.showing === "series" ? graph.series : graph.collection)
        else show("scope", graph.scope)
    }

    Key {
        command: "library.chronology_book"
        when: library.visible && library.selectedId > 0
        onActivated: layer.show("book", library.selectedId)
    }
    Key {
        command: "reader.chronology"
        when: win.inReader && reader.open && !layer.shown
        onActivated: layer.show("book", reader.bookId)
    }
    Key {
        command: "graph.chronology"
        when: win.graphShown && !win.inReader && !layer.shown && !!graphLoader.item
        onActivated: layer.showGraphs(graphLoader.item.browser)
    }

    Loader {
        id: loader
        anchors.fill: parent
        active: false
        sourceComponent: ChronologyPane {
            objectName: "chronology"
            visible: false
            active: visible && win.keys
            onDone: {
                visible = false
                win.restoreFocus()
            }
        }
    }
}
