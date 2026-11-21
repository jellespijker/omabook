import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The plugin manager: what Omabook is made of and what has been added to
// it, sorted by what each plugin does, with one plugin's details beside the
// list. Plugins are switched, ordered, tried, made from a template,
// installed and removed here. What it knows comes from PluginManager; this
// file is only how it looks and which key does what.
AppPopup {
    id: dialog
    objectName: "plugins"
    widest: 960
    tallest: 720
    padding: 20
    // Escape goes back a step before it closes, so the dialog handles it.
    closePolicy: Popup.CloseOnPressOutside

    readonly property var manager: backend.pluginManager
    // Text that is read but is not the point: softer than the foreground,
    // and still easy to read on any theme.
    readonly property color quiet: Qt.alpha(theme.foreground, 0.68)
    // The selected row and the rule between the panes, made of the
    // foreground so they show on a theme that names no selection colour.
    readonly property color chosenFill: Qt.alpha(theme.foreground, 0.13)
    // Wide enough for the list and the details side by side; otherwise the
    // details take the list's place.
    readonly property bool wide: win.expanded
    property bool detailShown: false
    property bool keysShown: false
    // The plugin the details are of: its place in the list, and its id, by
    // which it is found again when the list changes under it.
    property int at: 0
    property string currentId: ""
    readonly property var matching: {
        const wanted = filter.text.trim().toLowerCase().split(" ").filter(word => word !== "")
        if (wanted.length === 0)
            return manager.plugins
        return manager.plugins.filter(plugin => {
            const text = [plugin.name, plugin.description, groupNames[plugin.group], origins[plugin.origin],
                          stateNames[plugin.state], fieldList(plugin.fields)].join(" ").toLowerCase()
            return wanted.every(word => text.includes(word))
        })
    }
    readonly property var current: matching.length > 0 ? matching[Math.min(at, matching.length - 1)] : null
    // Which of the things needing attention "Show" leads to next.
    property int healthAt: 0

    readonly property var groupNames: ({
        metadata: qsTr("Book details"), reading: qsTr("Reading sites"), script: qsTr("Scripts"),
        // Programs of the user's own that can't be used as they are.
        program: qsTr("Programs"), calibre: qsTr("Calibre"), format: qsTr("Formats and adding books"),
        catalog: qsTr("Catalogs"), ai: qsTr("AI")
    })
    // Said beside a group's name, where the order of its plugins matters.
    readonly property var groupNotes: ({ metadata: qsTr("asked in this order") })
    readonly property var origins: ({
        "built-in": qsTr("Built in"), calibre: qsTr("From Calibre"), folder: qsTr("Your own")
    })
    readonly property var stateNames: ({
        on: qsTr("On"), off: qsTr("Off"), needs: qsTr("Needs something"), broken: qsTr("Broken")
    })
    readonly property var fieldNames: ({
        title: qsTr("title"), authors: qsTr("authors"), series: qsTr("series"), publisher: qsTr("publisher"),
        published: qsTr("date"), language: qsTr("language"), isbn: qsTr("ISBN"), description: qsTr("description"),
        tags: qsTr("tags"), cover: qsTr("cover")
    })
    // What a script plugin asked to be allowed in plugin.json, in plain
    // words; a program plugin is not held to any of it.
    readonly property var permissionNames: ({
        http: qsTr("the network"), files: qsTr("files in its own folder"), run: qsTr("programs on this computer"),
        microphone: qsTr("the microphone"), program: qsTr("everything you can: it is a program of its own, not held to permissions")
    })
    readonly property var eventNames: ({
        "app.started": qsTr("Omabook starts"), "library.switched": qsTr("another library is opened"),
        "book.added": qsTr("a book is added"), "book.removed": qsTr("a book is removed"),
        "book.changed": qsTr("a book's details change"), "book.opened": qsTr("a book is opened"),
        "book.closed": qsTr("a book is closed"), "page.turned": qsTr("a page is turned"),
        "lookup.finished": qsTr("a look-up has finished"), "log.changed": qsTr("its data comes from another computer")
    })
    function fieldList(fields) { return (fields || []).map(field => fieldNames[field] || field).join(", ") }
    function permissionText(permissions) {
        return (permissions || []).length === 0 ? qsTr("nothing beyond the library")
               : permissions.map(permission => permissionNames[permission] || permission).join(", ")
    }
    function healthText(issue) {
        if (issue.issue === "broken") return qsTr("%1 stopped with an error").arg(issue.name)
        if (issue.issue === "missing") return qsTr("%1 can't find its program").arg(issue.name)
        if (issue.issue === "needs-omabook") return qsTr("%1 needs a newer Omabook than this").arg(issue.name)
        if (issue.issue === "update-needs-omabook") return qsTr("%1 has an update that waits for Omabook %2").arg(issue.name).arg(issue.needs)
        if (issue.issue === "review")
            return issue.reason === "name" ? qsTr("%1 has an update that goes by another name: review").arg(issue.name)
                                           : qsTr("%1 has an update that asks for %2: review").arg(issue.name).arg(permissionText(issue.permissions))
        if (issue.issue === "update") return qsTr("%1 has an update").arg(issue.name)
        if (issue.issue === "source") return qsTr("%1 can't be updated from where it came from").arg(issue.name)
        return qsTr("Calibre is switched off, though a Calibre library is on this computer")
    }

    function select(index) {
        if (index < 0 || index >= matching.length)
            return
        at = index
        currentId = matching[index].id
    }
    // Shows a plugin by its id, whatever the filter hid.
    function show(id) {
        if (!matching.some(plugin => plugin.id === id))
            filter.text = ""
        const index = matching.findIndex(plugin => plugin.id === id)
        if (index < 0)
            return
        select(index)
        keysShown = false
        if (!wide)
            detailShown = true
    }
    function showNextIssue() {
        const issues = manager.health
        if (issues.length === 0)
            return
        healthAt = healthAt % issues.length
        show(issues[healthAt].id)
        healthAt = (healthAt + 1) % issues.length
    }
    function toggle(plugin) {
        if (plugin && plugin.switchable)
            backend.setPluginEnabled(plugin.id, !plugin.enabled)
    }
    function askRemove(plugin) {
        if (plugin && plugin.removable)
            removal.show(plugin)
    }
    function reload() {
        manager.reload()
        // Calibre's plugins are remembered between runs; this asks Calibre again.
        backend.refreshPlugins()
    }
    // One step back: out of the filter, the keys, the details, then the dialog.
    function back() {
        if (filter.activeFocus || detail.typing) {
            if (filter.activeFocus)
                filter.text = ""
            keyboard.forceActiveFocus()
        } else if (keysShown) {
            keysShown = false
        } else if (filter.text !== "") {
            filter.text = ""
        } else if (!wide && detailShown) {
            detailShown = false
        } else {
            close()
        }
    }

    // The list is made anew whenever a plugin changes; the one that was
    // selected stays selected.
    onMatchingChanged: {
        const found = matching.findIndex(plugin => plugin.id === currentId)
        at = found >= 0 ? found : Math.max(0, Math.min(at, matching.length - 1))
        currentId = matching.length > 0 ? matching[at].id : ""
    }
    // The first plugin is shown with the heading above it.
    onAtChanged: at === 0 ? list.positionViewAtBeginning() : list.positionViewAtIndex(at, ListView.Contain)
    onOpened: {
        manager.refresh()
        filter.text = ""
        detailShown = false
        keysShown = false
        healthAt = 0
        select(0)
        list.positionViewAtBeginning()
        keyboard.forceActiveFocus()
    }
    onClosed: manager.stopTrial()

    ColumnLayout {
        id: keyboard
        anchors.fill: parent
        spacing: 12
        focus: true
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        // Every key of the dialog. A key a text field took never gets here;
        // while one has the focus, only the keys that don't type are heard.
        Keys.onPressed: (event) => {
            const typing = filter.activeFocus || detail.typing
            const plain = event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier
            const shift = event.modifiers === Qt.ShiftModifier
            const backward = win.rightToLeft ? Qt.Key_Right : Qt.Key_Left
            let taken = true
            // While the keys are on show, only the ones that put them away.
            if (dialog.keysShown && event.key !== Qt.Key_Escape && event.key !== Qt.Key_Question && event.key !== Qt.Key_F1) taken = false
            else if (event.key === Qt.Key_Escape) dialog.back()
            else if (event.key === Qt.Key_Down && event.modifiers === Qt.AltModifier) { if (dialog.current) manager.move(dialog.current.id, 1) }
            else if (event.key === Qt.Key_Up && event.modifiers === Qt.AltModifier) { if (dialog.current) manager.move(dialog.current.id, -1) }
            else if (event.key === Qt.Key_Down) dialog.select(dialog.at + 1)
            else if (event.key === Qt.Key_Up) dialog.select(dialog.at - 1)
            else if (event.key === Qt.Key_PageDown) dialog.select(Math.min(dialog.matching.length - 1, dialog.at + 8))
            else if (event.key === Qt.Key_PageUp) dialog.select(Math.max(0, dialog.at - 8))
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                if (filter.activeFocus) keyboard.forceActiveFocus()
                if (!dialog.wide && dialog.current) dialog.detailShown = true
            }
            else if (event.key === Qt.Key_F && event.modifiers === Qt.ControlModifier) filter.forceActiveFocus()
            else if (typing) taken = false
            else if (event.key === Qt.Key_Slash) filter.forceActiveFocus()
            else if (event.key === Qt.Key_Question || event.key === Qt.Key_F1) dialog.keysShown = !dialog.keysShown
            else if (event.key === Qt.Key_J && plain) dialog.select(dialog.at + 1)
            else if (event.key === Qt.Key_K && plain) dialog.select(dialog.at - 1)
            else if (event.key === Qt.Key_J && shift) { if (dialog.current) manager.move(dialog.current.id, 1) }
            else if (event.key === Qt.Key_K && shift) { if (dialog.current) manager.move(dialog.current.id, -1) }
            else if (event.key === Qt.Key_Greater) listSide.resize(1)
            else if (event.key === Qt.Key_Less) listSide.resize(-1)
            else if (event.key === Qt.Key_Home) dialog.select(0)
            else if (event.key === Qt.Key_End) dialog.select(dialog.matching.length - 1)
            else if (event.key === Qt.Key_Space) dialog.toggle(dialog.current)
            else if ((event.key === backward || event.key === Qt.Key_Backspace) && !dialog.wide && dialog.detailShown) dialog.detailShown = false
            else if (event.key === Qt.Key_N && plain) creation.show()
            else if (event.key === Qt.Key_I && plain) installation.show()
            else if (event.key === Qt.Key_R && plain) dialog.reload()
            else if (event.key === Qt.Key_R && shift) { if (dialog.current && dialog.current.kind === "script") manager.retry(dialog.current.id) }
            else if (event.key === Qt.Key_O && plain) manager.openFolder(dialog.current && dialog.current.removable ? dialog.current.id : "")
            else if (event.key === Qt.Key_T && plain) {
                if (dialog.current && dialog.current.tryable) {
                    if (!dialog.wide) dialog.detailShown = true
                    detail.tryIt()
                }
            }
            else if (event.key === Qt.Key_A && plain) dialog.showNextIssue()
            else if (event.key === Qt.Key_U && plain) { if (manager.sourced) manager.checkAll() }
            else if (event.key === Qt.Key_U && shift) { if (dialog.current && dialog.current.source) manager.updateNow(dialog.current.id) }
            else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_D) && plain) dialog.askRemove(dialog.current)
            else taken = false
            event.accepted = taken
        }

        // The name, the field that finds a plugin and the two buttons in
        // one row; in a compact window the field has a row of its own.
        GridLayout {
            Layout.fillWidth: true
            columnSpacing: 8
            rowSpacing: 8
            Label {
                Layout.row: 0
                Layout.column: 0
                text: qsTr("Plugins")
                font.pixelSize: 18
                font.bold: true
            }
            TextField {
                id: filter
                objectName: "pluginsFilter"
                Layout.row: win.compact ? 1 : 0
                Layout.column: win.compact ? 0 : 1
                Layout.columnSpan: win.compact ? 4 : 1
                Layout.fillWidth: true
                Layout.maximumWidth: win.compact ? Number.POSITIVE_INFINITY : 340
                Layout.preferredHeight: 40
                Layout.leftMargin: win.compact ? 0 : 8
                placeholderText: qsTr("Find a plugin")
                selectByMouse: true
                // The keys belong to the list until the field is asked for.
                focusPolicy: Qt.ClickFocus
                Material.roundedScale: theme.radius
                onTextEdited: {
                    dialog.detailShown = false
                    dialog.select(0)
                }
            }
            Item {
                Layout.row: 0
                Layout.column: win.compact ? 1 : 2
                Layout.fillWidth: true
            }
            AppButton {
                objectName: "pluginsNew"
                Layout.row: 0
                Layout.column: win.compact ? 2 : 3
                focusPolicy: Qt.TabFocus
                text: qsTr("New…")
                onClicked: creation.show()
            }
            AppButton {
                objectName: "pluginsInstall"
                Layout.row: 0
                Layout.column: win.compact ? 3 : 4
                focusPolicy: Qt.TabFocus
                text: qsTr("Install…")
                onClicked: installation.show()
            }
        }

        // One line for whatever needs attention, leading to the plugin.
        Rectangle {
            objectName: "pluginsHealth"
            // Not over the details where they stand alone: there is no room
            // for both, and the line is about finding the plugin.
            visible: manager.health.length > 0 && (dialog.wide || !dialog.detailShown) && !dialog.keysShown
            Layout.fillWidth: true
            implicitHeight: healthRow.implicitHeight + 8
            radius: theme.radius
            color: Qt.alpha(theme.colors.orange, 0.1)
            border.color: Qt.alpha(theme.colors.orange, 0.5)

            RowLayout {
                id: healthRow
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 6
                spacing: 10
                PluginMark { condition: "needs" }
                Label {
                    objectName: "pluginsHealthText"
                    Layout.fillWidth: true
                    text: manager.health.length === 0 ? ""
                          : manager.health.length === 1 ? dialog.healthText(manager.health[0])
                          : qsTr("%Ln plugins need attention", "", manager.health.length)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                }
                AppButton {
                    objectName: "pluginsHealthShow"
                    focusPolicy: Qt.TabFocus
                    text: manager.health.length > 1 ? qsTr("Show Next") : qsTr("Show")
                    onClicked: dialog.showNextIssue()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16
            visible: !dialog.keysShown

            // The list is a pane like those beside the shelf and the page:
            // its edge drags, < and > move it, and its width is remembered.
            SidePane {
                id: listSide
                objectName: "pluginsSide"
                name: "plugins"
                leading: true
                lined: false
                fixed: !dialog.wide
                standard: 312
                least: 240
                most: 520
                // What the details keep beside it.
                room: dialog.availableWidth - 16 - 380
                visible: dialog.wide || !dialog.detailShown
                Layout.fillWidth: !dialog.wide
                Layout.fillHeight: true

            ListView {
                id: list
                objectName: "pluginsList"
                anchors.fill: parent
                clip: true
                // The count, not the plugins: a plugin that changes is then
                // drawn anew in place, and the list keeps where it was.
                model: dialog.matching.length
                currentIndex: dialog.at
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: Column {
                    id: row
                    required property int index
                    readonly property var plugin: dialog.matching[index] || ({})
                    // A heading where what the plugins do changes.
                    readonly property bool first: index === 0 || dialog.matching[index - 1].group !== plugin.group
                    readonly property bool chosen: dialog.at === index
                    width: list.width

                    RowLayout {
                        visible: row.first
                        width: parent.width - 14
                        height: implicitHeight + (row.index === 0 ? 6 : 22)
                        spacing: 8
                        Label {
                            Layout.alignment: Qt.AlignBottom
                            Layout.leftMargin: 8
                            Layout.bottomMargin: 6
                            text: dialog.groupNames[row.plugin.group] || row.plugin.group || ""
                            color: theme.accent
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignBottom
                            Layout.bottomMargin: 7
                            text: dialog.groupNotes[row.plugin.group] || ""
                            color: dialog.quiet
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                    Rectangle {
                        objectName: "pluginRow_" + (row.plugin.id || "")
                        width: parent.width - 14
                        height: win.touch ? 46 : 34
                        radius: Math.min(4, theme.radius)
                        color: row.chosen ? dialog.chosenFill : rowHover.hovered ? Qt.alpha(theme.foreground, 0.06) : "transparent"
                        Accessible.role: Accessible.ListItem
                        Accessible.name: (row.plugin.name || "") + ", " + (dialog.stateNames[row.plugin.state] || "")

                        HoverHandler { id: rowHover }
                        TapHandler {
                            onTapped: {
                                dialog.select(row.index)
                                keyboard.forceActiveFocus()
                                if (!dialog.wide)
                                    dialog.detailShown = true
                            }
                        }
                        RowLayout {
                            anchors.fill: parent
                            // What Calibre found sits beneath Calibre itself.
                            anchors.leftMargin: row.plugin.beneath ? 26 : 2
                            anchors.rightMargin: 10
                            spacing: 2

                            // The mark is the switch, for the pointer and for touch.
                            Item {
                                objectName: "pluginMark_" + (row.plugin.id || "")
                                Layout.preferredWidth: win.touch ? 44 : 30
                                Layout.fillHeight: true
                                PluginMark {
                                    anchors.centerIn: parent
                                    condition: row.plugin.state || "on"
                                }
                                TapHandler {
                                    enabled: row.plugin.switchable === true
                                    onTapped: {
                                        dialog.select(row.index)
                                        dialog.toggle(row.plugin)
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: row.plugin.name || ""
                                textFormat: Text.PlainText
                                color: row.plugin.state === "off" ? dialog.quiet : theme.foreground
                                elide: Text.ElideRight
                            }
                            Label {
                                visible: row.plugin.origin !== "built-in" && row.plugin.group !== "script" && !row.plugin.beneath
                                Layout.leftMargin: 6
                                text: row.plugin.origin === "calibre" ? qsTr("Calibre") : row.plugin.source ? qsTr("GitHub") : qsTr("Yours")
                                color: dialog.quiet
                                font.pixelSize: 12
                            }
                        }
                    }
                }

                // What to do when there is nothing of one's own yet, or
                // nothing by the name that was typed.
                footer: Item {
                    width: list.width
                    height: hint.visible ? hint.implicitHeight + 28 : 8
                    Label {
                        id: hint
                        objectName: "pluginsHint"
                        readonly property bool own: dialog.manager.plugins.some(plugin => plugin.removable)
                        visible: text !== ""
                        x: 8
                        y: 16
                        width: parent.width - 30
                        text: dialog.matching.length === 0 ? qsTr("No plugin goes by that.")
                              : filter.text.trim() === "" && !own ? qsTr("Nothing of your own yet. N starts a plugin from a template; I installs one someone else wrote.")
                              : ""
                        color: dialog.quiet
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }

            }

            Rectangle {
                visible: dialog.wide
                Layout.fillHeight: true
                implicitWidth: 1
                color: dialog.chosenFill
            }

            PluginDetail {
                id: detail
                objectName: "pluginDetail"
                visible: dialog.wide || dialog.detailShown
                Layout.fillWidth: true
                Layout.fillHeight: true
                host: dialog
                plugin: dialog.current
                alone: !dialog.wide
            }
        }

        // Every key of the dialog, in place of the list while it is asked for.
        Flickable {
            objectName: "pluginsKeys"
            visible: dialog.keysShown
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: keysGrid.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            GridLayout {
                id: keysGrid
                width: parent.width - 14
                // Read down the first column and then the second; one
                // column where two don't fit.
                readonly property bool paired: width >= 560
                flow: paired ? GridLayout.TopToBottom : GridLayout.LeftToRight
                rows: paired ? 9 : -1
                columns: paired ? -1 : 1
                columnSpacing: 24
                rowSpacing: 8
                Repeater {
                    // The same list the help shows, from panelkeys.cpp.
                    model: {
                        translator.language
                        const own = manual.panelKeys.find(panel => panel.id === "plugins")
                        return own ? own.keys : []
                    }
                    delegate: RowLayout {
                        id: entry
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            Layout.preferredWidth: 132
                            text: entry.modelData.keys
                            color: theme.accent
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: entry.modelData.text
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            // While the sources are asked, that is said where the keys are.
            Thinking {
                id: asking
                objectName: "pluginsAsking"
                Layout.fillWidth: true
                active: manager.checking === "all" || manager.checking === "daily"
                text: qsTr("Asking GitHub for updates…")
                room: width
            }
            Label {
                objectName: "pluginsKeysHint"
                visible: !asking.active
                Layout.fillWidth: true
                // The keys used most; ? shows the rest.
                text: qsTr("Space switches · Enter shows details · / finds · ? shows every key")
                color: dialog.quiet
                font.pixelSize: 12
                elide: Text.ElideRight
                // A click shows every key, as ? does; the help (?) lists them too.
                HoverHandler { enabled: !win.touch; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    enabled: !win.touch
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onTapped: dialog.keysShown = !dialog.keysShown
                }
            }
            AppButton {
                objectName: "pluginsCheckAll"
                // Only where a plugin came from GitHub at all, and where the
                // window has room for a third button: U does the same, and
                // each plugin has its own Check Now.
                visible: manager.sourced && !win.compact
                enabled: manager.checking === ""
                focusPolicy: Qt.TabFocus
                text: qsTr("Check for Updates")
                onClicked: manager.checkAll()
            }
            AppButton {
                objectName: "pluginsRefresh"
                focusPolicy: Qt.TabFocus
                text: qsTr("Look Again")
                onClicked: dialog.reload()
            }
            AppButton {
                focusPolicy: Qt.TabFocus
                text: qsTr("Close")
                primary: true
                onClicked: dialog.close()
            }
        }
    }

    PluginNewSheet {
        id: creation
        host: dialog
        onMade: (id) => dialog.show(id)
        onClosed: keyboard.forceActiveFocus()
    }
    PluginInstallSheet {
        id: installation
        host: dialog
        onInstalled: (id) => dialog.show(id)
        onClosed: keyboard.forceActiveFocus()
    }
    // A button that was pressed with the keyboard and then went away (Forget
    // the Source, Roll Back) takes the focus with it, and the keys with the
    // focus; they come back to the dialog.
    readonly property bool adrift: opened && !activeFocus && !creation.visible && !installation.visible && !removal.visible
    onAdriftChanged: if (adrift) ashore.restart()
    Timer {
        id: ashore
        interval: 0
        onTriggered: if (dialog.adrift) keyboard.forceActiveFocus()
    }
    // What the manager puts on offer by itself (an update that asks for more
    // than the version installed, a plugin a book needs) is shown for a yes or a no.
    Connections {
        target: dialog.manager
        function onOfferChanged() {
            if (dialog.opened && Object.keys(dialog.manager.offer).length > 0)
                installation.review()
        }
    }

    AppPopup {
        id: removal
        objectName: "pluginRemoval"
        property string pluginId: ""
        property string pluginName: ""
        property string problem: ""
        widest: 460
        function show(plugin) {
            pluginId = plugin.id
            pluginName = plugin.name
            problem = ""
            open()
            confirm.forceActiveFocus()
        }
        onClosed: keyboard.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 16
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label {
                Layout.fillWidth: true
                text: qsTr("Remove the plugin “%1”?").arg(removal.pluginName)
                textFormat: Text.PlainText
                font.pixelSize: 18
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "pluginRemovalNote"
                Layout.fillWidth: true
                text: removal.problem !== "" ? removal.problem
                      : qsTr("Its folder moves to the trash, with everything it kept there. To stop it and keep it, switch it off instead.")
                textFormat: Text.PlainText
                color: removal.problem !== "" ? theme.colors.orange : dialog.quiet
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                // As where a book is removed: Enter removes, to the trash,
                // and Escape keeps.
                AppButton {
                    text: qsTr("Keep")
                    onClicked: removal.close()
                }
                AppButton {
                    id: confirm
                    objectName: "pluginRemovalConfirm"
                    text: qsTr("Remove")
                    primary: true
                    onClicked: {
                        const answer = dialog.manager.remove(removal.pluginId)
                        if (answer.error) removal.problem = answer.error
                        else removal.close()
                    }
                }
            }
        }
    }
}
