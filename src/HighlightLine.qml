import QtQuick

// The line under a highlight's words (#81), in its colour's own pattern of
// dashes and gaps (HighlightColors), so a highlight is told by its line
// where its colour isn't. Plain rectangles, drawn by every renderer.
Item {
    id: line
    property color color: "transparent"
    // Dashes and gaps, in widths of the line; none for a solid one.
    property var dashes: []
    property real thickness: 2
    height: thickness

    readonly property var pieces: {
        const pattern = dashes || []
        if (pattern.length === 0) return [{ x: 0, width: width }]
        const made = []
        let at = 0, i = 0
        while (at < width && made.length < 400) {
            const length = pattern[i % pattern.length] * thickness
            if (i % 2 === 0) made.push({ x: at, width: Math.min(length, width - at) })
            at += length
            ++i
        }
        return made
    }
    Repeater {
        model: line.pieces
        delegate: Rectangle {
            required property var modelData
            x: modelData.x
            width: modelData.width
            height: line.thickness
            color: line.color
        }
    }
}
