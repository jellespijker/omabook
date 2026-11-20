import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One box for getting around the chronology, as the graph has one: type a
// few letters of an event's name to go to it ("find"), or of a timeline, a
// book, a series, a collection, or a character, place, group or theme to
// show only what is on it or involves it ("filter"). Enter takes the first match.
AppPopup {
    id: chooser
    objectName: "chronologyPalette"
    padding: 16
    property var browser
    // "find" or "filter".
    property string mode: "find"
    property var entries: []
    signal picked(var id)

    readonly property var matching: {
        const typed = field.text.trim().toLowerCase()
        if (mode === "find") {
            chooser.browser.shownCount
            return chooser.browser.find(typed).map(event => ({ id: event.id, label: event.name, detail: event.detail }))
        }
        const wanted = typed.split(" ").filter(word => word !== "")
        return entries.filter(entry => {
            const text = (chooser.kindName(entry) + " " + entry.label).toLowerCase()
            return wanted.length === 0 ? entry.common : wanted.every(word => text.includes(word))
        })
    }
    function kindName(entry) { return entry.mark ? words.kind(entry.mark) : entry.kindName }

    function show(what) {
        mode = what
        entries = what === "filter" ? browser.choices() : []
        field.text = ""
        list.currentIndex = 0
        open()
        field.forceActiveFocus()
    }
    function take(entry) {
        if (entry === undefined) return
        close()
        if (mode === "find") chooser.picked(entry.id)
        else browser.setFilter(entry.act, entry.value, !entry.on)
    }

    GraphWords { id: words }

    ColumnLayout {
        spacing: 8
        // A popup is not an Item and doesn't inherit the window's mirroring.
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        TextField {
            id: field
            objectName: "chronologyPaletteField"
            Layout.preferredWidth: Math.min(460, win.width - 2 * win.dialogEdge - 2 * chooser.padding)
            placeholderText: chooser.mode === "filter" ? qsTr("Show") : qsTr("Find")
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
            objectName: "chronologyPaletteList"
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
                        Layout.preferredWidth: 72
                        text: chooser.mode === "filter" ? chooser.kindName(entry.modelData) || "" : ""
                        color: Qt.alpha(theme.foreground, 0.6)
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    GraphKind {
                        visible: !!entry.modelData.mark || chooser.mode === "find"
                        kind: entry.modelData.mark || "event"
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
                        visible: chooser.mode === "find"
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
            text: chooser.mode === "filter" ? qsTr("Nothing here goes by that") : qsTr("No event goes by that")
            color: theme.colors.muted
        }
    }
}
