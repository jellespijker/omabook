import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omabook

// The open book's margin notes (#82), over the page: the notes themselves
// (MarginNotes), the one being written, and the ways to them. Where the
// window has room for the margin column, notes stand in it beside their
// passages and are edited where they stand; where it hasn't, a mark at the
// end of each passage opens its note on a card beside it. The keyboard comes
// first: Shift+N writes a note on the selected words (selecting them first
// when there are none), E goes through the notes on the page (Up and Down,
// Enter edits, Delete removes, Escape goes back to the page). Notes whose
// passages can't be found in the book any more are listed, to be placed
// again on the words selected, or removed.
Item {
    id: layer
    objectName: "readingNotes"
    // The reading view, whose page (`content`) shows the text.
    property Item host
    readonly property alias notes: model
    readonly property var content: host ? host.content : null
    // The note the keyboard or the pointer is on (its number), 0 for none.
    property var current: 0
    // The note being written: {start, end} of the chapter `draftSection`
    // shows, in the characters of the page's text; null for none.
    property var draft: null
    property int draftSection: -1
    readonly property bool draftHere: !!draft && draftSection === reader.section
    // Whether the notes stand in a margin column here, or are marks in the text.
    readonly property bool beside: reader.format.noteWidth > 0
    // The keyboard is going through the notes on the page.
    property bool going: false
    // While it is, a card is up, or the lost ones are listed, the keys are the notes'.
    readonly property bool focused: going || card.shown || lostList.opened || writing
    readonly property bool writing: !!draft || editing !== 0
    // The note whose words are being changed, 0 for none.
    property var editing: 0

    // The window's reader, by a name of the layer's own: inside MarginNotes
    // `reader` is its own property.
    readonly property QtObject windowReader: reader
    MarginNotes { id: model; reader: layer.windowReader }
    // What another computer sent, once the library is read again after a sync.
    Connections {
        target: backend
        function onRevisionChanged() { model.refresh() }
    }
    Connections {
        target: reader
        function onSectionChanged() { layer.leave() }
        function onBookChanged() { layer.leave() }
    }

    // A note on the words selected; with none, they are selected first (V),
    // and the note is written once Enter says they are the ones.
    function write() {
        if (!content) return
        if (reader.kind !== "flow") {
            backend.say(qsTr("These pages are pictures, so there are no words to write a margin note on."))
            return
        }
        const chosen = typeof content.selectionSpan === "function" ? content.selectionSpan() : null
        if (!chosen) {
            host.startSelecting("note")
            return
        }
        content.clearSelection()
        card.close()
        going = false
        draftSection = reader.section
        draft = { start: chosen.start, end: chosen.end, text: chosen.text }
        model.drafting = true
        current = 0
        // Where there is no margin column, the card beside the words.
        if (!beside) Qt.callLater(() => layer.openDraft(chosen))
    }
    function openDraft(chosen) { card.openOn(0, "", chosen.text, chosen.start, chosen.end) }
    // Done writing or editing a note: the words kept, or none (removed, never written).
    function finish(id, words) {
        if (id === 0 && draft) {
            const made = words.trim() !== "" ? model.write(draft.text.textDocument, draft.start, draft.end, words) : 0
            draft = null
            model.drafting = false
            if (made > 0) {
                current = made
                backend.say(qsTr("Margin note written"))
            }
        } else if (id) {
            const removed = words.trim() === ""
            model.change(id, words)
            if (removed) backend.say(qsTr("Margin note removed"))
        }
        editing = 0
        card.close()
        back()
    }
    function removeNote(id) {
        if (id === 0) {
            finish(0, "")
            return
        }
        model.remove(id)
        editing = 0
        card.close()
        backend.say(qsTr("Margin note removed"))
        back()
    }
    // The keyboard back where it was: on the notes, while going through them, else the page.
    function back() {
        if (going && inSight().length > 0) keys.forceActiveFocus()
        else leave()
    }
    function leave() {
        // The keyboard goes back to the page only from the notes: a turn to
        // another chapter from the contents leaves it with them.
        const had = focused
        going = false
        current = 0
        if (draft) {
            draft = null
            model.drafting = false
        }
        editing = 0
        card.close()
        if (had && content && host.active) content.forceActiveFocus()
    }
    // The notes on the page, top to bottom: [{id, text, start, end}].
    function inSight() { return content && typeof content.notesInSight === "function" ? content.notesInSight() : [] }
    // A note opened from its mark in the text, or gone to with the keyboard.
    function open(id, text, start, end) {
        current = id
        if (beside && content && content.marginColumn) {
            const shown = content.marginColumn.cardOf(id)
            if (shown) {
                editing = id
                shown.edit()
                return
            }
        }
        const entry = model.notes.find(note => note.id === id)
        card.openOn(id, entry ? entry.note : "", text, start, end)
    }
    // E: the first note on the page, and from there the others.
    function go() {
        const seen = inSight()
        if (seen.length === 0) {
            backend.say(model.count > 0 ? qsTr("No margin notes on this page") : qsTr("No margin notes in this book yet"))
            return
        }
        going = true
        step(0)
    }
    function step(by) {
        const seen = inSight()
        if (seen.length === 0) {
            leave()
            return
        }
        let at = seen.findIndex(one => one.id === current)
        at = at < 0 ? 0 : Math.max(0, Math.min(seen.length - 1, at + by))
        current = seen[at].id
        if (beside) keys.forceActiveFocus()
        else card.openOn(seen[at].id, model.notes.find(note => note.id === seen[at].id).note, seen[at].text, seen[at].start, seen[at].end, true)
    }
    function editCurrent() {
        if (!current) return
        const seen = inSight().find(one => one.id === current)
        if (seen) open(current, seen.text, seen.start, seen.end)
    }

    // Going through the notes in the margin column.
    Item {
        id: keys
        objectName: "noteKeys"
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Down || event.key === Qt.Key_J || event.key === Qt.Key_Tab) layer.step(1)
            else if (event.key === Qt.Key_Up || event.key === Qt.Key_K || event.key === Qt.Key_Backtab) layer.step(-1)
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_E) layer.editCurrent()
            else if (event.key === Qt.Key_Delete || event.key === Qt.Key_D) layer.removeNote(layer.current)
            else if (event.key === Qt.Key_Escape) layer.leave()
            else return
            event.accepted = true
        }
        onActiveFocusChanged: if (!activeFocus && layer.going && !layer.writing && !card.shown) layer.going = false
    }

    // A note on a card beside its passage, where there is no margin column.
    FocusScope {
        id: card
        objectName: "noteCard"
        property bool shown: false
        property var noteId: 0
        property rect around: Qt.rect(0, 0, 0, 0)
        function openOn(id, words, text, start, end, browsing) {
            noteId = id
            note.words = words
            const a = text.positionToRectangle(start)
            const b = text.positionToRectangle(end)
            const at = text.mapToItem(layer, Math.min(a.x, b.x), a.y)
            around = Qt.rect(at.x, at.y, a.y === b.y ? Math.max(4, b.x - a.x) : text.width, b.y + b.height - a.y)
            shown = true
            if (id === 0 || !browsing) {
                layer.editing = id
                note.edit()
            } else {
                note.editing = false
                note.forceActiveFocus()
            }
        }
        function close() {
            if (!shown) return
            shown = false
            note.editing = false
        }
        visible: shown
        z: 8
        width: Math.min(360, layer.width - 16)
        height: note.implicitHeight
        x: Math.max(8, Math.min(layer.width - width - 8, around.x))
        y: {
            const below = around.y + around.height + 6
            const above = around.y - height - 6
            return Math.max(8, Math.min(layer.height - height - 8, below + height <= layer.height - 8 || above < 8 ? below : above))
        }
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Down || event.key === Qt.Key_J || event.key === Qt.Key_Tab) layer.step(1)
            else if (event.key === Qt.Key_Up || event.key === Qt.Key_K || event.key === Qt.Key_Backtab) layer.step(-1)
            else if (event.key === Qt.Key_Escape) layer.leave()
            else return
            event.accepted = true
        }
        // A press on the card is the card's, not the page's under it.
        TapHandler { gesturePolicy: TapHandler.WithinBounds }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -1
            radius: theme.radius
            color: theme.background
            border.color: theme.colors.selection
        }
        NoteCard {
            id: note
            width: parent.width
            noteId: card.noteId
            lines: 0
            current: layer.going && !editing
            fontSize: Math.max(13, reader.format.noteFontSize)
            onFinished: (words) => layer.finish(card.noteId, words)
            onRemoveAsked: layer.removeNote(card.noteId)
        }
    }

    // The notes whose passages are gone: how many, and the list of them.
    AppButton {
        id: lostChip
        objectName: "lostNotes"
        visible: model.lost.length > 0 && reader.open && !card.shown
        z: 3
        // In the corner where the lines start, under the text, clear of the
        // title line above and of the finger's menu button at the other corner.
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.margins: 12
        font.pixelSize: 12
        leftPadding: 10
        rightPadding: 10
        text: qsTr("%Ln margin notes lost their place", "", model.lost.length)
        focusPolicy: Qt.NoFocus
        onClicked: lostList.open()
    }
    AppPopup {
        id: lostList
        objectName: "lostNotesList"
        widest: 520
        onClosed: if (layer.content) layer.content.forceActiveFocus()
        DialogBody {
            Label {
                Layout.fillWidth: true
                text: qsTr("The words these notes were on are not in the book any more: a new edition, or another file. Select the words a note belongs to and place it there, or remove it.")
                wrapMode: Text.Wrap
                color: theme.colors.muted
            }
            Repeater {
                model: lostList.opened ? layer.notes.lost : []
                delegate: ColumnLayout {
                    id: lostNote
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("“%1”").arg(lostNote.modelData.quote)
                        font.italic: true
                        color: theme.colors.muted
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }
                    Label {
                        Layout.fillWidth: true
                        text: lostNote.modelData.note
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                    }
                    DialogActions {
                        AppButton {
                            objectName: "lostNoteRemove"
                            text: qsTr("Remove")
                            onClicked: layer.notes.remove(lostNote.modelData.id)
                        }
                        AppButton {
                            objectName: "lostNotePlace"
                            text: qsTr("Place on the selected words")
                            enabled: !!layer.content && typeof layer.content.selectionSpan === "function" && !!layer.content.selectionSpan()
                            onClicked: {
                                const chosen = layer.content.selectionSpan()
                                if (chosen && layer.notes.placeAgain(lostNote.modelData.id, chosen.text.textDocument, chosen.start, chosen.end))
                                    backend.say(qsTr("Margin note placed"))
                            }
                        }
                    }
                }
            }
            actions: [
                AppButton {
                    text: qsTr("Close")
                    primary: true
                    onClicked: lostList.close()
                }
            ]
        }
    }
}
