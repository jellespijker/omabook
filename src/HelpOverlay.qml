import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The help: every key as it is bound now, and a few pages on how things
// work. Nothing here is a list written by hand: the keys come from the
// commands table (so a new command, or a plugin's, shows up by itself) and
// the pages from helptopics.cpp, which `omabook help` prints too.
//
// It is also a way to do things: type what you want ("add a folder",
// "bigger"), Up and Down pick, and Enter runs the command, through the same
// Key that its shortcut runs. Typing only ever searches; nothing runs until
// Enter.
AppPopup {
    id: popup
    objectName: "help"
    padding: 16
    // As large as is useful, and never larger than the window.
    widest: 900
    tallest: 660

    // "keys", or the id of a page.
    property string page: "keys"
    // Opened over a book, reading comes first.
    property bool reading: false
    // The selected row, or -1 while a page is only being read.
    property int current: -1
    // Said in the footer when Enter can't do what was asked.
    property string note: ""
    // The commands that can be run where the window is now.
    property var runnable: ({})
    // What to do once the popup has closed: a command's id or a tour.
    property string wanted: ""
    readonly property bool wide: !win.compact
    readonly property string query: field.text.trim().toLowerCase()
    readonly property var commands: {
        translator.language
        return manual.commands
    }
    // The keys of one panel or dialog each, which work while it has the keyboard.
    readonly property var panels: {
        translator.language
        return manual.panelKeys
    }
    readonly property var topics: {
        translator.language
        return manual.topics
    }
    readonly property var pages: {
        const first = popup.reading ? ["reading", "ai"] : []
        const rest = popup.topics.filter(topic => !first.includes(topic.id))
        return [{ id: "keys", title: qsTr("Keys") }].concat(first.map(id => popup.topics.find(topic => topic.id === id)), rest)
                .filter(entry => entry !== undefined)
    }
    readonly property var actions: [
        { type: "action", action: "tour", text: qsTr("Take the tour"), note: qsTr("A short walk through the window") },
        { type: "action", action: "allnews", text: qsTr("What's new in Omabook %1").arg(manual.version),
          note: qsTr("What this version brought") }
    ]
    readonly property var rows: build()

    function selectable(row) { return row.type === "command" || row.type === "topic" || row.type === "action" }
    function commandRow(command) {
        return { type: "command", id: command.id, keys: command.keys, text: command.text, place: command.place,
                 where: command.where || command.place, note: command.place === "plugins" ? whereName(command.where) : "" }
    }
    // Where a plugin's command works, said beside it.
    function whereName(where) {
        return where === "reader" ? qsTr("While reading") : where === "library" ? qsTr("On the shelf") : where === "view" ? qsTr("In its view") : qsTr("Anywhere")
    }
    // A key of a panel's own, shown as a command is but not run from here.
    function panelRow(panel, key) {
        return { type: "command", id: panel.id + ":" + key.keys, keys: key.keys, text: key.text, place: "panel", note: "",
                 panelTitle: panel.title }
    }
    // The plugins with commands, by name, in the order they come.
    function pluginNames() {
        const names = []
        for (const command of popup.commands)
            if (command.place === "plugins" && !names.includes(command.plugin)) names.push(command.plugin)
        return names
    }
    function placeName(place) {
        return place === "reader" ? qsTr("Reading") : place === "library" ? qsTr("Library") : place === "graph" ? qsTr("Graph")
             : place === "chronology" ? qsTr("Chronology") : place === "view" ? qsTr("Plugin views") : place === "plugins" ? qsTr("Plugins") : qsTr("Everywhere")
    }

    // The rows of what is shown: the results of a search, the keys by
    // place, or one page with the commands it names.
    function build() {
        const rows = []
        if (popup.query !== "") {
            // "How do I add a folder" is a search for "add folder".
            const small = qsTr("how do i to the a an can what is my").split(" ")
            const all = popup.query.split(" ").filter(word => word !== "")
            const kept = all.filter(word => !small.includes(word))
            const words = kept.length > 0 ? kept : all
            const hit = text => words.every(word => text.includes(word))
            // A key typed as itself leads, then what starts with the words.
            const rank = command => command.keys.toLowerCase().split("  ").includes(popup.query) ? 0
                                  : command.text.toLowerCase().startsWith(words[0]) ? 1 : 2
            const found = popup.commands.filter(command => hit((command.text + " " + command.keys + " " + command.id).toLowerCase()))
                               .map((command, at) => [rank(command), at, command])
                               .sort((a, b) => a[0] - b[0] || a[1] - b[1]).map(ranked => ranked[2])
            const doing = popup.actions.filter(action => hit((action.text + " " + action.note).toLowerCase()))
            const local = []
            for (const panel of popup.panels)
                for (const key of panel.keys)
                    if (hit((key.text + " " + key.keys + " " + panel.title).toLowerCase())) local.push(panelRow(panel, key))
            const reading = popup.topics.filter(topic => hit(topic.search))
            if (found.length + doing.length > 0)
                rows.push({ type: "heading", text: qsTr("Keys") })
            for (const command of found)
                rows.push(commandRow(command))
            for (const action of doing)
                rows.push(action)
            if (local.length > 0)
                rows.push({ type: "heading", text: qsTr("In panels and dialogs") })
            for (const row of local)
                rows.push(Object.assign({}, row, { note: row.panelTitle }))
            if (reading.length > 0)
                rows.push({ type: "heading", text: qsTr("Pages") })
            for (const topic of reading)
                rows.push({ type: "topic", id: topic.id, text: topic.title, note: topic.summary })
            if (rows.length === 0)
                rows.push({ type: "text", html: qsTr("Nothing in the help goes by that. Try fewer words.") })
            return rows
        }
        if (popup.page === "keys") {
            const places = popup.reading ? ["reader", "graph", "chronology", "view", "global", "library"] : ["library", "graph", "chronology", "view", "global", "reader"]
            for (const place of places) {
                const here = popup.commands.filter(command => command.place === place)
                if (here.length === 0)
                    continue
                rows.push({ type: "heading", text: placeName(place) })
                for (const command of here)
                    rows.push(commandRow(command))
            }
            // Each plugin's commands under its own name, saying where they work.
            for (const name of pluginNames()) {
                rows.push({ type: "heading", text: qsTr("Plugin: %1").arg(name) })
                for (const command of popup.commands.filter(command => command.place === "plugins" && command.plugin === name))
                    rows.push(commandRow(command))
            }
            // Then the keys a panel or dialog has of its own.
            for (const panel of popup.panels) {
                rows.push({ type: "heading", text: panel.title })
                for (const key of panel.keys)
                    rows.push(panelRow(panel, key))
            }
            return rows
        }
        const topic = popup.topics.find(entry => entry.id === popup.page)
        if (topic === undefined)
            return rows
        rows.push({ type: "title", text: topic.title, note: topic.summary })
        for (const section of topic.sections) {
            rows.push({ type: "heading", text: section.heading })
            rows.push({ type: "text", html: section.html })
        }
        if (topic.id === "start")
            for (const action of popup.actions)
                rows.push(action)
        if (topic.commands.length > 0) {
            rows.push({ type: "heading", text: qsTr("Keys for this") })
            for (const id of topic.commands) {
                const command = popup.commands.find(entry => entry.id === id)
                if (command !== undefined)
                    rows.push(commandRow(command))
            }
        }
        return rows
    }

    function show() {
        reading = win.inReader
        runnable = win.runnableCommands()
        wanted = ""
        note = ""
        field.text = ""
        page = "keys"
        flick.contentY = 0
        current = rows.findIndex(row => selectable(row))
        open()
        field.forceActiveFocus()
    }
    function goTo(id) {
        field.text = ""
        page = id
        field.forceActiveFocus()
    }
    function turn(by) {
        const at = pages.findIndex(entry => entry.id === page)
        goTo(pages[(Math.max(0, at) + by + pages.length) % pages.length].id)
    }
    function scroll(by) {
        flick.contentY = Math.max(0, Math.min(flick.contentY + by, Math.max(0, flick.contentHeight - flick.height)))
    }
    // Down and Up go to the next row that can be taken when it is in or
    // near what shows, and scroll the text otherwise, so a page reads from
    // top to bottom with the same two keys.
    function step(by) {
        note = ""
        let next = -1
        for (let i = current + by; i >= 0 && i < rows.length; i += by) {
            if (selectable(rows[i])) {
                next = i
                break
            }
        }
        const item = next >= 0 ? list.itemAt(next) : null
        if (item === null || item.y > flick.contentY + flick.height + 60 || item.y + item.height < flick.contentY - 60) {
            scroll(by * 120)
            return
        }
        current = next
        if (item.y < flick.contentY)
            flick.contentY = Math.max(0, item.y - 8)
        else if (item.y + item.height > flick.contentY + flick.height)
            flick.contentY = item.y + item.height - flick.height + 8
    }
    function take(index) {
        const row = rows[index]
        if (row === undefined || !selectable(row))
            return
        if (row.type === "topic") {
            goTo(row.id)
        } else if (row.type === "action") {
            wanted = row.action
            close()
        } else if (runnable[row.id]) {
            wanted = row.id
            close()
        } else {
            current = index
            note = row.place === "panel" ? qsTr("That one works in its panel or dialog: %1.").arg(row.panelTitle)
                 : row.where === "reader" ? qsTr("That one works while a book is open.")
                 : row.where === "library" ? qsTr("That one works on the shelf.")
                 : row.where === "graph" ? qsTr("That one works in the graph.") : row.where === "chronology" ? qsTr("That one works in the chronology.")
                 : qsTr("That can't be done right now.")
        }
    }

    onRowsChanged: {
        flick.contentY = 0
        // A page is read first; the keys and a search are picked from.
        current = (query !== "" || page === "keys") ? rows.findIndex(row => selectable(row)) : -1
    }
    // Only once the popup is gone is the keyboard the window's again, so
    // what was asked for runs then.
    onClosed: {
        const what = wanted
        wanted = ""
        if (what === "tour" || what === "allnews")
            win.startTour(what)
        else if (what !== "")
            win.run(what)
    }

    contentItem: ColumnLayout {
        spacing: 12
        // A popup is not an Item and doesn't inherit the window's mirroring,
        // so its content opts in.
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        // The help closes on the key that opened it. A shortcut outside a
        // dialog is off while the dialog is up, so this one lives in it; a
        // key pressed alone (?) is the field's to see, below.
        Shortcut {
            sequences: custom.chordKeys["global.help"] || []
            enabled: popup.opened
            onActivated: popup.close()
        }

        TextField {
            id: field
            objectName: "helpField"
            Layout.fillWidth: true
            focus: true
            placeholderText: qsTr("Search the help, or type what you want to do")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onTextEdited: popup.note = ""
            Keys.onPressed: (event) => {
                const helpKeys = custom.plainKeys["global.help"] || []
                if (event.key === Qt.Key_Down) popup.step(1)
                else if (event.key === Qt.Key_Up) popup.step(-1)
                else if (event.key === Qt.Key_PageDown) popup.scroll(flick.height * 0.8)
                else if (event.key === Qt.Key_PageUp) popup.scroll(-flick.height * 0.8)
                else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) popup.take(popup.current)
                else if (event.key === Qt.Key_Tab) popup.turn(1)
                else if (event.key === Qt.Key_Backtab) popup.turn(-1)
                // Escape first empties the search, and closes the help after.
                else if (event.key === Qt.Key_Escape && field.text !== "") field.text = ""
                // The key that opened the help closes it, until something is typed.
                else if (field.text === "" && event.text !== "" && helpKeys.includes(event.text.toUpperCase())) popup.close()
                else return
                event.accepted = true
            }
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: popup.wide ? 2 : 1
            columnSpacing: 16
            rowSpacing: 8

            // The pages: down the side where there is room, along the top
            // where there isn't.
            ListView {
                id: nav
                objectName: "helpPages"
                Layout.preferredWidth: popup.wide ? 190 : -1
                Layout.fillWidth: !popup.wide
                Layout.fillHeight: popup.wide
                Layout.preferredHeight: popup.wide ? -1 : (win.touch ? 44 : 34)
                orientation: popup.wide ? ListView.Vertical : ListView.Horizontal
                clip: true
                spacing: 2
                model: popup.pages
                currentIndex: popup.query !== "" ? -1 : popup.pages.findIndex(entry => entry.id === popup.page)
                highlightMoveDuration: 0

                delegate: Item {
                    id: entry
                    required property int index
                    required property var modelData
                    objectName: "helpPage-" + modelData.id
                    width: popup.wide ? nav.width : name.implicitWidth + 24
                    height: win.touch ? 44 : 34

                    Rectangle {
                        anchors.fill: parent
                        visible: entry.ListView.isCurrentItem || pageHover.hovered
                        radius: Math.min(4, theme.radius)
                        color: theme.colors.selection
                        opacity: entry.ListView.isCurrentItem ? 1 : 0.5
                    }
                    Label {
                        id: name
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        verticalAlignment: Text.AlignVCenter
                        text: entry.modelData.title
                        elide: Text.ElideRight
                        font.bold: entry.ListView.isCurrentItem
                    }
                    HoverHandler { id: pageHover }
                    TapHandler { onTapped: popup.goTo(entry.modelData.id) }
                }
            }

            Flickable {
                id: flick
                objectName: "helpPage"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: width
                contentHeight: column.height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                Column {
                    id: column
                    // Clear of the scroll bar, which lies over the page's edge.
                    width: flick.width - 12

                    Repeater {
                        id: list
                        objectName: "helpRows"
                        model: popup.rows

                        delegate: Loader {
                            id: row
                            required property int index
                            required property var modelData
                            readonly property bool selected: index === popup.current
                            width: column.width
                            sourceComponent: modelData.type === "command" ? commandRow
                                           : modelData.type === "heading" ? headingRow
                                           : modelData.type === "title" ? titleRow
                                           : modelData.type === "text" ? textRow : linkRow

                            Component {
                                id: headingRow
                                Item {
                                    // More room above a heading than below it; none above the first.
                                    implicitHeight: heading.implicitHeight + (row.index === 0 ? 8 : 26)
                                    Label {
                                        id: heading
                                        anchors.bottom: parent.bottom
                                        anchors.bottomMargin: 6
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.leftMargin: 10
                                        text: row.modelData.text
                                        font.pixelSize: 15
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            Component {
                                id: titleRow
                                Column {
                                    leftPadding: 10
                                    rightPadding: 10
                                    spacing: 4
                                    Label {
                                        width: parent.width - 20
                                        text: row.modelData.text
                                        font.pixelSize: 22
                                        font.bold: true
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        width: parent.width - 20
                                        text: row.modelData.note
                                        color: theme.colors.muted
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }
                            Component {
                                id: textRow
                                Item {
                                    implicitHeight: words.implicitHeight + 4
                                    Label {
                                        id: words
                                        x: 10
                                        // A line no longer than reads comfortably.
                                        width: Math.min(parent.width - 20, 600)
                                        text: row.modelData.html
                                        textFormat: Text.RichText
                                        wrapMode: Text.Wrap
                                        lineHeight: 1.3
                                    }
                                }
                            }
                            Component {
                                id: commandRow
                                Item {
                                    objectName: "helpRow-" + row.modelData.id
                                    readonly property string keys: row.modelData.keys
                                    readonly property bool selected: row.selected
                                    implicitHeight: Math.max(win.touch ? 44 : 30, what.implicitHeight + 10)

                                    Rectangle {
                                        anchors.fill: parent
                                        visible: row.selected
                                        radius: Math.min(4, theme.radius)
                                        color: theme.colors.selection
                                    }
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 6
                                        spacing: 12
                                        Label {
                                            Layout.preferredWidth: popup.wide ? 150 : 104
                                            text: row.modelData.keys
                                            color: theme.accent
                                            font.bold: true
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            id: what
                                            Layout.fillWidth: true
                                            text: row.modelData.text
                                            wrapMode: Text.Wrap
                                        }
                                        // Where a plugin's command works, or whose a panel's key is.
                                        Label {
                                            objectName: "helpRowNote"
                                            visible: !!row.modelData.note
                                            Layout.maximumWidth: popup.wide ? 180 : 110
                                            text: row.modelData.note || ""
                                            color: theme.colors.muted
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                        }
                                        // Enter, for a finger or a mouse. A tap on the
                                        // row itself only selects: a list that is being
                                        // read must not quit the app under a thumb.
                                        Rectangle {
                                            objectName: "helpRun"
                                            visible: row.selected && !!popup.runnable[row.modelData.id]
                                            Layout.preferredWidth: Math.max(run.implicitWidth + 20, win.touch ? 36 : 0)
                                            Layout.preferredHeight: win.touch ? 36 : 22
                                            radius: Math.min(4, theme.radius)
                                            color: runHover.hovered ? theme.background : "transparent"
                                            border.color: theme.colors.muted
                                            Label {
                                                id: run
                                                anchors.centerIn: parent
                                                text: qsTr("Run")
                                                font.pixelSize: 12
                                            }
                                            HoverHandler { id: runHover }
                                            TapHandler { onTapped: popup.take(row.index) }
                                        }
                                    }
                                    TapHandler {
                                        onTapped: {
                                            popup.note = ""
                                            popup.current = row.index
                                        }
                                        onDoubleTapped: popup.take(row.index)
                                    }
                                }
                            }
                            Component {
                                id: linkRow
                                Item {
                                    objectName: "helpLink-" + (row.modelData.id || row.modelData.action)
                                    implicitHeight: Math.max(win.touch ? 44 : 34, link.implicitHeight + 10)

                                    Rectangle {
                                        anchors.fill: parent
                                        visible: row.selected || linkHover.hovered
                                        radius: Math.min(4, theme.radius)
                                        color: theme.colors.selection
                                        opacity: row.selected ? 1 : 0.5
                                    }
                                    RowLayout {
                                        id: link
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 12
                                        Label {
                                            text: row.modelData.text
                                            font.bold: true
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: row.modelData.note
                                            color: theme.colors.muted
                                            elide: Text.ElideRight
                                        }
                                    }
                                    HoverHandler { id: linkHover }
                                    TapHandler { onTapped: popup.take(row.index) }
                                }
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                objectName: "helpNote"
                Layout.fillWidth: true
                text: popup.note !== "" ? popup.note
                    : win.touch ? "" : qsTr("Up and Down pick, Enter runs or opens, Tab turns the page, Esc closes.")
                color: popup.note !== "" ? theme.foreground : theme.colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            AppButton {
                objectName: "helpNews"
                text: qsTr("What's New")
                focusPolicy: Qt.NoFocus
                onClicked: {
                    popup.wanted = "allnews"
                    popup.close()
                }
            }
            AppButton {
                objectName: "helpTour"
                text: qsTr("Take the Tour")
                focusPolicy: Qt.NoFocus
                onClicked: {
                    popup.wanted = "tour"
                    popup.close()
                }
            }
        }
    }
}
