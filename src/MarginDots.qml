import QtQuick

// The margin folded away (#167), or with no room for its column (#157): a
// small dot beside each line of the text that has a margin note on it,
// typed or written by hand (#82, #90), someone else's note (#85) or ink in
// the margin (#152), in the colour of the first of them; several on a line
// are one dot with their count. A click, a finger's tap or the pen's on a
// dot, or Enter on it (Tab goes through them), opens its line's first note
// on the card beside its words (ReadingNotes), to read, edit or write on.
// It stands in the band beside the text where the lines end, as wide as it
// is given, and changes nothing of the text's layout.
Item {
    id: dots
    objectName: "marginDots"
    // The notes (ReadingNotes), and the TextEdit whose chapter they are on.
    property var reading: null
    property TextEdit text
    // Where the line of a character of the text is in this item, top to
    // bottom, or NaN where the page doesn't show it.
    property var place: function(position) { return NaN }
    // Something moved the text under the dots (the page that shows it, a turn of a page).
    property real laid: 0
    // Whether the page this band is beside shows (a page of a spread).
    property bool on: true
    // Only where the notes don't stand in a margin column.
    readonly property bool wanted: !!reading && !reading.beside && reader.kind === "flow"
    visible: wanted && on

    property var spans: []
    property var otherSpans: []
    property var inkSpans: []
    property int relaid: 0
    function respan() {
        const ready = wanted && !!text
        const others = ready ? reading.others : null
        spans = ready ? reading.notes.spans(text.textDocument) : []
        otherSpans = others && others.shown ? others.notesIn(text.textDocument) : []
        inkSpans = ready ? reader.lineInk.marginIn(text) : []
    }
    Timer { id: soon; interval: 0; onTriggered: dots.respan() }
    onTextChanged: soon.restart()
    onReadingChanged: soon.restart()
    onWantedChanged: soon.restart()
    Connections {
        target: dots.text
        function onTextChanged() { soon.restart() }
        function onContentHeightChanged() { dots.relaid++ }
        function onWidthChanged() { dots.relaid++ }
    }
    Connections {
        target: dots.reading ? dots.reading.notes : null
        function onChanged() { soon.restart() }
    }
    Connections {
        target: dots.reading ? dots.reading.others : null
        function onChanged() { soon.restart() }
    }
    Connections {
        target: reader.lineInk
        function onChanged() { soon.restart() }
    }

    // The lines with something on them, top to bottom: {top, height, items:
    // [{kind, span, color}]}, one's own notes first, then others', then ink.
    readonly property var lines: {
        laid
        relaid
        if (!text || !visible) return []
        const byLine = {}
        const add = (span, kind, color) => {
            const top = place(span.start)
            if (isNaN(top)) return
            const r = text.positionToRectangle(span.start)
            const key = Math.round(r.y)
            if (!byLine[key]) byLine[key] = { top: top, height: r.height, items: [] }
            byLine[key].items.push({ kind: kind, span: span, color: color })
        }
        for (const span of spans) add(span, "note", theme.accent)
        for (const span of otherSpans) add(span, "other", span.color)
        for (const span of inkSpans) add(span, "ink", span.color)
        return Object.values(byLine).sort((a, b) => a.top - b.top)
    }
    // The first words of a note, for its name.
    function firstWords(words) {
        const all = (words || "").trim().split(/\s+/).filter(word => word !== "")
        return all.slice(0, 6).join(" ") + (all.length > 6 ? "…" : "")
    }
    function nameOf(line) {
        const first = line.items[0]
        const words = firstWords(first.span.note)
        const one = first.kind === "ink" ? qsTr("Ink in the margin")
                  : first.kind === "other" ? qsTr("%1's note: %2").arg(first.span.called).arg(words)
                  : words !== "" ? qsTr("Note: %1").arg(words) : qsTr("Handwritten note")
        return line.items.length > 1 ? qsTr("%1, and %Ln more on this line", "", line.items.length - 1).arg(one) : one
    }
    // The line's first note, on its card beside its words.
    function open(line) {
        const first = line.items[0]
        if (first.kind === "note") reading.open(first.span.id, text, first.span.start, first.span.end)
        else if (first.kind === "other") reading.openOther(first.span, text)
        else backend.say(qsTr("Ink in the margin beside this line"))
    }

    Repeater {
        model: dots.lines
        delegate: Item {
            id: dot
            required property var modelData
            readonly property var first: modelData.items[0]
            readonly property int count: modelData.items.length
            readonly property bool lit: first.kind === "note" && !!dots.reading && dots.reading.current === first.span.id
            // A finger's tap on it is its own, not the page's under it (ReaderTouch, #141).
            readonly property bool takesTaps: true
            // And the pen's, not the pen's gestures over the page (PenGestures).
            readonly property bool takesPen: true
            readonly property real hit: win.touch ? 36 : 24
            objectName: first.kind === "note" ? "marginDot_" + first.span.id : "marginDot_" + first.kind
            readonly property string said: dots.nameOf(modelData)
            x: (dots.width - hit) / 2
            y: modelData.top + modelData.height / 2 - hit / 2
            width: hit
            height: hit
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: said
            Accessible.onPressAction: dots.open(modelData)
            Keys.onPressed: (event) => {
                if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter && event.key !== Qt.Key_Space) return
                dots.open(dot.modelData)
                event.accepted = true
            }
            Rectangle {
                anchors.centerIn: parent
                visible: dot.activeFocus
                width: dot.hit - 4
                height: width
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: theme.foreground
            }
            Rectangle {
                anchors.centerIn: parent
                width: dot.count > 1 ? 16 : dot.lit || dot.activeFocus ? 10 : 8
                height: width
                radius: width / 2
                color: dot.first.color
                Text {
                    anchors.centerIn: parent
                    visible: dot.count > 1
                    text: dot.count > 9 ? "9+" : dot.count.toLocaleString(Qt.locale(), 'f', 0)
                    font.pixelSize: 10
                    font.bold: true
                    color: dot.first.kind === "note" ? theme.accentForeground : theme.background
                }
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: dots.open(dot.modelData) }
        }
    }
}
