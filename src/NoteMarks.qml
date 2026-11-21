import QtQuick

// The margin notes of the chapter one TextEdit of the page shows (#82),
// marked in its text: a small mark where each note's passage ends, and the
// passage lit while its note is the one being read, written or gone to. In a
// window with no room for the margin column the mark is how a note is
// opened, by a click or a tap; it is a finger's width to hit then. There
// others' notes (#85, #124) are marked too, a ring in their writer's colour,
// and open on a card of their own. Like PageMarks it lies over the text and
// changes nothing of its layout.
Item {
    id: marks
    // The TextEdit it serves, and the notes (ReadingNotes).
    property TextEdit text
    property var reading: null
    // What of the text is on the page: from and to, in its own coordinates
    // (a page of a spread shows a stretch of the column); a mark outside it isn't there.
    property real from: -1e9
    property real to: 1e9
    // Where each note is in the text: [{id, start, end, note}].
    property var spans: []
    // Others' notes, where no margin column stands them beside the text:
    // [{id, start, end, note, called, color, person, ink}] (AnnotationLayerView::notesIn).
    readonly property var others: reading ? reading.others : null
    property var otherSpans: []
    function respan() {
        spans = reading && text ? reading.notes.spans(text.textDocument) : []
        otherSpans = others && others.shown && reading && !reading.beside && text ? others.notesIn(text.textDocument) : []
    }
    Timer { id: soon; interval: 0; onTriggered: marks.respan() }
    onTextChanged: soon.restart()
    onReadingChanged: soon.restart()
    Connections {
        target: marks.text
        function onTextChanged() { soon.restart() }
    }
    Connections {
        target: marks.reading ? marks.reading.notes : null
        function onChanged() { soon.restart() }
    }
    Connections {
        target: marks.others
        function onChanged() { soon.restart() }
    }
    Connections {
        target: marks.reading
        function onBesideChanged() { soon.restart() }
    }
    // Laid out again (another width, another size of type): the same
    // characters, elsewhere.
    property int laid: 0
    Connections {
        target: marks.text
        function onContentHeightChanged() { marks.laid++ }
        function onWidthChanged() { marks.laid++ }
    }

    // The line of the text at a height of it, which a note written by hand
    // there or beside it in the margin column is on (#90): {text, start, end},
    // the line above where it is between paragraphs; null where the page
    // shows none.
    function lineAt(y) {
        for (let up = 0; up <= 48; up += 8) {
            const at = y - up
            if (!text || at < Math.max(0, from) || at > Math.min(text.height, to)) continue
            const a = text.positionAt(0, at)
            const b = text.positionAt(text.width, at)
            if (Math.max(a, b) > Math.min(a, b)) return { text: text, start: Math.min(a, b), end: Math.max(a, b) }
        }
        return null
    }
    // The note a position of the text ends the passage of, as a rectangle.
    function endOf(span) {
        laid
        const r = text.positionToRectangle(Math.max(span.start, span.end - 1))
        return Qt.rect(r.x + r.width, r.y, 1, r.height)
    }

    Repeater {
        model: marks.spans
        delegate: Item {
            id: mark
            required property var modelData
            readonly property rect end: marks.endOf(modelData)
            readonly property bool lit: !!marks.reading && marks.reading.current === modelData.id
            readonly property real hit: win.touch ? 36 : 22
            objectName: "noteMark_" + modelData.id
            visible: end.y + end.height > marks.from && end.y < marks.to
            x: end.x - hit / 2 + 4
            y: end.y + end.height / 2 - hit / 2 - end.height * 0.3
            width: hit
            height: hit
            Rectangle {
                anchors.centerIn: parent
                width: mark.lit ? 9 : 7
                height: width
                radius: width / 2
                color: theme.accent
                opacity: mark.lit ? 1 : 0.75
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler {
                onTapped: if (marks.reading) marks.reading.open(mark.modelData.id, marks.text, mark.modelData.start, mark.modelData.end)
            }
        }
    }

    Repeater {
        model: marks.otherSpans
        delegate: Item {
            id: theirs
            required property var modelData
            readonly property rect end: marks.endOf(modelData)
            readonly property real hit: win.touch ? 36 : 22
            objectName: "othersNoteMark"
            visible: end.y + end.height > marks.from && end.y < marks.to
            x: end.x - hit / 2 + 4
            y: end.y + end.height / 2 - hit / 2 - end.height * 0.3
            width: hit
            height: hit
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("%1's note").arg(modelData.called)
            Rectangle {
                anchors.centerIn: parent
                width: 9
                height: 9
                radius: 4.5
                color: "transparent"
                border.width: 2
                border.color: theirs.modelData.color
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler {
                onTapped: if (marks.reading) marks.reading.openOther(theirs.modelData, marks.text)
            }
        }
    }

    // The passage of the note that is current, lit: a rectangle a line, the
    // lines between as one.
    readonly property var litSpan: {
        if (!reading || !reading.current) return null
        for (const span of spans) if (span.id === reading.current) return span
        return null
    }
    readonly property var litRects: {
        laid
        if (!litSpan || !text) return []
        const a = text.positionToRectangle(litSpan.start)
        const b = text.positionToRectangle(litSpan.end)
        if (a.y === b.y) return [Qt.rect(a.x, a.y, Math.max(4, b.x - a.x), a.height)]
        const rects = [Qt.rect(a.x, a.y, Math.max(4, text.width - a.x), a.height)]
        if (b.y - (a.y + a.height) > 1) rects.push(Qt.rect(0, a.y + a.height, text.width, b.y - a.y - a.height))
        rects.push(Qt.rect(0, b.y, Math.max(4, b.x), b.height))
        return rects
    }
    Repeater {
        model: marks.litRects
        delegate: Rectangle {
            required property rect modelData
            x: modelData.x - 1
            y: modelData.y
            width: modelData.width + 2
            height: modelData.height
            radius: Math.min(3, theme.radius)
            color: Qt.alpha(theme.accent, 0.16)
        }
    }
}
