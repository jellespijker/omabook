import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What can be done here, for a pointer or a finger: the commands of the
// shelf, of one book, of the page or of the graph, each with the key that
// does the same, and the commands the plugins add for that place. It is
// opened by the ⋯ in a header, by a right click on a book, or from the bar
// over the page, and holds what has no button of its own, so the window
// stays quiet and nothing is the keyboard's alone.
//
// It lists only what can be done right now (a book's export needs a book
// with a file selected), and runs what is picked through the same Key the
// keyboard does (win.run), once it has closed and the keyboard is the
// window's again. Typing narrows it, as in the other lists.
AppPopup {
    id: menu
    objectName: "commandMenu"
    padding: 12
    widest: 460

    // "library", "book", "reader", "graph", "chronology" or "view" (a plugin's own).
    property string kind: "library"
    // What gets the keyboard back before the command runs: a command of
    // the graph beside the page works while the graph has it.
    property Item returnTo: null
    property var entries: []
    // Picked, and run once the menu has gone.
    property string wanted: ""
    // Where it was opened from, to stand beside; centred when not given.
    property point near: Qt.point(-1, -1)
    readonly property bool placed: near.x >= 0

    // The commands each kind lists, in the order they are listed. What has
    // a button where the menu is opened from is here too: the menu is the
    // one place that has all of it, with its key.
    readonly property var kinds: ({
        "library": ["library.add", "library.add_folder", "library.new", "library.filter", "library.suggest", "library.ask",
                    "library.collections", "library.publish", "library.subscribe", "library.libraries", "library.sync", "library.sites", "library.stats", "library.tidy",
                    "library.graph", "library.graph_build", "library.group", "library.details", "library.plugins",
                    "global.settings", "global.change", "global.help", "global.quit"],
        "book": ["library.read", "library.details", "library.edit", "library.lookup", "library.collection_book",
                 "library.graph_book", "library.chronology_book", "library.transcribe", "library.found_graph", "library.found_moment", "library.export", "library.remove"],
        "reader": ["reader.back", "reader.contents", "reader.annotations", "reader.find", "reader.bookmark", "reader.read_aloud", "reader.voice", "reader.sentence_back",
                   "reader.sentence_next", "reader.read_aloud_stop", "reader.ask", "reader.catch_up",
                   "reader.cast", "reader.graph", "reader.chronology", "reader.marks", "reader.select", "reader.note", "reader.notes", "reader.fold_margin", "reader.ink", "reader.show_ink", "reader.format", "reader.pages", "reader.layout",
                   "reader.larger", "reader.smaller", "reader.expand", "reader.previous", "reader.next", "reader.start",
                   "reader.end", "global.settings", "global.change", "global.help", "global.quit"],
        "graph": ["graph.tenant", "graph.find", "graph.filter", "graph.list", "graph.details", "graph.open", "graph.moments", "graph.expand",
                  "graph.back", "graph.next", "graph.previous", "graph.further", "graph.nearer", "graph.zoom_in",
                  "graph.zoom_out", "graph.fit", "graph.beyond", "graph.chronology", "graph.add", "graph.edit", "graph.merge",
                  "graph.remove", "graph.close"],
        "chronology": ["chronology.tenant", "chronology.find", "chronology.filter", "chronology.range", "chronology.filter_clear", "chronology.unplaced",
                       "chronology.details", "chronology.page", "chronology.graph", "chronology.left", "chronology.right",
                       "chronology.up", "chronology.down", "chronology.zoom_in", "chronology.zoom_out", "chronology.fit",
                       "chronology.pan_left", "chronology.pan_right", "chronology.pan_up", "chronology.pan_down", "chronology.close"],
        // A plugin's view: its own commands come after these.
        "view": ["view.back", "view.close", "global.settings", "global.help"]
    })
    // Where the plugins' commands for a kind work.
    function pluginsFit(kind, where) {
        if (kind === "reader") return where === "reader" || where === "global"
        if (kind === "library") return where === "library" || where === "global"
        // Only the commands of the plugin whose view it is can run, and only those are listed.
        if (kind === "view") return where === "view"
        // A command for the shelf runs on the selected book.
        return kind === "book" && where === "library"
    }

    function show(what, at, focusAfter) {
        kind = what
        near = at || Qt.point(-1, -1)
        returnTo = focusAfter || null
        wanted = ""
        const runnable = win.runnableCommands()
        const known = {}
        for (const command of manual.commands)
            known[command.id] = command
        const found = []
        // A plugin's entry on the shelf is opened or taken off, and no more.
        const entry = what === "book" && win.selectedBookId() < 0
        for (const id of kinds[what] || [])
            if (runnable[id] && known[id] && (!entry || id === "library.read" || id === "library.remove")) found.push(known[id])
        for (const command of manual.commands)
            if (command.place === "plugins" && pluginsFit(what, command.where) && runnable[command.id]) found.push(command)
        entries = found
        field.text = ""
        list.currentIndex = 0
        open()
        field.forceActiveFocus()
    }
    readonly property var matching: {
        const words = field.text.trim().toLowerCase().split(" ").filter(word => word !== "")
        return entries.filter(entry => words.every(word => (entry.short + " " + entry.text + " " + entry.keys + " "
                                                            + (entry.plugin || "")).toLowerCase().includes(word)))
    }
    function take(entry) {
        if (!entry) return
        wanted = entry.id
        close()
    }
    onClosed: {
        const what = wanted
        wanted = ""
        if (returnTo) returnTo.forceActiveFocus()
        if (what !== "") win.run(what)
    }

    // Beside the pointer where it was opened from one, inside the window.
    anchors.centerIn: placed ? undefined : Overlay.overlay
    Binding on x {
        when: menu.placed
        value: Math.max(win.dialogEdge, Math.min(menu.near.x, win.width - menu.width - win.dialogEdge))
    }
    Binding on y {
        when: menu.placed
        value: Math.max(win.dialogEdge, Math.min(menu.near.y, win.height - menu.height - win.dialogEdge))
    }

    ColumnLayout {
        spacing: 6
        // A popup is not an Item and doesn't inherit the window's mirroring.
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        TextField {
            id: field
            objectName: "commandMenuField"
            Layout.preferredWidth: Math.min(436, win.width - 2 * win.dialogEdge - 2 * menu.padding)
            placeholderText: menu.kind === "book" ? qsTr("Do with this book") : qsTr("Do")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onTextEdited: list.currentIndex = 0
            Keys.onDownPressed: list.incrementCurrentIndex()
            Keys.onUpPressed: list.decrementCurrentIndex()
            Keys.onReturnPressed: menu.take(menu.matching[list.currentIndex])
            Keys.onEnterPressed: menu.take(menu.matching[list.currentIndex])
        }
        ListView {
            id: list
            objectName: "commandMenuList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 420, Math.max(88, win.height - 2 * win.dialogEdge - 2 * menu.padding - field.height - 24))
            clip: true
            model: menu.matching
            highlightMoveDuration: 0
            // Every entry is made, not only those in view: the list is short,
            // and each is a control that says which command it runs.
            cacheBuffer: 4000
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: entry
                required property int index
                required property var modelData
                // The command it runs, as every control that stands for one says.
                readonly property string runs: modelData.id
                objectName: "menuEntry-" + modelData.id
                width: list.width
                height: win.touch ? 44 : 34
                Accessible.role: Accessible.MenuItem
                Accessible.name: modelData.short
                Accessible.onPressAction: menu.take(modelData)

                Rectangle {
                    anchors.fill: parent
                    visible: entry.ListView.isCurrentItem || hover.hovered
                    opacity: entry.ListView.isCurrentItem ? 1 : 0.5
                    radius: Math.min(4, theme.radius)
                    color: theme.colors.selection
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 10
                    Label {
                        Layout.fillWidth: true
                        text: entry.modelData.short
                        elide: Text.ElideRight
                    }
                    // Whose it is, for a plugin's.
                    Label {
                        visible: !!entry.modelData.plugin
                        Layout.maximumWidth: list.width * 0.3
                        text: entry.modelData.plugin || ""
                        color: theme.colors.muted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    // The key that does the same, to learn it by.
                    Label {
                        Layout.maximumWidth: list.width * 0.3
                        text: entry.modelData.key || ""
                        color: theme.accent
                        font.pixelSize: 12
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }
                HoverHandler {
                    id: hover
                    onHoveredChanged: if (hovered) list.currentIndex = entry.index
                }
                TapHandler { onTapped: menu.take(entry.modelData) }
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
