import QtQuick
import QtQuick.Controls

// A filter that is on, as a chip that takes it off; or, without the cross,
// one that does something to all of them.
Rectangle {
    id: chip
    property string text
    property string runs: ""
    // The most it can be wide: its words give way to fit.
    property real room: 400
    property bool removes: true
    signal tapped()
    width: Math.min(chipRow.implicitWidth + 20, room)
    height: win.touch ? 40 : 28
    radius: theme.radius
    color: hover.hovered ? theme.colors.selection : "transparent"
    border.color: theme.colors.selection
    Accessible.role: Accessible.Button
    Accessible.name: removes ? qsTr("Take off %1").arg(text) : text

    Row {
        id: chipRow
        anchors.centerIn: parent
        spacing: 6
        Label {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, chip.room - 20 - (cross.visible ? cross.width + 6 : 0))
            text: chip.text
            font.pixelSize: 13
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
        Glyph {
            id: cross
            visible: chip.removes
            anchors.verticalCenter: parent.verticalCenter
            name: "cross"
            size: 10
            color: theme.colors.muted
        }
    }
    HoverHandler { id: hover }
    TapHandler { onTapped: chip.tapped() }
}
