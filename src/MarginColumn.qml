import QtQuick

// The margin column (#82): the margin notes of what the page shows, beside
// the text, each at the height of its passage's first line; notes whose
// passages are close are stacked, each under the one before, none over
// another (PageLayout::stack). The column is part of the page's geometry,
// which the layout decides (PageLayout::columns); a page that is laid out
// again, wider, narrower or in another size of type, moves its notes with
// the lines they are on. Others' notes (#85, AnnotationLayerView) stand among
// them, each under its writer's name and in their colour, to read and to keep
// as a fact of the graph, never to edit.
Item {
    id: column
    objectName: "marginColumn"
    // The notes (ReadingNotes), and the TextEdit whose chapter they are on.
    property var reading: null
    property TextEdit text
    // Where a character of the text is in this column, top to bottom, or
    // NaN where the page doesn't show it (another page of the chapter).
    property var place: function(position) { return NaN }
    // The foot of the room, which notes are kept above; -1 for none (a column that scrolls).
    property real foot: -1
    // Something moved the text under the notes (the page that shows it, a
    // turn of a page); a number that changes says so.
    property real laid: 0
    // The text laid out again, which this hears of itself.
    property int relaid: 0
    readonly property real spacing: 8

    // The notes of the chapter on the page, and the one being written; and others'.
    property var spans: []
    property var otherSpans: []
    readonly property var others: reading ? reading.others : null
    readonly property var entries: {
        laid
        relaid
        const all = []
        for (const span of spans) {
            const top = place(span.start)
            if (!isNaN(top)) all.push({ id: span.id, start: span.start, end: span.end, note: span.note, ink: span.ink, top: top })
        }
        for (const span of otherSpans) {
            const top = place(span.start)
            if (!isNaN(top)) all.push({ id: "o:" + span.id, other: span, top: top })
        }
        // Only where the notes stand in the column: without one the note is
        // written on its card (ReadingNotes), and a second, hidden field for
        // it here would take the keyboard and close it unwritten (#157).
        if (reading && reading.beside && reading.draft && reading.draftHere) {
            const top = place(reading.draft.start)
            if (!isNaN(top)) all.push({ id: 0, start: reading.draft.start, end: reading.draft.end, note: "", ink: false, top: top })
        }
        return all.sort((a, b) => a.top - b.top)
    }
    // The notes shown, by their numbers (0 for the one being written): a
    // card stays the same card while the page is laid out again, so one
    // being edited keeps what was typed.
    property var ids: []
    // Where each stands, by its number, once each card's height is known.
    property var tops: ({})
    function entryOf(id) {
        for (const entry of entries) if (entry.id === id) return entry
        return null
    }
    // Others' notes shown, by their ids ("o:" and the note's in its space).
    property var otherIds: []
    function arrange() {
        const now = entries.filter(entry => !entry.other).map(entry => entry.id)
        if (now.length !== ids.length || now.some((id, i) => id !== ids[i])) ids = now
        const theirs = entries.filter(entry => !!entry.other).map(entry => entry.id)
        if (theirs.length !== otherIds.length || theirs.some((id, i) => id !== otherIds[i])) otherIds = theirs
        const wanted = []
        for (const entry of entries) {
            const card = cardOf(entry.id)
            wanted.push({ top: entry.top, height: card ? card.implicitHeight : 40 })
        }
        const stood = reading ? reading.notes.stack(wanted, spacing, foot) : []
        const placed = {}
        for (let i = 0; i < entries.length; ++i) placed[entries[i].id] = stood[i]
        tops = placed
    }
    Timer { id: soon; interval: 0; onTriggered: column.arrange() }
    onEntriesChanged: soon.restart()
    onFootChanged: soon.restart()
    onWidthChanged: soon.restart()
    function respan() {
        spans = reading && text ? reading.notes.spans(text.textDocument) : []
        otherSpans = others && others.shown && text ? others.notesIn(text.textDocument) : []
    }
    Timer { id: spanSoon; interval: 0; onTriggered: column.respan() }
    onTextChanged: spanSoon.restart()
    onReadingChanged: spanSoon.restart()
    Connections {
        target: column.text
        function onTextChanged() { spanSoon.restart() }
        function onContentHeightChanged() { column.relaid++ }
    }
    Connections {
        target: column.reading ? column.reading.notes : null
        function onChanged() { spanSoon.restart() }
    }
    Connections {
        target: column.others
        function onChanged() { spanSoon.restart() }
    }
    // The notes on the page, top to bottom, for the keyboard to go through.
    function shown() {
        const found = []
        for (const entry of entries) {
            const card = cardOf(entry.id)
            if (entry.id && !entry.other && card && card.visible) found.push(entry.id)
        }
        return found
    }
    function cardOf(id) {
        for (let i = 0; i < cards.count; ++i)
            if (cards.itemAt(i) && cards.itemAt(i).noteId === id) return cards.itemAt(i)
        for (let i = 0; i < otherCards.count; ++i)
            if (otherCards.itemAt(i) && otherCards.itemAt(i).cardId === id) return otherCards.itemAt(i)
        return null
    }

    Repeater {
        id: cards
        model: column.ids
        delegate: NoteCard {
            required property var modelData
            readonly property var entry: column.entryOf(modelData)
            objectName: modelData ? "marginNote_" + modelData : "marginNoteDraft"
            width: column.width
            y: column.tops[modelData] !== undefined ? column.tops[modelData] : (entry ? entry.top : 0)
            // A note that runs past the foot of a page stays out of sight
            // until the keyboard goes to it, or the page turns.
            visible: !!entry && (column.foot < 0 || y + height <= column.foot + 0.5 || current || editing)
            noteId: modelData
            words: entry ? entry.note : ""
            ink: !!entry && !!entry.ink
            notes: column.reading ? column.reading.notes : null
            current: !!column.reading && column.reading.current === modelData && modelData !== 0
            lines: column.foot < 0 ? 8 : 4
            fontSize: reader.format.noteFontSize
            onImplicitHeightChanged: soon.restart()
            onFinished: (words, changed) => column.reading.finish(noteId, words, changed)
            onRemoveAsked: column.reading.removeNote(noteId)
            onActivated: {
                column.reading.current = noteId
                // Its tools' keys are the note's while it is edited, not the page's.
                if (ink) column.reading.editing = noteId
            }
            Component.onCompleted: if (noteId === 0 || (column.reading && column.reading.editing === noteId)) edit()
        }
    }

    Repeater {
        id: otherCards
        model: column.otherIds
        delegate: OthersNoteCard {
            required property var modelData
            readonly property var entry: column.entryOf(modelData)
            readonly property string cardId: modelData
            objectName: "othersNote"
            width: column.width
            y: column.tops[modelData] !== undefined ? column.tops[modelData] : (entry ? entry.top : 0)
            visible: !!entry && (column.foot < 0 || y + height <= column.foot + 0.5)
            note: entry ? entry.other : null
            annotationLayer: column.others
            lines: column.foot < 0 ? 8 : 4
            fontSize: reader.format.noteFontSize
            onImplicitHeightChanged: soon.restart()
        }
    }
}
