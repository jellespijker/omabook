import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The way back, at the start of a header, for a pointer or a finger: an
// arrow with where it goes back to where the header is wide, the arrow
// alone where it isn't. The graph's and the chronology's.
Rectangle {
    id: back
    // Where it goes back to, in words.
    property string text
    // The arrow alone.
    property bool compact: false
    // The command it is the pointer's way to (see AppButton).
    property string runs: ""
    signal tapped()

    Layout.preferredWidth: Math.max(row.implicitWidth + 16, height)
    Layout.preferredHeight: win.touch ? 40 : 30
    radius: theme.radius
    color: hover.hovered ? theme.colors.selection : "transparent"
    Accessible.role: Accessible.Button
    Accessible.name: back.text
    Accessible.onPressAction: back.tapped()
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 4
        Glyph {
            anchors.verticalCenter: parent.verticalCenter
            name: win.rightToLeft ? "forward" : "back"
            size: 14
            color: theme.colors.muted
        }
        Label {
            visible: !back.compact
            text: back.text
            color: theme.colors.muted
        }
    }
    HoverHandler { id: hover }
    TapHandler { onTapped: back.tapped() }
}
