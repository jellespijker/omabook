import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One margin note (#82): its words, set small, as they stand in the margin
// column or on the card a mark in the text opens; and, being edited, a field
// in the same place. The keyboard edits it first: Enter (or E) to edit,
// Escape or Ctrl+Enter when done, Delete to remove it; a note left with no
// words is removed too. A click or a tap edits it as well.
//
// A note written by hand (#90) shows its ink instead (NoteInk), which the pen
// writes on wherever it stands; edited, Ctrl+Z and Ctrl+Shift+Z undo and redo
// a pause of the pen, X takes the eraser, W shows its words.
FocusScope {
    id: card
    // The note's number; 0 for one being written on a passage that has none yet.
    property var noteId: 0
    property string words: ""
    // Written by hand, and the notes (MarginNotes) its ink is kept by.
    property bool ink: false
    property var notes: null
    property bool editing: false
    // The keyboard is on it, going through the notes.
    property bool current: false
    // How many lines it shows while it isn't edited; 0 for all of them.
    property int lines: 6
    property int fontSize: 13
    // Done editing, with these words (none: removed, or never written).
    signal finished(string words)
    signal removeAsked()
    // Pressed while not being edited.
    signal activated()

    implicitHeight: frame.implicitHeight
    function edit() {
        editing = true
        if (ink) {
            card.forceActiveFocus()
            return
        }
        field.text = words
        field.forceActiveFocus()
        field.cursorPosition = field.length
    }
    function finish() {
        if (!editing) return
        editing = false
        finished(ink ? words : field.text)
    }
    onEditingChanged: if (editing && !ink && !field.activeFocus) field.forceActiveFocus()

    Keys.onPressed: (event) => {
        if (editing && ink && inkLoader.item) inkKeys(event)
        if (editing) return
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_E) edit()
        else if (event.key === Qt.Key_Delete || event.key === Qt.Key_D) removeAsked()
        else return
        event.accepted = true
    }
    function inkKeys(event) {
        const ctrl = event.modifiers & Qt.ControlModifier
        const shift = event.modifiers & Qt.ShiftModifier
        const inked = inkLoader.item
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) finish()
        else if (ctrl && (event.key === Qt.Key_Y || (event.key === Qt.Key_Z && shift))) inked.redo()
        else if (ctrl && event.key === Qt.Key_Z) inked.undo()
        else if (event.key === Qt.Key_X) inked.toggleEraser()
        else if (event.key === Qt.Key_W) inked.toggleWords()
        else if (event.key === Qt.Key_Delete) removeAsked()
        else return
        event.accepted = true
    }

    Rectangle {
        id: frame
        width: parent.width
        // A finger's height at least, to be tapped.
        implicitHeight: Math.max(win.touch ? 40 : 0, body.implicitHeight + 12)
        radius: Math.min(6, theme.radius)
        color: card.editing ? theme.background : Qt.alpha(theme.colors.selection, card.current || hover.hovered ? 0.55 : 0.28)
        border.width: card.editing || card.current ? 1 : 0
        border.color: card.current && !card.editing ? theme.foreground : theme.accent
        // Its passage's colour along the edge that faces the text.
        Rectangle {
            width: 2
            height: parent.height - 8
            y: 4
            x: win.rightToLeft ? parent.width - width : 0
            radius: 1
            color: theme.accent
            opacity: 0.8
        }

        ColumnLayout {
            id: body
            x: 10
            y: 6
            width: parent.width - 16
            spacing: 6
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Loader {
                id: inkLoader
                Layout.fillWidth: true
                active: card.ink
                visible: active
                sourceComponent: NoteInk {
                    notes: card.notes
                    noteId: card.noteId
                    editing: card.editing
                    words: card.words
                    onDone: card.finish()
                    onRemoveAsked: {
                        card.editing = false
                        card.removeAsked()
                    }
                }
            }
            Label {
                objectName: "noteWords"
                Layout.fillWidth: true
                visible: !card.editing && !card.ink
                text: card.words
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                maximumLineCount: card.current || card.lines <= 0 ? 1000 : card.lines
                elide: Text.ElideRight
                font.pixelSize: card.fontSize
                lineHeight: 1.15
                color: theme.foreground
            }
            TextArea {
                id: field
                objectName: "noteField"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(implicitHeight, win.touch ? 40 : 0)
                visible: card.editing && !card.ink
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.PlainText
                font.pixelSize: card.fontSize
                placeholderText: qsTr("A margin note")
                leftPadding: 2
                rightPadding: 2
                topPadding: 4
                bottomPadding: 4
                background: null
                color: theme.foreground
                Keys.onEscapePressed: card.finish()
                Keys.onPressed: (event) => {
                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) {
                        card.finish()
                        event.accepted = true
                    }
                }
                // Clicked away from, it is done.
                onActiveFocusChanged: if (!activeFocus && card.editing && !buttons.pressing) card.finish()
            }
            // For a pointer or a finger: done, or away with it.
            RowLayout {
                id: buttons
                visible: card.editing && !card.ink
                Layout.fillWidth: true
                spacing: 6
                readonly property bool pressing: done.pressed || removal.pressed
                Item { Layout.fillWidth: true }
                AppButton {
                    id: removal
                    objectName: "noteRemove"
                    text: qsTr("Remove")
                    focusPolicy: Qt.NoFocus
                    leftPadding: 10
                    rightPadding: 10
                    onClicked: {
                        card.editing = false
                        card.removeAsked()
                    }
                }
                AppButton {
                    id: done
                    objectName: "noteDone"
                    text: qsTr("Done")
                    primary: true
                    focusPolicy: Qt.NoFocus
                    leftPadding: 10
                    rightPadding: 10
                    onClicked: card.finish()
                }
            }
        }
        HoverHandler { id: hover; cursorShape: card.editing ? Qt.IBeamCursor : Qt.PointingHandCursor }
        TapHandler {
            enabled: !card.editing
            onTapped: {
                card.activated()
                card.edit()
            }
        }
    }
}
