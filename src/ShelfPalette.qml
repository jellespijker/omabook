import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One box for arranging the shelf: type a few letters of what to filter by
// (a status, a tag, an author, a format...) or of an order, and Enter takes
// the first match. It stands in for a toolbar of drop-downs.
AppPopup {
    // Not `palette`: every control has a property of that name, which would
    // be found first.
    id: chooser
    objectName: "palette"
    padding: 16
    // What it was opened for: "filter" or "sort", or, from a header too
    // narrow to show them, "view" (the ways to show the shelf) or "more"
    // (what the buttons under fingers do); or "collections" (which to show,
    // and making, changing and sorting them) and "in" (the collections the
    // selected book, `bookId`, is in, each ticked on or off).
    property string mode: "filter"
    property var bookId: 0
    signal acted(string what)
    property var entries: []
    readonly property var matching: {
        const typed = field.text.trim().toLowerCase()
        const wanted = typed.split(" ").filter(word => word !== "")
        const found = entries.filter(entry => {
            const text = (entry.kindName + " " + entry.label).toLowerCase()
            // With nothing typed, the long lists show only their most common.
            return wanted.length === 0 ? entry.common : wanted.every(word => text.includes(word))
        })
        // What was typed in full leads, then what starts with it: "read"
        // means Read before Reading or Unread.
        const rank = entry => entry.label.toLowerCase() === typed ? 0 : entry.label.toLowerCase().startsWith(typed) ? 1 : 2
        return typed === "" ? found
               : found.map((entry, at) => [rank(entry), at, entry]).sort((a, b) => a[0] - b[0] || a[1] - b[1]).map(ranked => ranked[2])
    }

    function show(what, book) {
        mode = what
        bookId = book || 0
        entries = what === "sort" ? orders() : what === "view" ? ways() : what === "more" ? actions()
                  : what === "collections" ? collections() : what === "in" ? bookIn() : filters()
        field.text = ""
        list.currentIndex = 0
        open()
        field.forceActiveFocus()
    }
    function orders() {
        const list = ["recent", "title", "author", "series", "added", "progress", "rating", "published", "finished"].map(
            key => ({ kindName: "", label: words.sort(key), sort: key, on: backend.books.sort === key, common: true, count: 0 }))
        list.push({ kindName: "", label: qsTr("Reverse the order"), reverse: true, on: backend.books.reversed, common: true, count: 0 })
        return list
    }
    function ways() {
        const list = ["covers", "spines", "carousel", "list", "stacks"].map(
            key => ({ kindName: "", label: words.view(key), view: key, on: backend.view === key, common: true, count: 0 }))
        list.push({ kindName: "", label: qsTr("Graph"), graph: true, on: false, common: true, count: 0 })
        return list
    }
    function actions() {
        return [["add", qsTr("Add")], ["filter", qsTr("Filter")], ["collections", qsTr("Collections")], ["suggest", qsTr("Suggest")],
                ["libraries", qsTr("Libraries")], ["settings", qsTr("Settings")]].map(
            action => ({ kindName: "", label: action[1], act: action[0], on: false, common: true, count: 0 }))
    }
    // Which collection to show, then what can be done with them.
    function collections() {
        const list = [{ kindName: "", label: qsTr("All books"), collection: 0, on: backend.collection === 0, common: true, count: 0 }]
        for (const collection of backend.collections)
            list.push({ kindName: "", label: collection.path, collection: collection.id, on: backend.collection === collection.id,
                        common: true, count: collection.count })
        list.push({ kindName: "", label: qsTr("New collection…"), act: "collectionNew", on: false, common: true, count: 0 })
        if (backend.shelfRule() !== "")
            list.push({ kindName: "", label: qsTr("New collection of this search and filters…"), act: "collectionFromShelf",
                        on: false, common: true, count: 0 })
        if (backend.collection !== 0)
            list.push({ kindName: "", label: qsTr("Change %1…").arg(backend.collectionName), act: "collectionEdit",
                        on: false, common: true, count: 0 })
        if (backend.collections.length > 0 && librarian.hasAi)
            list.push({ kindName: "", label: qsTr("Sort books into collections, with AI…"), act: "collectionSort",
                        on: false, common: true, count: 0 })
        return list
    }
    // The collections the book is in, to tick on or off.
    function bookIn() {
        const list = backend.bookCollections(bookId).map(collection => ({
            kindName: "", label: collection.name, inCollection: collection.id, on: collection.in, common: true, count: 0 }))
        list.push({ kindName: "", label: qsTr("New collection…"), act: "collectionNew", on: false, common: true, count: 0 })
        return list
    }
    function filters() {
        const active = backend.books.filters
        const list = []
        if (Object.keys(active).length > 0)
            list.push({ kindName: "", label: qsTr("Clear the filters"), clear: true, on: false, common: true, count: 0 })
        // What narrows the shelf can be kept, as a collection.
        if (backend.shelfRule() !== "")
            list.push({ kindName: "", label: qsTr("Keep this as a collection…"), act: "collectionFromShelf", on: false, common: true, count: 0 })
        else if (backend.collections.length === 0)
            list.push({ kindName: "", label: qsTr("New collection…"), act: "collectionNew", on: false, common: true, count: 0 })
        for (const kind of ["status", "held", "rating", "format", "language", "tag", "author", "series"]) {
            const choices = backend.books.choices(kind)
            for (let i = 0; i < choices.length; ++i) {
                const on = active[kind] !== undefined && String(active[kind]) === String(choices[i].value)
                list.push({ kindName: words.kind(kind), label: words.value(kind, choices[i].value), kind: kind,
                            value: choices[i].value, count: choices[i].count, on: on, common: on || i < 5 })
            }
        }
        // What is filtered on now leads, where it can be taken off again.
        return list.filter(entry => entry.on).concat(list.filter(entry => !entry.on))
    }
    function take(entry) {
        if (entry === undefined)
            return
        if (entry.inCollection !== undefined) {
            // Ticked on or off, and the list stays for the next.
            backend.setInCollection(entry.inCollection, bookId, !entry.on)
            const at = list.currentIndex
            entries = bookIn()
            list.currentIndex = at
            return
        } else if (entry.collection !== undefined) {
            backend.collection = entry.collection
        } else if (entry.view !== undefined) {
            backend.view = entry.view
        } else if (entry.graph) {
            close()
            win.showGraph(0)
            return
        } else if (entry.act !== undefined) {
            close()
            chooser.acted(entry.act)
            return
        } else if (entry.sort !== undefined) {
            backend.books.reversed = false
            backend.books.sort = entry.sort
        } else if (entry.reverse) {
            backend.books.reversed = !backend.books.reversed
        } else if (entry.clear) {
            backend.books.clearFilters()
        } else {
            backend.books.setFilter(entry.kind, entry.on ? "" : entry.value)
        }
        close()
    }

    Words { id: words }

    ColumnLayout {
        spacing: 8
        // A popup is not an Item and doesn't inherit the window's mirroring.
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        TextField {
            id: field
            objectName: "paletteField"
            Layout.preferredWidth: Math.min(400, win.width - 2 * win.dialogEdge - 2 * chooser.padding)
            placeholderText: chooser.mode === "sort" ? qsTr("Order by") : chooser.mode === "view" ? qsTr("Show as")
                             : chooser.mode === "more" ? qsTr("Do") : chooser.mode === "collections" ? qsTr("Collection")
                             : chooser.mode === "in" ? qsTr("In the collections") : qsTr("Filter by")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onTextEdited: list.currentIndex = 0
            Keys.onDownPressed: list.incrementCurrentIndex()
            Keys.onUpPressed: list.decrementCurrentIndex()
            Keys.onReturnPressed: chooser.take(chooser.matching[list.currentIndex])
            Keys.onEnterPressed: chooser.take(chooser.matching[list.currentIndex])
        }
        ListView {
            id: list
            objectName: "paletteList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 360, Math.max(88, win.height - 2 * win.dialogEdge - 2 * chooser.padding - field.height - 40))
            clip: true
            model: chooser.matching
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: entry
                required property int index
                required property var modelData
                width: list.width
                height: win.touch ? 44 : 34

                Rectangle {
                    anchors.fill: parent
                    visible: entry.ListView.isCurrentItem
                    radius: Math.min(4, theme.radius)
                    color: theme.colors.selection
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 10
                    Label {
                        visible: chooser.mode === "filter"
                        Layout.preferredWidth: 84
                        text: entry.modelData.kindName
                        color: Qt.alpha(theme.foreground, 0.6)
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: entry.modelData.label
                        elide: Text.ElideRight
                    }
                    Label {
                        visible: entry.modelData.count > 0
                        text: entry.modelData.count.toLocaleString(Qt.locale(), 'f', 0)
                        color: Qt.alpha(theme.foreground, 0.6)
                        font.pixelSize: 12
                    }
                    Glyph {
                        visible: entry.modelData.on
                        name: "check"
                        size: 14
                    }
                }
                TapHandler { onTapped: chooser.take(entry.modelData) }
            }
        }
        Label {
            visible: list.count === 0
            Layout.fillWidth: true
            text: qsTr("Nothing here goes by that")
            color: theme.colors.muted
        }
    }
}
