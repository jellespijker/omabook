import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A flat button with the window's corners. The primary one is filled with
// the accent and captioned in whichever of black or white reads on it; the
// others are outlined. It draws its own background: Material's sits in a
// shadow layer, which is more depth than this desktop has.
Button {
    id: button
    property bool primary: false
    // The commands this button is the pointer's way to, by their ids
    // ("library.add"), for the test that every command has one.
    property string runs: ""

    highlighted: primary
    flat: !primary
    leftPadding: 16
    rightPadding: 16
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    background: Rectangle {
        implicitWidth: 64
        implicitHeight: 36
        radius: theme.radius
        color: !button.enabled ? "transparent"
               : button.primary ? (button.down ? Qt.darker(theme.accent, 1.15) : theme.accent)
               : button.down || button.hovered ? theme.colors.selection : "transparent"
        border.width: button.visualFocus ? 2 : 1
        border.color: button.visualFocus ? theme.foreground
                      : button.primary && button.enabled ? theme.accent : theme.colors.selection
    }

    contentItem: Label {
        text: button.text
        font: button.font
        color: !button.enabled ? theme.colors.muted : button.primary ? theme.accentForeground : theme.foreground
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
