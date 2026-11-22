import QtQuick
import QtQuick.Controls

// The small button at the margin's edge that folds the margin column away
// and brings it back (#167), as {reader.fold_margin} and the Aa panel do,
// for every book (Settings::marginFolded). It shows where the book's notes
// have a column, or would have, folded away; its arrow points the way the
// column goes. The reader places it.
AbstractButton {
    id: fold
    objectName: "marginFold"
    readonly property bool folded: reader.format.marginFolded
    // A finger's tap on it is its own, not the page's under it (ReaderTouch, #141).
    readonly property bool takesTaps: true
    readonly property string said: folded ? qsTr("Unfold the margin notes") : qsTr("Fold the margin notes away")
    visible: reader.kind === "flow" && (reader.format.noteWidth > 0 || folded)
    width: win.touch ? 36 : 28
    height: width
    // The keyboard has its key for it.
    focusPolicy: Qt.NoFocus
    Accessible.role: Accessible.Button
    Accessible.name: said
    ToolTip.visible: hovered
    ToolTip.delay: 500
    ToolTip.text: said
    onClicked: notes.fold()
    // The reading notes (ReadingNotes), which fold it.
    property var notes: null

    background: Rectangle {
        radius: theme.radius
        color: fold.down || fold.hovered ? theme.colors.selection : Qt.alpha(theme.background, 0.85)
        border.color: theme.colors.selection
    }
    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            // The column is at the end the lines end at: it goes that way, and comes back from it.
            name: fold.folded !== win.rightToLeft ? "back" : "forward"
            size: 16
            color: theme.foreground
        }
    }
}
