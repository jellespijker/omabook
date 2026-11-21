import QtQuick

// Someone else's note (#85) on a card of its own beside its passage (#124),
// where the window has no room for the margin column: opened from its mark
// in the text (NoteMarks), under their name, to read and to keep as a fact,
// never to edit. Escape puts it away.
FocusScope {
    id: popup
    objectName: "othersNoteCard"
    // The room it stands in (its parent's size), and the layer the note is of (AnnotationLayerView).
    property size room: Qt.size(0, 0)
    property var annotationLayer: null
    property bool shown: false
    // {id, note, called, color, person, ink}, as AnnotationLayerView::notesIn gives it.
    property var note: null
    property rect around: Qt.rect(0, 0, 0, 0)
    signal leaveAsked()

    // Beside what it is about, a rectangle of its parent.
    function openAround(note, rect) {
        popup.note = note
        around = rect
        shown = true
        theirs.forceActiveFocus()
    }
    function close() {
        if (!shown) return
        shown = false
        note = null
    }
    visible: shown
    z: 8
    width: Math.min(360, room.width - 16)
    height: theirs.implicitHeight
    x: Math.max(8, Math.min(room.width - width - 8, around.x))
    y: {
        const below = around.y + around.height + 6
        const above = around.y - height - 6
        return Math.max(8, Math.min(room.height - height - 8, below + height <= room.height - 8 || above < 8 ? below : above))
    }
    Keys.onEscapePressed: popup.leaveAsked()
    // A press on the card is the card's, not the page's under it.
    TapHandler { gesturePolicy: TapHandler.WithinBounds }
    Rectangle {
        anchors.fill: parent
        anchors.margins: -1
        radius: theme.radius
        color: theme.background
        border.color: theme.colors.selection
    }
    OthersNoteCard {
        id: theirs
        objectName: "othersNote"
        width: parent.width
        note: popup.note
        annotationLayer: popup.annotationLayer
        lines: 0
        fontSize: Math.max(13, reader.format.noteFontSize)
    }
}
