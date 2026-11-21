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
    // Where the last press of the mouse was, in the text's coordinates.
    property point pressedAt: Qt.point(-1, -1)

    // Marking waits for the text to be set: a timer of its own, which goes
    // with this item.
    function lay() {
        if (!text) return
        if (on) reader.marks.mark(text.textDocument)
        else reader.marks.unmark(text.textDocument)
    }
    Timer { id: soon; interval: 0; onTriggered: helper.lay() }
    onOnChanged: soon.restart()
    onTextChanged: { soon.restart(); lightSoon.restart() }
    Component.onCompleted: soon.restart()
    Connections {
        target: helper.text
        // Set anew: another chapter, or the type changed. Marking it changes
        // its formats too, which is noticed and costs one comparison.
        function onTextChanged() { soon.restart(); lightSoon.restart() }
    }
    Connections {
        target: reader.marks
        function onChanged() { soon.restart() }
        function onLookChanged() { soon.restart() }
        function onFlashChanged() { lightSoon.restart() }
    }

    // A moment gone to from the graph: its words lit for a moment where
    // this text holds them, so the eye lands on the very spot. Looked for
    // once the chapter is laid out, which a turn to another one waits for.
    property var lit: []
    property real glow: 0
    Timer { id: lightSoon; interval: 60; onTriggered: helper.light() }
    function light() {
        const flash = reader.marks.flash
        if (!text || !flash.words || flash.section !== reader.section) {
            lit = []
            return
        }
        const found = reader.marks.findIn(text.textDocument, flash.words, flash.fraction)
        if (found.end === undefined) return
        const a = text.positionToRectangle(found.start)
        const b = text.positionToRectangle(found.end)
        // One rectangle a line the words are on.
        const rects = a.y === b.y ? [Qt.rect(a.x, a.y, Math.max(4, b.x - a.x), a.height)]
                                  : [Qt.rect(a.x, a.y, Math.max(4, text.width - a.x), a.height)]
        if (a.y !== b.y) {
            if (b.y - (a.y + a.height) > 1) rects.push(Qt.rect(0, a.y + a.height, text.width, b.y - a.y - a.height))
            rects.push(Qt.rect(0, b.y, Math.max(4, b.x), b.height))
        }
        lit = rects
        shine.restart()
    }
    SequentialAnimation {
        id: shine
        NumberAnimation { target: helper; property: "glow"; from: 0; to: 1; duration: 180; easing.type: Easing.OutCubic }
        PauseAnimation { duration: 1600 }
        NumberAnimation { target: helper; property: "glow"; to: 0; duration: 900; easing.type: Easing.InOutQuad }
        ScriptAction { script: helper.lit = [] }
    }
    Repeater {
        model: helper.lit
        delegate: Rectangle {
            required property rect modelData
            objectName: "pageMomentLit"
            x: modelData.x - 2
            y: modelData.y
            width: modelData.width + 4
            height: modelData.height
            radius: Math.min(3, theme.radius)
            color: Qt.alpha(theme.accent, 0.32)
            opacity: helper.glow
        }
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

    // A finger's selection (#140): the word at one end held while the other
    // end ("start" or "end") moves, or the whole of it ("word") while the
    // finger of a long press moves on; then from the words held to the word at
    // a position, whichever side of them it is, a whole word at a time.
    property int heldStart: -1
    property int heldEnd: -1
    function hold(which) {
        if (!text || text.selectionEnd <= text.selectionStart) return false
        const start = text.selectionStart, end = text.selectionEnd
        const word = which === "start" ? reader.marks.wordAt(text.textDocument, end - 1)
                   : which === "end" ? reader.marks.wordAt(text.textDocument, start) : { start: start, end: end }
        const whole = word.end > word.start
        heldStart = whole ? Math.max(start, word.start) : start
        heldEnd = whole ? Math.min(end, word.end) : end
        return true
    }
    function extendTo(position) {
        if (heldStart < 0) return false
        const word = reader.marks.wordAt(text.textDocument, position)
        if (word.end <= word.start) return false
        text.select(Math.min(heldStart, word.start), Math.max(heldEnd, word.end))
        return true
    }
    // The carets at the selection's two ends, in the text, and whether it runs right to left.
    function ends() {
        if (!text || text.selectionEnd <= text.selectionStart) return null
        const a = text.positionToRectangle(text.selectionStart)
        const next = text.positionToRectangle(text.selectionStart + 1)
        return { start: a, end: text.positionToRectangle(text.selectionEnd), rtl: Math.abs(next.y - a.y) < 1 ? next.x < a.x : win.rightToLeft }
    }

    // Its highlights (#81). The one at a point of the text: {id, color, rect}, or null.
    function highlightAt(x, y) {
        if (!text) return null
        const position = text.positionAt(x, y)
        const r = text.positionToRectangle(position)
        if (Math.abs(y - (r.y + r.height / 2)) > r.height) return null
        const found = reader.highlights.at(text.textDocument, position, position)
        if (!found.id) return null
        found.rect = r
        return found
    }
    function selectionSpan() { return text && text.selectionEnd > text.selectionStart ? { start: text.selectionStart, end: text.selectionEnd } : null }
    function highlightSelection(color) {
        const span = selectionSpan()
        return span ? reader.highlights.highlight(text.textDocument, span.start, span.end, color) : 0
    }
    function selectionHighlight() {
        const span = selectionSpan()
        return span ? reader.highlights.at(text.textDocument, span.start, span.end) : ({})
    }
    function placeSelection(id) {
        const span = selectionSpan()
        return span ? reader.highlights.placeAgain(id, text.textDocument, span.start, span.end) : false
    }
    // A stroke of the pen, its points in the text's coordinates.
    function penned(points) { return reader.highlights.penned(text.textDocument, points) }

    HoverHandler {
        id: pointer
        cursorShape: helper.text && helper.text.hoveredLink !== "" || helper.hovered ? Qt.PointingHandCursor : Qt.IBeamCursor
        onPointChanged: helper.hovered = pointer.hovered ? helper.thingAt(point.position.x, point.position.y) : null
        onHoveredChanged: if (!hovered) helper.hovered = null
    }
    PointHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        onActiveChanged: {
            if (active) helper.pressedAt = point.pressPosition
            else helper.released()
        }
    }
}
