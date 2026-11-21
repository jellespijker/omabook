import QtQuick

// The margin notes of a page that doesn't reflow, a PDF's or a comic's
// (#115), marked on it as a chapter's are in its text (NoteMarks): a small
// mark where each note's words end, or at the corner of the part of the page
// (a panel) a note is on, that part framed; and the words or the part lit
// while its note is the one being read, written or gone to. A click or a tap
// on the mark opens the note on a card beside it. It lies over the page and
// changes nothing of it.
Item {
    id: marks
    property int page: 0
    // The notes (ReadingNotes), whose model says where each stands.
    property var reading: null
    // Where each note of the page stands: [{id, note, part, x, y, width, height, endX, endTop, endY}].
    property var spots: []
    signal opened(var id, rect around)

    function respot() { spots = reading && width > 0 ? reading.notes.pageNotes(page, Qt.size(width, height)) : [] }
    Timer { id: soon; interval: 0; onTriggered: marks.respot() }
    Component.onCompleted: soon.restart()
    onWidthChanged: soon.restart()
    onPageChanged: soon.restart()
    onReadingChanged: soon.restart()
    Connections {
        target: marks.reading ? marks.reading.notes : null
        function onChanged() { soon.restart() }
    }
    // Top to bottom, then across: [{id, rect}].
    function inOrder() {
        return spots.slice().sort((a, b) => a.y - b.y || a.x - b.x).map(one => ({ id: one.id, rect: Qt.rect(one.x, one.y, one.width, one.height) }))
    }

    Repeater {
        model: marks.spots
        delegate: Item {
            id: spot
            required property var modelData
            readonly property bool lit: !!marks.reading && marks.reading.current === modelData.id
            readonly property real hit: win.touch ? 36 : 22
            anchors.fill: parent
            // The part a note on a panel is on, framed; any note's words or part lit while it is current.
            Rectangle {
                objectName: "pageNotePart_" + spot.modelData.id
                visible: spot.modelData.part || spot.lit
                x: spot.modelData.x - 1
                y: spot.modelData.y - 1
                width: spot.modelData.width + 2
                height: spot.modelData.height + 2
                radius: Math.min(3, theme.radius)
                color: spot.lit ? Qt.alpha(theme.accent, 0.16) : "transparent"
                border.color: spot.modelData.part ? Qt.alpha(theme.accent, spot.lit ? 1 : 0.7) : "transparent"
                border.width: spot.modelData.part ? 1.5 : 0
            }
            Item {
                objectName: "pageNoteMark_" + spot.modelData.id
                // A finger's tap on it is its own, not the page's under it (ReaderTouch, #141).
                readonly property bool takesTaps: true
                // At a panel's corner; just past a passage's last word, high on its line, clear of the letters.
                x: Math.max(0, Math.min(marks.width - width, spot.modelData.endX - spot.hit / 2 + (spot.modelData.part ? 0 : 5)))
                y: Math.max(0, Math.min(marks.height - height, (spot.modelData.part ? spot.modelData.endY : spot.modelData.endTop + 2) - spot.hit / 2))
                width: spot.hit
                height: spot.hit
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Margin note: %1").arg(spot.modelData.note)
                Rectangle {
                    anchors.centerIn: parent
                    width: spot.lit ? 11 : 9
                    height: width
                    radius: width / 2
                    color: theme.accent
                    // Seen on a white page whatever the theme.
                    border.color: "white"
                    border.width: 1.5
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: marks.opened(spot.modelData.id, Qt.rect(spot.modelData.x, spot.modelData.y, spot.modelData.width, spot.modelData.height)) }
            }
        }
    }
}
