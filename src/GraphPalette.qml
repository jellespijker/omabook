import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One box for getting around the graph, as the shelf has one: type a few
// letters of a thing's name to go to it ("find"), to say which thing is the
// same as the selected one ("merge"), or of a kind, a book, a series or a
// world to narrow what is shown ("filter"). Enter takes the first match.
AppPopup {
    id: chooser
    objectName: "graphPalette"
    padding: 16
    property var browser
    // "find", "merge" or "filter".
    property string mode: "find"
    // Beside the page only the open book's graph is shown, so there is no
    // other book or world to switch to.
    property bool reading: false
    property var entries: []
    // The node to leave out when merging: the one the others fold into.
    property var except: 0
    signal picked(var entry)
    // What was picked to merge, said once the box has closed: a question
    // asked while it is still closing loses the keyboard to it.
    property var chosen: null
    onClosed: {
        if (!chosen) return
        const entry = chosen
        chosen = null
        chooser.picked(entry)
    }

    readonly property var matching: {
        const typed = field.text.trim().toLowerCase()
        if (mode !== "filter") {
            chooser.browser.rows
            return chooser.browser.find(typed).filter(node => node.id !== chooser.except)
                          .map(node => ({ id: node.id, kind: node.kind, label: node.name, detail: node.summary }))
        }
        const wanted = typed.split(" ").filter(word => word !== "")
        return entries.filter(entry => {
            const text = (entry.kindName + " " + entry.label).toLowerCase()
            // With nothing typed, the long lists show only their first few.
            return wanted.length === 0 ? entry.common : wanted.every(word => text.includes(word))
        })
    }

    function show(what, except) {
        mode = what
        chooser.except = except || 0
        entries = what === "filter" ? filters() : []
        field.text = ""
        list.currentIndex = 0
        open()
        field.forceActiveFocus()
    }
    function filters() {
        const list = []
        const hidden = browser.kinds.some(kind => !kind.shown)
        if (hidden || browser.onlyAsked)
            list.push({ kindName: "", label: qsTr("Show everything again"), act: "clear", on: false, common: true, count: 0 })
        list.push({ kindName: "", label: qsTr("Only what I have asked about"), act: "asked", on: browser.onlyAsked, common: true, count: 0 })
        if (browser.showing === "book" && browser.beyondCount > 0)
            list.push({ kindName: "", label: qsTr("What it connects to in other books"), act: "beyond", on: browser.beyond, common: true,
                        count: browser.beyondCount })
        if (!reading && browser.showing === "scope" && browser.scope === "")
            list.push({ kindName: "", label: qsTr("Only connections between books"), act: "across", on: browser.onlyAcross, common: true,
                        count: 0 })
        for (const kind of browser.kinds)
            list.push({ kindName: qsTr("Kind"), label: words.kinds(kind.kind), act: "kind", value: kind.kind, on: kind.shown,
                        common: true, count: kind.count, mark: kind.kind })
        for (let depth = 0; depth <= 4; ++depth)
            list.push({ kindName: qsTr("Reach"), label: words.reach(depth), act: "depth", value: depth, on: browser.depth === depth,
                        common: depth < 3, count: 0 })
        if (reading)
            return list
        for (const scope of browser.scopes)
            list.push({ kindName: qsTr("Graph"), label: words.scope(scope.scope), act: "scope", value: scope.scope,
                        on: browser.showing === "scope" && browser.scope === scope.scope, common: true, count: 0 })
        const books = browser.books
        const series = {}
        for (let i = 0; i < books.length; ++i) {
            list.push({ kindName: qsTr("Book"), label: books[i].title, act: "book", value: books[i].bookId,
                        on: browser.bookId === books[i].bookId, common: i < 5, count: books[i].things })
            if (books[i].series !== "")
                series[books[i].series] = (series[books[i].series] || 0) + books[i].things
        }
        for (const name of Object.keys(series).sort())
            list.push({ kindName: qsTr("Series"), label: name, act: "series", value: name, on: browser.series === name,
                        common: true, count: 0 })
        // Only the books of a collection in the tenant shown: the top-level
        // collection itself and those inside it; none in Unfiled; any in
        // the one graph of the whole library, which has no path.
        const tenant = graphTenants.shown
        const top = tenant.path || ""
        const within = collection => !tenant.unfiled && (top === "" || collection.path === top || collection.path.startsWith(top + "/"))
        for (const collection of backend.collections.filter(within))
            list.push({ kindName: qsTr("Collection"), label: collection.name, act: "collection", value: collection.name,
                        on: browser.collection === collection.name, common: true, count: 0 })
        return list
    }
    function take(entry) {
        if (entry === undefined)
            return
        chosen = mode === "merge" ? entry : null
        close()
        if (mode === "merge") return
        if (mode === "find") browser.jumpTo(entry.id)
        else if (entry.act === "clear") browser.showEveryKind()
        else if (entry.act === "asked") browser.onlyAsked = !entry.on
        else if (entry.act === "beyond") browser.beyond = !entry.on
        else if (entry.act === "across") browser.onlyAcross = !entry.on
        else if (entry.act === "kind") browser.setKindShown(entry.value, !entry.on)
        else if (entry.act === "depth") browser.depth = entry.value
        else if (entry.act === "scope") browser.showScope(entry.value)
        else if (entry.act === "book") browser.showBook(entry.value)
        else if (entry.act === "series") browser.showSeries(entry.value)
        else if (entry.act === "collection") browser.showCollection(entry.value)
    }

    GraphWords { id: words }

    ColumnLayout {
        spacing: 8
        // A popup is not an Item and doesn't inherit the window's mirroring.
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        TextField {
            id: field
            objectName: "graphPaletteField"
            Layout.preferredWidth: Math.min(420, win.width - 2 * win.dialogEdge - 2 * chooser.padding)
            placeholderText: chooser.mode === "filter" ? qsTr("Show") : chooser.mode === "merge" ? qsTr("The same as") : qsTr("Find")
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
            objectName: "graphPaletteList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Math.max(120, win.height - 220), 360)
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
                        Layout.preferredWidth: 64
                        text: entry.modelData.kindName || ""
                        color: Qt.alpha(theme.foreground, 0.6)
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    GraphKind {
                        visible: chooser.mode !== "filter" || !!entry.modelData.mark
                        kind: entry.modelData.mark || entry.modelData.kind || ""
                        size: 16
                    }
                    Label {
                        Layout.fillWidth: chooser.mode === "filter"
                        Layout.maximumWidth: list.width * (chooser.mode === "filter" ? 1 : 0.5)
                        text: entry.modelData.label
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: chooser.mode !== "filter"
                        Layout.fillWidth: true
                        text: entry.modelData.detail || ""
                        color: theme.colors.muted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: (entry.modelData.count || 0) > 0
                        text: (entry.modelData.count || 0).toLocaleString(Qt.locale(), 'f', 0)
                        color: Qt.alpha(theme.foreground, 0.6)
                        font.pixelSize: 12
                    }
                    Glyph {
                        visible: !!entry.modelData.on
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
            text: chooser.mode === "filter" ? qsTr("Nothing here goes by that") : qsTr("Nothing known goes by that")
            color: theme.colors.muted
        }
    }
}
