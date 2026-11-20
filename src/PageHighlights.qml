import QtQuick

// The highlights of the chapter one TextEdit shows (#81), under its words:
// each a thin wash of its colour (theme-aware, so the words stay as easy to
// read as the rest of the page) with a line of its own under them (HighlightLine), solid,
// dashed or dotted by colour, for a reader who doesn't tell the colours
// apart. It lies under the text (z below it) and takes no input of its own.
Item {
    id: layer
    // The TextEdit it lies under.
    property TextEdit text
    // One a line of each highlight: {x, y, width, height, tint, line, dashes}.
    property var pieces: []

    function lay() {
        const spans = text && text.textDocument ? reader.highlights.spansIn(text.textDocument) : []
        const looks = {}
        for (const one of reader.highlights.colors) looks[one.key] = one
        const made = []
        for (const span of spans) {
            const look = looks[span.color] || {}
            for (const r of span.rects)
                made.push({ x: r.x, y: r.y, width: r.width, height: r.height, tint: look.tint, line: look.line, dashes: look.dashes || [] })
        }
        pieces = made
    }
    // Laid once the text is laid out: a timer of its own, which goes with this item.
    Timer { id: soon; interval: 0; onTriggered: layer.lay() }
    onTextChanged: soon.restart()
    Component.onCompleted: soon.restart()
    Connections {
        target: layer.text
        function onTextChanged() { soon.restart() }
        function onContentHeightChanged() { soon.restart() }
        function onWidthChanged() { soon.restart() }
    }
    Connections {
        target: reader.highlights
        function onChanged() { soon.restart() }
        function onLookChanged() { soon.restart() }
    }

    Repeater {
        model: layer.pieces
        delegate: Item {
            required property var modelData
            objectName: "highlightRect"
            x: modelData.x
            y: modelData.y
            width: modelData.width
            height: modelData.height
            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: -1
                anchors.rightMargin: -1
                radius: Math.min(2, theme.radius)
                color: parent.modelData.tint || "transparent"
            }
            HighlightLine {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                color: parent.modelData.line || "transparent"
                dashes: parent.modelData.dashes
            }
        }
    }
}
