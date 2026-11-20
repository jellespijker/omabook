import QtQuick
import QtQuick.Controls

// The events of which neither a time nor an order is known, listed apart
// under the lanes: they can't be put anywhere on them, and are not hidden.
Rectangle {
    id: strip
    property var browser
    signal picked(var id)
    readonly property string runs: "chronology.unplaced"
    height: (win.touch ? 44 : 34) + 30
    color: theme.background

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: theme.colors.selection
    }
    Label {
        id: heading
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 6
        text: qsTr("No time or order known: %Ln", "", strip.browser.unplaced.length)
        color: theme.colors.muted
        font.pixelSize: 12
        font.bold: true
        elide: Text.ElideRight
    }
    ListView {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: 4
        height: win.touch ? 40 : 30
        orientation: ListView.Horizontal
        spacing: 6
        clip: true
        model: strip.browser.unplaced
        delegate: Rectangle {
            id: entry
            required property var modelData
            readonly property bool current: modelData.id === strip.browser.selected
            width: Math.min(name.implicitWidth + 20, 240)
            height: list.height
            radius: theme.radius
            color: current || hover.hovered ? theme.colors.selection : "transparent"
            border.color: theme.colors.selection
            Label {
                id: name
                anchors.centerIn: parent
                width: Math.min(implicitWidth, entry.width - 20)
                text: entry.modelData.name
                font.pixelSize: 13
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
            HoverHandler { id: hover }
            TapHandler { onTapped: strip.picked(entry.modelData.id) }
        }
    }
}
