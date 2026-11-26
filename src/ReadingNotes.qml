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
//
// With the pen (#90), writing on the text that isn't a highlight's gesture,
// or in the margin column, writes a note by hand on the line beside it: its
// first strokes are what the pen wrote there, and the pen goes on writing on
// its card, which stands in the column (or, without one, beside the line).
//
// On the pages of a PDF or a comic (#115), which stand still, a note is on
// words of a PDF's page or on a part of a comic's (a panel, chosen as words
// are selected), marked where it is (PageNotes) and opened on a card beside it.
//
// Others' notes (#85) stand in the margin column beside one's own; where
// there is none, each is marked in the text in its writer's colour and opens
// on a card of its own beside its passage (#124), to read, never to edit.
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
    readonly property bool beside: reader.kind === "flow" && reader.format.noteWidth > 0
    // The keyboard is going through the notes on the page.
    property bool going: false
    // While it is, a card is up, or the lost ones are listed, the keys are the notes'.
    readonly property bool focused: going || card.shown || othersCard.shown || lostList.opened || writing
    readonly property bool writing: !!draft || editing !== 0
    // The note whose words are being changed, 0 for none.
    property var editing: 0

    // The window's reader, by a name of the layer's own: inside MarginNotes
    // `reader` is its own property.
    readonly property QtObject windowReader: reader
    readonly property QtObject windowSettings: settings
    MarginNotes { id: model; reader: layer.windowReader; settings: layer.windowSettings; othersWanted: othersModel.notes > 0 }
    // Others' annotations on the book (#85): their highlights under the
    // words, their notes in the margin column, as far as the reader has read.
    readonly property alias others: othersModel
    AnnotationLayerView { id: othersModel; reader: layer.windowReader; settings: layer.windowSettings }
    // What another computer sent, once the library is read again after a sync.
    Connections {
        target: backend
        function onRevisionChanged() {
            model.refresh()
            othersModel.refresh()
        }
    }
    // Where the column comes, others' notes stand in it, no more on a card.
    onBesideChanged: othersCard.close()
    Connections {
        target: reader
        function onSectionChanged() { layer.leave() }
        function onBookChanged() { layer.leave() }
    }

    // A note on the words selected; with none, they are selected first (V),
    // and the note is written once Enter says they are the ones.
    function write() {
        if (!content) return
        if (reader.kind !== "flow") return writeOnPage()
        const chosen = typeof content.selectionSpan === "function" ? content.selectionSpan() : null
        if (!chosen) {
            host.startSelecting("note")
            return
        }
        content.clearSelection()
        card.close()
        going = false
        draftSection = reader.section
        // Written on the page's `chapter`, a text that stays laid out: a page of a
        // spread can stop showing, its text emptied, as the margin column comes (#165).
        draft = { start: chosen.start, end: chosen.end, text: chosen.text, chapter: chosen.chapter || chosen.text }
        model.drafting = true
        current = 0
        // Where there is no margin column, the card beside the words.
        if (!beside) Qt.callLater(() => layer.openDraft(chosen))
    }
    // Where in a chapter's text (`chapter`, a QQuickTextDocument) the note
    // being written is, or the one the keyboard or the pen is on; -1 for none.
    function held(chapter) {
        if (draftHere) return draft.start
        const span = current ? model.spans(chapter).find(one => one.id === current) : null
        return span ? span.start : -1
    }
    function openDraft(chosen) { card.openOn(0, "", chosen.text, chosen.start, chosen.end) }
    // Of a TextEdit's characters from `start` to `end`, the rectangle in this layer they take.
    function around(text, start, end) {
        const a = text.positionToRectangle(start)
        const b = text.positionToRectangle(end)
        const at = text.mapToItem(layer, Math.min(a.x, b.x), a.y)
        return Qt.rect(at.x, at.y, a.y === b.y ? Math.max(4, b.x - a.x) : text.width, b.y + b.height - a.y)
    }
    // Someone else's note, from its mark in the text (NoteMarks), on its card.
    function openOther(note, text) {
        card.close()
        othersCard.openAround(note, layer.around(text, note.start, note.end))
    }
    // What the pen wrote that meant nothing to the highlights (PenGestures):
    // the strokes of `snapshot` that `which` counts, the first of them begun
    // at `at`, a point of the page (`content`). A note by hand on the line
    // there, when there is one: whether it was written.
    function penWrote(snapshot, which, at) {
        if (reader.kind !== "flow" || !content || typeof content.lineAt !== "function") return false
        const line = content.lineAt(at.x, at.y)
        if (!line) return false
        // As wide as its card will be: the column's, or the card's beside the line.
        const room = (beside ? reader.format.noteWidth : card.width) - 16
        const made = model.writeInk(line.text.textDocument, line.start, line.end, snapshot, which, model.inkUnits / Math.max(40, room))
        if (made <= 0) return false
        if (draft) {
            draft = null
            model.drafting = false
        }
        current = made
        backend.say(qsTr("Handwritten margin note written"))
        if (!beside) Qt.callLater(() => layer.open(made, line.text, line.start, line.end))
        return true
    }
    // On a page that stands still: the words selected on it, or the part of it chosen.
    function writeOnPage() {
        const chosen = typeof content.pageSelection === "function" ? content.pageSelection() : null
        if (!chosen) {
            host.startSelecting("note")
            return
        }
        content.clearSelection()
        card.close()
        going = false
        draftSection = chosen.page
        draft = chosen
        current = 0
        Qt.callLater(() => card.openAround(0, "", fromContent(chosen.rect)))
    }
    // A rectangle of the page (content) in this layer.
    function fromContent(rect) {
        const at = content.mapToItem(layer, rect.x, rect.y)
        return Qt.rect(at.x, at.y, rect.width, rect.height)
    }
    // The note being written, with these words: on a chapter's words, a page's, or a part of a page.
    function writeDraft(words) {
        if (words.trim() === "") return 0
        if (draft.region)
            return model.writeOnPart(draft.page, Qt.rect(draft.region.x, draft.region.y, draft.region.width, draft.region.height), draft.aspect, words)
        if (draft.page !== undefined) return model.writeOnPage(draft.page, draft.start, draft.end, words)
        return model.write(draft.chapter.textDocument, draft.start, draft.end, words)
    }
    // Done writing or editing a note: the words kept, or none (removed, never written).
    function finish(id, words, changed) {
        const entry = id ? model.notes.find(note => note.id === id) : null
        if (entry && entry.ink) {
            // Written by hand: its ink is kept as it goes, and its words are what was read of it.
            editing = 0
            card.close()
            back()
            return
        }
        if (id === 0 && draft) {
            const made = writeDraft(words)
            draft = null
            model.drafting = false
            if (made > 0) {
                current = made
                backend.say(qsTr("Margin note written"))
            }
        } else if (id && changed) {
            const removed = words.trim() === ""
            model.change(id, words)
            if (removed) backend.say(qsTr("Margin note removed"))
        }
        editing = 0
        card.close()
        back()
        // Done writing, the keyboard is the page's again, once the key that finished it is done.
        if (!going) Qt.callLater(() => { if (content && !writing && !focused) content.forceActiveFocus() })
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
        othersCard.close()
        if (had && content && host.active) content.forceActiveFocus()
    }
    // The notes on the page, top to bottom: [{id, text, start, end}].
    function inSight() { return content && typeof content.notesInSight === "function" ? content.notesInSight() : [] }
    // A note opened from its mark on a page that stands still, its rectangle the page's (content's).
    function openAt(id, rect) {
        current = id
        const entry = model.notes.find(note => note.id === id)
        card.openAround(id, entry ? entry.note : "", fromContent(rect), false, entry ? entry.ink : false)
    }
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
        card.openOn(id, entry ? entry.note : "", text, start, end, false, entry ? entry.ink : false)
    }
    // {reader.fold_margin}, the button at the margin's edge, the Aa panel (#167):
    // the margin column folded away for every book, a dot beside each line with notes, or back.
    function fold() {
        settings.marginFolded = !settings.marginFolded
        backend.say(settings.marginFolded ? qsTr("Margin notes folded away") : qsTr("Margin notes unfolded"))
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
        const entry = model.notes.find(note => note.id === seen[at].id)
        const words = entry.note
        if (beside) keys.forceActiveFocus()
        else if (seen[at].rect) card.openAround(seen[at].id, words, fromContent(seen[at].rect), true, entry.ink)
        else card.openOn(seen[at].id, words, seen[at].text, seen[at].start, seen[at].end, true, entry.ink)
    }
    function editCurrent() {
        if (!current) return
        const seen = inSight().find(one => one.id === current)
        if (seen && seen.rect) openAt(current, seen.rect)
        else if (seen) open(current, seen.text, seen.start, seen.end)
    }

    // A click or a finger's tap beside a note's card closes it (and ReaderTouch), the note kept (#167).
    function closeCard() {
        if (!card.shown) return false
        if (note.editing) note.finish()
        else leave()
        return true
    }
    TapHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        enabled: card.shown
        onTapped: (point) => { if (!card.contains(card.mapFromItem(layer, point.position))) layer.closeCard() }
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
        property bool ink: false
        property rect around: Qt.rect(0, 0, 0, 0)
        function openOn(id, words, text, start, end, browsing, handwritten) {
            openAround(id, words, layer.around(text, start, end), browsing, handwritten)
        }
        // Beside what it is about, a rectangle of this layer.
        function openAround(id, words, rect, browsing, handwritten) {
            noteId = id
            ink = !!handwritten
            note.words = words
            around = rect
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
            ink: card.ink
            notes: model
            lines: 0
            current: layer.going && !editing
            fontSize: Math.max(13, reader.format.noteFontSize)
            onFinished: (words, changed) => layer.finish(card.noteId, words, changed)
            onRemoveAsked: layer.removeNote(card.noteId)
        }
    }

    // Someone else's note on a card beside its passage, where there is no margin column (#124).
    OthersNotePopup {
        id: othersCard
        room: Qt.size(layer.width, layer.height)
        annotationLayer: othersModel
        onLeaveAsked: layer.leave()
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
        // The words selected, a chapter's or a page's, to place a lost note on; null for none.
        function selectedWords() {
            const content = layer.content
            if (!content) return null
            if (typeof content.selectionSpan === "function") return content.selectionSpan()
            const chosen = typeof content.pageSelection === "function" ? content.pageSelection() : null
            return chosen && !chosen.region ? chosen : null
        }
        function placeOnSelected(id) {
            const chosen = selectedWords()
            if (!chosen) return false
            return chosen.page !== undefined ? layer.notes.placeAgainOnPage(id, chosen.page, chosen.start, chosen.end)
                                             : layer.notes.placeAgain(id, chosen.text.textDocument, chosen.start, chosen.end)
        }
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
                        visible: !lostNote.modelData.ink
                        text: lostNote.modelData.note
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                    }
                    Loader {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 360
                        active: !!lostNote.modelData.ink
                        visible: active
                        sourceComponent: NoteInk {
                            readOnly: true
                            notes: model
                            noteId: lostNote.modelData.id
                            words: lostNote.modelData.note
                        }
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
                            enabled: !!lostList.selectedWords()
                            onClicked: if (lostList.placeOnSelected(lostNote.modelData.id)) backend.say(qsTr("Margin note placed"))
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
