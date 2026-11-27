import QtQuick

// For fingers: the one button that is always over the page, which shows the
// bar of the reading commands (ReaderBar) and puts it away. It stands on a
// ground of its own with an edge in the text's colour, so it is told from any
// page, white or dark, and no words of the page show through it (#269).
AppButton {
    id: button
    // The reading view it works.
    property Item host
    width: 52
    height: 52
    // The whole of it is its ground, not Material's lower strip of it.
    topInset: 0
    bottomInset: 0
    focusPolicy: Qt.NoFocus
    text: host.showBar ? "✕" : "☰"
    Accessible.name: qsTr("More")
    font.pixelSize: 20
    onClicked: host.showBar = !host.showBar

    background: Rectangle {
        objectName: "menuButtonGround"
        radius: theme.radius
        color: button.down || button.hovered ? theme.colors.selection : theme.background
        border.color: theme.foreground
    }
}
