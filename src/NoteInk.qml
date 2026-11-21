import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omabook

// A margin note written by hand (#90), on its card: its ink, as wide as the
// card whatever the card's width (MarginNotes.inkUnits across), with room
// below it to write on. The pen writes on it wherever it stands, its other
// end erases; while the note is edited the mouse writes too, a finger as the
// writing bar has it (#134, ReadingInk.fingers), two fingers scroll, and its
// tools are there: undo and redo, the pen or the eraser, its words,
// what was read of the handwriting, shown on request (and read then, if they
// weren't yet), and done or away with it, wrapped as the column is narrow.
// Each pause of the pen is kept (MarginNotes.ink).
ColumnLayout {
    id: inked
    // The notes (MarginNotes) and this one's number.
    property var notes: null
    property var noteId: 0
    // Being edited: the tools shown, a finger and the mouse writing.
    property bool editing: false
    // Shown, not written on (a lost note in the list of them).
    property bool readOnly: false
    // What was read of it.
    property string words: ""
    property bool showWords: false
    readonly property real unit: width / (notes ? notes.inkUnits : 240)
    // The lowest of its strokes, in ink units.
    property real lowest: 0
    spacing: 2
    // Done with it, or away with it, from its tools.
    signal done()
    signal removeAsked()

    function sameStrokes(a, b) {
        return Object.keys(a).sort().join(",") === Object.keys(b).sort().join(",")
    }
    // What is kept, shown, unless it is what is shown: so writing here keeps its undo.
    function reload() {
        if (!notes || !noteId) return
        // What waits for the pen's pause goes first, or it would be shown away.
        surface.flush()
        const kept = notes.strokes(noteId)
        if (!sameStrokes(kept, surface.strokes())) surface.load(kept)
        lowest = notes.inkBottom(surface)
    }
    function undo() { surface.undo() }
    function redo() { surface.redo() }
    function toggleEraser() { surface.tool = surface.tool === InkSurface.Eraser ? InkSurface.Pen : InkSurface.Eraser }
    function toggleWords() {
        showWords = !showWords
        if (showWords && words === "" && notes) notes.recognize(noteId)
    }
    function flush() { surface.flush() }
    Component.onCompleted: reload()
    onNoteIdChanged: reload()
    onEditingChanged: if (!editing) {
        surface.flush()
        surface.tool = InkSurface.Pen
    }
    Connections {
        target: inked.notes || null
        ignoreUnknownSignals: true
        function onChanged() { if (!surface.writing) inked.reload() }
    }

    InkSurface {
        id: surface
        objectName: "noteInk"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(48, inked.lowest + (inked.readOnly ? 6 : 36)) * inked.unit
        inkScale: inked.unit
        readOnly: inked.readOnly
        // A blue that reads on a light page and a dark one alike: the ink keeps it.
        color: "#3f73c9"
        penWidth: 1.6
        eraserWidth: 16
        batchPause: 500
        fingers: inked.editing ? reader.ink.fingerUse : InkSurface.Scroll
        mouseDraws: inked.editing
        Accessible.role: Accessible.Graphic
        Accessible.name: inked.words !== "" ? qsTr("Handwritten margin note: %1").arg(inked.words) : qsTr("Handwritten margin note")
        onStrokesChanged: inked.lowest = inked.notes ? inked.notes.inkBottom(surface) : 0
        onInked: (batch) => { if (inked.notes && inked.noteId) inked.notes.ink(inked.noteId, batch) }
        // Where the ink may go on, a line under the writing, faintly.
        Rectangle {
            visible: !inked.readOnly && (inked.editing || surface.penNear)
            x: 0
            width: parent.width
            y: Math.max(40, inked.lowest + 28) * inked.unit
            height: 1
            color: theme.colors.selection
        }
    }
    Label {
        objectName: "noteInkWords"
        Layout.fillWidth: true
        visible: inked.showWords
        text: {
            if (inked.words !== "") return inked.words
            if (!inked.notes) return ""
            inked.notes.readings
            const state = inked.notes.readingOf(inked.noteId)
            return state !== "" ? state : qsTr("Not read yet")
        }
        color: inked.words !== "" ? theme.foreground : theme.colors.muted
        font.italic: inked.words === ""
        font.pixelSize: 12
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
    // A tool: as small as a finger allows (NoteCard's 40 pixels), as the column is narrow and a page short.
    component Tool: AppButton {
        focusPolicy: Qt.NoFocus
        leftPadding: 8
        rightPadding: 8
        topInset: 0
        bottomInset: 0
        implicitHeight: win.touch ? 40 : 32
    }
    // Its tools, while it is edited, wrapped in a narrow column.
    Flow {
        Layout.fillWidth: true
        visible: inked.editing && !inked.readOnly
        spacing: 2
        Tool {
            objectName: "noteInkUndo"
            text: qsTr("Undo")
            enabled: surface.canUndo
            onClicked: surface.undo()
        }
        Tool {
            objectName: "noteInkRedo"
            text: qsTr("Redo")
            enabled: surface.canRedo
            onClicked: surface.redo()
        }
        Tool {
            objectName: "noteInkEraser"
            text: qsTr("Eraser")
            primary: surface.tool === InkSurface.Eraser
            onClicked: inked.toggleEraser()
        }
        Tool {
            objectName: "noteInkWordsButton"
            text: inked.showWords ? qsTr("Hide words") : qsTr("Words")
            onClicked: inked.toggleWords()
        }
        Tool {
            objectName: "noteRemove"
            text: qsTr("Remove")
            onClicked: inked.removeAsked()
        }
        Tool {
            objectName: "noteDone"
            text: qsTr("Done")
            primary: true
            onClicked: inked.done()
        }
    }
}
