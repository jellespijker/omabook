import QtQuick

// What one TextEdit of the page adds to reading: the graph's things marked
// in its chapter, what is under the pointer, and the selection, made with
// the mouse, a finger or the keyboard. It lies over the text and takes no
// clicks of its own, so selecting and following links work as they did.
Item {
    id: helper
    // The TextEdit it serves.
    property TextEdit text
    // Whether the things are marked.
    property bool on: false
    // The thing under the pointer, as ReadingMarks.at gives it with `rect`
    // (in the text's coordinates), or null.
    property var hovered: null
    // A press of the mouse ended: on selected text, or on a marked thing.
    signal released()

    // Marking waits for the text to be set: a timer of its own, which goes
    // with this item.
    function lay() {
        if (!text) return
        if (on) reader.marks.mark(text.textDocument)
        else reader.marks.unmark(text.textDocument)
    }
    Timer { id: soon; interval: 0; onTriggered: helper.lay() }
    onOnChanged: soon.restart()
    onTextChanged: soon.restart()
    Component.onCompleted: soon.restart()
    Connections {
        target: helper.text
        // Set anew: another chapter, or the type changed. Marking it changes
        // its formats too, which is noticed and costs one comparison.
        function onTextChanged() { soon.restart() }
    }
    Connections {
        target: reader.marks
        function onChanged() { soon.restart() }
        function onLookChanged() { soon.restart() }
    }

    // A rectangle of the text from one position to another, on the first line.
    function span(start, end) {
        const a = text.positionToRectangle(start)
        const b = text.positionToRectangle(end)
        return a.y === b.y ? Qt.rect(a.x, a.y, Math.max(4, b.x - a.x), a.height) : Qt.rect(a.x, a.y, Math.max(4, text.width - a.x), a.height)
    }
    // The thing marked at a point of the text, with where it is; or null.
    function thingAt(x, y) {
        if (!on || !text) return null
        const position = text.positionAt(x, y)
        const found = reader.marks.at(text.textDocument, position)
        if (!found.id) return null
        const rect = span(found.start, found.end)
        // positionAt answers the nearest place even beside the text.
        if (x < rect.x - 2 || x > rect.x + rect.width + 2 || y < rect.y - 2 || y > rect.y + rect.height + 2) return null
        found.rect = rect
        return found
    }
    // The selection: its words, the paragraph it starts in, and where it is.
    function selection() {
        if (!text || text.selectedText === "") return null
        const a = text.positionToRectangle(text.selectionStart)
        const b = text.positionToRectangle(text.selectionEnd)
        return {
            text: text.selectedText,
            paragraph: reader.marks.paragraph(text.textDocument, text.selectionStart),
            rect: Qt.rect(Math.min(a.x, b.x), a.y, a.y === b.y ? Math.max(4, b.x - a.x) : text.width, b.y + b.height - a.y)
        }
    }
    function selectWordAt(x, y) {
        const word = reader.marks.wordAt(text.textDocument, text.positionAt(x, y))
        if (word.end <= word.start) return false
        text.select(word.start, word.end)
        return true
    }
    // The keyboard's selection: a word to start with, then words or lines
    // on (or back), the start staying where it was when it is extended.
    property int anchorAt: -1
    property int activeAt: -1
    function begin(position) {
        const word = reader.marks.wordAt(text.textDocument, position)
        anchorAt = word.start
        activeAt = word.end
        text.select(anchorAt, activeAt)
    }
    function step(words, lines, extend) {
        if (anchorAt < 0) return
        const doc = text.textDocument
        let at = words > 0 || extend ? activeAt : anchorAt
        if (words !== 0) at = reader.marks.wordFrom(doc, at, words)
        if (lines !== 0) {
            const now = text.positionToRectangle(at)
            at = text.positionAt(now.x + 1, lines > 0 ? now.y + now.height * 1.5 : now.y - now.height * 0.5)
        }
        const word = reader.marks.wordAt(doc, at)
        const forward = words > 0 || lines > 0
        if (extend) {
            activeAt = forward ? word.end : at
        } else {
            anchorAt = word.start
            activeAt = word.end
        }
        text.select(Math.min(anchorAt, activeAt), Math.max(anchorAt, activeAt))
    }
    function finish() {
        anchorAt = -1
        activeAt = -1
    }
    // Where the keyboard's selection is, to keep it in sight.
    function activeRect() { return text.positionToRectangle(activeAt >= 0 ? activeAt : text.selectionEnd) }

    HoverHandler {
        id: pointer
        cursorShape: helper.text && helper.text.hoveredLink !== "" || helper.hovered ? Qt.PointingHandCursor : Qt.IBeamCursor
        onPointChanged: helper.hovered = pointer.hovered ? helper.thingAt(point.position.x, point.position.y) : null
        onHoveredChanged: if (!hovered) helper.hovered = null
    }
    PointHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        onActiveChanged: if (!active) helper.released()
    }
}
