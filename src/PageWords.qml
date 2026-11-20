import QtQuick

// The words of a PDF's page as it is drawn (#81): its highlights over them,
// thin washes made for paper, whatever the theme, each with the line of its
// colour; and its words selected by the mouse (a drag that starts on a
// word), a finger (a long press on one, see PageReader) or the keyboard, as
// a chapter's are, to highlight them or do the rest of what the menu offers.
// A page of pictures (a comic) has no words, and this nothing to do.
Item {
    id: layer
    property int page: 0
    // The words, where they stand at the page's size: loaded when first needed.
    property var words: null
    function wordsNow() {
        if (words === null) words = reader.highlights.pageWords(page, Qt.size(width, height))
        return words
    }
    onWidthChanged: { words = null; soon.restart() }
    // What is selected, by its words' places in the list; -1 for nothing.
    property int anchorAt: -1
    property int activeAt: -1
    readonly property bool hasSelection: anchorAt >= 0 && activeAt >= 0
    readonly property int firstWord: Math.min(anchorAt, activeAt)
    readonly property int lastWord: Math.max(anchorAt, activeAt)
    // A mouse press on a word, and where the last one was.
    property bool dragging: false
    property point pressedAt: Qt.point(-1, -1)
    signal selected()

    property var spans: []
    // One a line of each highlight, with how it is drawn on paper.
    property var pieces: []
    function lay() {
        spans = width > 0 ? reader.highlights.pageSpans(page, Qt.size(width, height)) : []
        const looks = {}
        for (const one of reader.highlights.colors) looks[one.key] = one
        const made = []
        for (const span of spans) {
            const look = looks[span.color] || {}
            for (const r of span.rects)
                made.push({ x: r.x, y: r.y, width: r.width, height: r.height, tint: look.paperTint, line: look.paperLine,
                            dashes: look.dashes || [] })
        }
        pieces = made
    }
    Timer { id: soon; interval: 0; onTriggered: layer.lay() }
    Component.onCompleted: soon.restart()
    onPageChanged: { words = null; soon.restart() }
    Connections {
        target: reader.highlights
        function onChanged() { soon.restart() }
    }

    function wordAt(x, y) {
        const all = wordsNow()
        for (let i = 0; i < all.length; ++i) {
            const w = all[i]
            if (x >= w.x - 2 && x <= w.x + w.width + 2 && y >= w.y - 2 && y <= w.y + w.height + 2) return i
        }
        return -1
    }
    function clear() { anchorAt = -1; activeAt = -1 }
    function selectWordAt(x, y) {
        const at = wordAt(x, y)
        anchorAt = at
        activeAt = at
        return at >= 0
    }
    // The selection: its words, where it is (in this item), and its characters on the page.
    function selection() {
        if (!hasSelection) return null
        const all = wordsNow()
        const a = all[firstWord], b = all[lastWord]
        let left = a.x, top = a.y, right = a.x + a.width, bottom = a.y + a.height
        for (let i = firstWord; i <= lastWord; ++i) {
            left = Math.min(left, all[i].x); right = Math.max(right, all[i].x + all[i].width)
            top = Math.min(top, all[i].y); bottom = Math.max(bottom, all[i].y + all[i].height)
        }
        return { text: reader.highlights.pageText(page, a.start, b.end), paragraph: "", start: a.start, end: b.end,
                 rect: Qt.rect(left, top, right - left, bottom - top) }
    }
    // The highlight under a point: {id, color, rect}, or null.
    function highlightAt(x, y) {
        for (const span of spans)
            for (const r of span.rects)
                if (x >= r.x && x <= r.x + r.width && y >= r.y && y <= r.y + r.height) return { id: span.id, color: span.color, rect: r }
        return null
    }
    // The keyboard's selection: the first word, then words or lines on (or back).
    function begin() {
        const all = wordsNow()
        anchorAt = all.length > 0 ? 0 : -1
        activeAt = anchorAt
        return anchorAt >= 0
    }
    function step(count, lines, extend) {
        const all = wordsNow()
        if (!hasSelection || all.length === 0) return
        let at = count > 0 || lines > 0 || extend ? activeAt : anchorAt
        at = Math.max(0, Math.min(all.length - 1, at + count))
        if (lines !== 0) {
            const now = all[at]
            let best = at, off = 1e9
            for (let i = 0; i < all.length; ++i) {
                const w = all[i]
                const down = w.y - now.y
                if ((lines > 0 && down <= now.height * 0.5) || (lines < 0 && down >= -now.height * 0.5)) continue
                const d = Math.abs(down) * 4 + Math.abs(w.x - now.x)
                if (d < off) { off = d; best = i }
            }
            at = best
        }
        activeAt = at
        if (!extend) anchorAt = at
    }
    function activeRect() {
        const all = wordsNow()
        const w = all[Math.max(0, activeAt)]
        return w ? Qt.rect(w.x, w.y, w.width, w.height) : Qt.rect(0, 0, 0, 0)
    }

    Repeater {
        model: layer.pieces
        delegate: Item {
            required property var modelData
            objectName: "pageHighlightRect"
            x: modelData.x - 1
            y: modelData.y
            width: modelData.width + 2
            height: modelData.height
            // Over the page's picture, words and all: thin enough to leave them black.
            Rectangle { anchors.fill: parent; color: parent.modelData.tint || "transparent"; opacity: 0.45 }
            HighlightLine {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: parent.modelData.line || "transparent"
                dashes: parent.modelData.dashes
            }
        }
    }
    // The selected words, one box each, in the accent.
    Repeater {
        model: layer.hasSelection ? layer.lastWord - layer.firstWord + 1 : 0
        delegate: Rectangle {
            required property int index
            readonly property var word: layer.words ? layer.words[layer.firstWord + index] : null
            objectName: "pageSelection"
            visible: !!word
            x: word ? word.x - 1 : 0
            y: word ? word.y : 0
            width: word ? word.width + 2 : 0
            height: word ? word.height : 0
            color: Qt.alpha(theme.accent, 0.35)
        }
    }

    // A drag of the mouse that starts on a word selects from it to the word
    // under the pointer; one that starts beside the words scrolls, as ever.
    PointHandler {
        id: mouse
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        onActiveChanged: {
            if (active) {
                layer.pressedAt = point.pressPosition
                const at = layer.wordAt(point.pressPosition.x, point.pressPosition.y)
                layer.dragging = at >= 0
                layer.anchorAt = at
                layer.activeAt = at
                if (at < 0) layer.clear()
                return
            }
            // A click, not a drag, selects nothing: it is for a highlight, or nothing.
            if (layer.dragging && layer.anchorAt === layer.activeAt
                    && Math.abs(point.position.x - layer.pressedAt.x) + Math.abs(point.position.y - layer.pressedAt.y) < 4)
                layer.clear()
            layer.dragging = false
            layer.selected()
        }
        onPointChanged: {
            if (!active || !layer.dragging) return
            const at = layer.wordAt(point.position.x, point.position.y)
            if (at >= 0) layer.activeAt = at
        }
    }
}
