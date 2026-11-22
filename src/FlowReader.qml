import QtQuick
import QtQuick.Controls

// A flowing book: one chapter at a time, set in the reader's own font and
// the desktop's colors, in one column that scrolls, no wider than the
// measure (its longest line, in characters of the font) and centred with
// the room to spare as margins. A screen's step shows nothing again: it
// goes on from the first line that wasn't wholly in sight.
//
// When it runs on (the layout "scroll", readingformat.h), the chapter ends
// in a quiet line naming the next one, and scrolling on past it, by the
// wheel, a finger pulling past the end or a step at the end, goes on into
// the next chapter at its top; back past the top goes to the end of the one
// before.
Flickable {
    id: flick
    objectName: "flowReader"
    // Without the soft hyphens hyphenation puts in (hyphenation.h), which are only for the eye.
    readonly property string selectedText: page.selectedText.replace(/\u00ad/g, "")
    // Whether the graph's things are marked in the text.
    property bool marking: false
    // Whether reading runs on from one chapter into the next.
    property bool runsOn: false
    // A finger is on the page: it scrolls, rather than selecting words.
    property bool fingerDown: false
    // A finger moves an end of what it selected (#140): the page holds still under it.
    property bool holding: false
    interactive: !holding
    // The book's margin notes (ReadingNotes), and the room their column
    // takes beside the text, a gutter beyond it (PageLayout::columns).
    property var notes: null
    readonly property real noteRoom: notes && reader.format.noteWidth > 0 ? reader.format.noteWidth + reader.format.gutter : 0
    property alias marginColumn: margin
    // What lies over the page (the ink's bar), where the pen doesn't write on it.
    property Item covering: null
    // The thing under the pointer changed (null for none), and a selection
    // was made with the mouse.
    signal pointed(var thing)
    signal selected()
    readonly property real range: Math.max(0, contentHeight - height)
    // The same, worked out when it is asked for. What runs because the
    // chapter's height changed may run before `range` has heard of it.
    function span() { return Math.max(0, contentHeight - height) }
    // The place to show once the chapter is laid out, until the reader scrolls.
    property real wanted: -1
    property string wantedAnchor: ""
    property bool settling: false
    // How far through the chapter the page is, as the reader left it, and
    // the size the chapter and the view had then. While either is another,
    // the chapter is being laid out again, and where the view happens to
    // stand in the middle of that is not the reader's doing.
    property real held: 0
    // Not before the view was told where the reader is: what it does while
    // it is being made says nothing of where they are, and must not be
    // taken for it.
    property bool ready: false
    property real laidHeight: 0
    property real laidView: 0
    function hold() {
        held = span() > 0 ? contentY / span() : held
        laidHeight = contentHeight
        laidView = height
    }

    // The options the text is set with, and the room around it: the margin
    // asked for, less where a narrow window hasn't the width for it.
    readonly property var format: reader.format.effective
    readonly property real side: Math.min(format.margins, Math.max(8, (width - 280) / 2))

    // A picture is seen whole in the window, less a line above and below.
    readonly property int pictureHeight: Math.floor((height - 2 * side - format.font_size * 3) * 100 / Math.max(100, format.line_height))
    onPictureHeightChanged: if (ready) reader.pageHeight = pictureHeight
    contentWidth: width
    contentHeight: page.height + 2 * side + 32 + (chapterEnd.visible ? chapterEnd.height + 32 : 0)
    // Running on, a finger may pull past either end, which is how it turns
    // to the next chapter or back (see onDraggingChanged).
    boundsBehavior: runsOn ? Flickable.DragOverBounds : Flickable.StopAtBounds
    clip: true
    focus: true
    ScrollBar.vertical: ScrollBar {}

    function place(y) {
        settling = true
        const room = span()
        contentY = Math.max(0, Math.min(room, y))
        settling = false
        // Before the chapter is laid out there is nowhere to be yet.
        if (room > 0) {
            reader.fraction = contentY / room
            hold()
        }
    }
    // Shows the wanted place. A chapter's height keeps changing while its
    // pictures arrive, so this runs again each time it does.
    function settle() {
        if (wanted < 0)
            return
        const at = wantedAnchor !== "" ? reader.anchorPosition(page.textDocument, wantedAnchor) : -1
        place(at >= 0 ? page.y + page.positionToRectangle(at).y - 24 : wanted * span())
    }
    function scrollBy(distance) {
        wanted = -1
        place(contentY + distance)
    }
    function atEnd() { return contentY >= span() - 1 }
    function atStart() { return contentY <= 1 }
    // A screen on or back, from the first line that wasn't wholly in sight
    // (PageLayout::screenStep), in the column's own terms and back.
    function screen(forward) {
        const top = reader.screenStep(page.textDocument, contentY - page.y, height, forward)
        wanted = -1
        place(top <= 0.5 && !forward ? 0 : page.y + top)
    }
    // A page down at the end of a chapter turns to the next one.
    function pageDown() {
        if (atEnd()) next()
        else screen(true)
    }
    function pageUp() {
        if (atStart()) backToEnd()
        else screen(false)
    }
    // Running on, a line past the end is the next chapter too.
    function lineDown() {
        if (runsOn && atEnd()) next()
        else scrollBy(page.font.pixelSize * 4)
    }
    function lineUp() {
        if (runsOn && atStart()) backToEnd()
        else scrollBy(-page.font.pixelSize * 4)
    }
    function next() { reader.goTo(reader.section + 1) }
    // The end of the chapter before.
    function backToEnd() { if (reader.section > 0) reader.goTo(reader.section - 1, "", true) }
    // To the chapter's start, and from there to the one before.
    function previous() {
        if (!atStart()) toStart()
        else reader.goTo(reader.section - 1)
    }
    // A finger's tap on the line at the chapter's end goes on.
    function activateAt(x, y) {
        if (!chapterEnd.visible || chapterEnd.last) return false
        const at = chapterEnd.mapFromItem(flick, x, y)
        if (at.x < 0 || at.y < 0 || at.x > chapterEnd.width || at.y > chapterEnd.height) return false
        next()
        return true
    }
    function toStart() { scrollBy(-contentY) }
    function toEnd() { scrollBy(range) }
    function zoom(steps) { reader.format.zoom(steps) }
    // A pinch changes the type a size at a time as the fingers spread.
    property real pinched: 1
    function pinch(delta) {
        pinched *= delta
        if (pinched > 1.08) { zoom(1); pinched = 1 }
        else if (pinched < 0.92) { zoom(-1); pinched = 1 }
    }
    // The words at the top of the window, to name a bookmark by.
    // The reading view's ways into the text; points and rectangles are in
    // this item's coordinates.
    function fromPage(rect) {
        const at = page.mapToItem(flick, rect.x, rect.y)
        return Qt.rect(at.x, at.y, rect.width, rect.height)
    }
    function thingAt(x, y) {
        const at = page.mapFromItem(flick, x, y)
        const found = marks.thingAt(at.x, at.y)
        if (found) found.rect = fromPage(found.rect)
        return found
    }
    function selection() {
        const chosen = marks.selection()
        if (chosen) chosen.rect = fromPage(chosen.rect)
        return chosen
    }
    function selectAt(x, y) {
        const at = page.mapFromItem(flick, x, y)
        return marks.selectWordAt(at.x, at.y)
    }
    // The keyboard's selection starts at the first word on the screen.
    function beginSelecting() {
        marks.begin(page.positionAt(0, Math.max(0, contentY - page.y + page.font.pixelSize)))
    }
    function stepSelecting(words, lines, extend) {
        marks.step(words, lines, extend)
        const r = marks.activeRect()
        if (page.y + r.y < contentY) scrollBy(page.y + r.y - contentY - page.font.pixelSize * 2)
        else if (page.y + r.y + r.height > contentY + height) scrollBy(page.y + r.y + r.height - contentY - height + page.font.pixelSize * 2)
    }
    function endSelecting() { marks.finish() }
    // A finger's selection (SelectHandles, #140): its ends, one held while
    // the other goes to the word at a point, and at the page's foot or head
    // the column scrolls on.
    function selectionEnds() {
        const ends = marks.ends()
        return ends ? { start: fromPage(ends.start), end: fromPage(ends.end), rtl: ends.rtl } : null
    }
    function holdSelection(which) { return marks.hold(which) }
    function extendSelection(x, y) {
        const at = page.mapFromItem(flick, x, y)
        return marks.extendTo(page.positionAt(at.x, at.y))
    }
    function edgeStep(direction) {
        if (direction > 0 ? atEnd() : atStart()) return false
        scrollBy(direction * page.font.pixelSize)
        return true
    }
    function clearSelection() {
        marks.finish()
        page.deselect()
    }
    function copySelection() { plainCopy.copyText(selectedText) }
    // The selection, for a margin note: {text, start, end}; null for none.
    function selectionSpan() { return page.selectedText !== "" ? { text: page, start: page.selectionStart, end: page.selectionEnd } : null }
    // The notes whose passages start on the screen, top to bottom.
    function notesInSight() {
        if (!notes) return []
        return notes.notes.spans(page.textDocument).filter(span => {
            const y = page.y + page.positionToRectangle(span.start).y
            return y >= contentY && y < contentY + height
        }).map(span => ({ id: span.id, text: page, start: span.start, end: span.end }))
    }
    // Highlights (#81): the one at a point, the one the last click was on,
    // the selection made one, and a stroke of the pen.
    function highlightAtPoint(x, y) {
        const at = page.mapFromItem(flick, x, y)
        const found = marks.highlightAt(at.x, at.y)
        if (found) found.rect = fromPage(found.rect)
        return found
    }
    function pressedHighlight() {
        const found = marks.pressedAt.x < 0 ? null : marks.highlightAt(marks.pressedAt.x, marks.pressedAt.y)
        if (found) found.rect = fromPage(found.rect)
        return found
    }
    function highlightSelection(color) {
        const made = marks.highlightSelection(color)
        clearSelection()
        return made
    }
    function selectionHighlight() { return marks.selectionHighlight() }
    function placeSelection(id) {
        const placed = marks.placeSelection(id)
        clearSelection()
        return placed
    }
    function penStroke(points) { return marks.penned(points.map(p => page.mapFromItem(flick, p.x, p.y))) }
    // The line at a point of the page or beside it, for a note written there by hand.
    function lineAt(x, y) { return noteMarks.lineAt(page.mapFromItem(flick, x, y).y) }
    function placeText() {
        const start = page.positionAt(page.width / 2, Math.max(0, contentY - page.y + 8))
        return page.getText(start, start + 120).replace(/\u00ad/g, "")
    }
    // The sentence read aloud stays in sight (#73): one below the screen or
    // above it is brought to a third of the way down, which keeps the place.
    function keepInSight(r) {
        const top = page.y + r.y
        if (top >= contentY && top + r.height <= contentY + height - page.font.pixelSize) return
        wanted = -1
        place(top - height / 3)
    }
    PlainCopy { id: plainCopy }

    onContentYChanged: {
        if (!ready || settling || contentHeight !== laidHeight || height !== laidView)
            return
        wanted = -1
        reader.fraction = span() > 0 ? contentY / span() : 0
        hold()
    }
    // The chapter was laid out again: its pictures arrived, the type
    // changed size, or the page changed width because a pane beside it
    // opened, closed or was dragged. A place that is still wanted is shown;
    // otherwise the page stays as far through the chapter as it was, which
    // the same number of pixels down no longer is.
    function relaid() {
        if (!ready) return
        if (wanted >= 0) settle()
        else if (span() > 0) place(held * span())
    }
    onContentHeightChanged: relaid()
    // Pulled past the end by a finger, on into the next chapter; past the
    // top, back to the end of the one before. A short pull only bounces.
    readonly property real pull: Math.min(120, height / 6)
    // The furthest the finger pulled past an end while it was down: the
    // view springs back as it lifts.
    property real pulled: 0
    onVerticalOvershootChanged: if (dragging && Math.abs(verticalOvershoot) > Math.abs(pulled)) pulled = verticalOvershoot
    onDraggingChanged: {
        const far = pulled
        pulled = 0
        if (dragging || !runsOn) return
        if (far > pull && reader.section + 1 < reader.count) pulledTo.going = 1
        else if (far < -pull && reader.section > 0) pulledTo.going = -1
        else return
        pulledTo.restart()
    }
    // Once the finger has let go: the flick it started is stopped, so the
    // chapter it leads to opens at its top (or the one before at its end).
    Timer {
        id: pulledTo
        interval: 0
        property int going: 0
        onTriggered: {
            flick.cancelFlick()
            if (going > 0) flick.next()
            else flick.backToEnd()
        }
    }
    onHeightChanged: relaid()
    // Once more when everything has its size. A page made for a book that
    // is already open (the window went from two pages to one) may be laid
    // out before it is told where the reader was, and nothing changes
    // afterwards to make it look. A timer of the view's own: a deferred
    // call would outlive it.
    Timer { id: settleSoon; interval: 0; onTriggered: flick.settle() }

    Connections {
        target: reader
        function onMoved(anchor, fraction) {
            flick.wanted = fraction
            flick.wantedAnchor = anchor
            flick.settle()
        }
    }

    TextEdit {
        id: page
        objectName: "flowText"
        y: flick.side + 8
        // With the margin column, the two are centred together, the column
        // at the end the lines end at.
        x: Math.round((flick.width - width - flick.noteRoom) / 2) + (win.rightToLeft ? flick.noteRoom : 0)
        width: Math.max(200, Math.min(reader.format.lineWidth, flick.width - 2 * flick.side - flick.noteRoom))
        readOnly: true
        selectByMouse: !flick.fingerDown
        persistentSelection: true
        activeFocusOnPress: false
        textFormat: TextEdit.RichText
        wrapMode: TextEdit.Wrap
        color: theme.foreground
        selectionColor: theme.accent
        selectedTextColor: theme.accentForeground
        font.pixelSize: flick.format.font_size
        font.family: flick.format.font_family !== "" ? flick.format.font_family : Qt.application.font.family
        // The chapter set as the reading format says (readingformat.h):
        // spacing, indent and alignment, and links in the accent.
        text: reader.format.html
        onWidthChanged: reader.pageWidth = Math.round(width)
        Component.onCompleted: {
            // Where the reader was, taken once: this view leads from here on.
            flick.wanted = reader.fraction
            flick.held = reader.fraction
            flick.ready = true
            reader.pageWidth = Math.round(width)
            reader.pageHeight = flick.pictureHeight
            flick.settle()
            settleSoon.restart()
        }
        onLinkActivated: (link) => { if (!reader.follow(link)) Qt.openUrlExternally(link) }

        PageHighlights { anchors.fill: parent; z: -1; text: page; others: flick.notes ? flick.notes.others : null }
        SentenceMark { anchors.fill: parent; z: -1; text: page; onMarked: (r) => flick.keepInSight(r) }
        PageMarks {
            id: marks
            anchors.fill: parent
            text: page
            on: flick.marking
            onHoveredChanged: flick.pointed(hovered ? Object.assign({}, hovered, { rect: flick.fromPage(hovered.rect) }) : null)
            onReleased: flick.selected()
        }
        NoteMarks {
            id: noteMarks
            anchors.fill: parent
            text: page
            reading: flick.notes
        }
        // Free ink (#152), across the window: the margins on either side of the text too.
        LineInkLayer { x: -page.x; width: flick.width; height: page.height; text: page; view: flick; covering: flick.covering; others: flick.notes ? flick.notes.others : null }
    }
    MarginColumn {
        id: margin
        visible: flick.noteRoom > 0
        x: win.rightToLeft ? page.x - flick.noteRoom : page.x + page.width + reader.format.gutter
        width: reader.format.noteWidth
        height: flick.contentHeight
        reading: flick.notes
        text: page
        laid: page.width + page.y
        place: function(position) { return page.y + page.positionToRectangle(position).y }
    }

    // The chapter's end, when reading runs on: a quiet line saying so, and
    // what comes next, which a click or a tap goes on to.
    Item {
        id: chapterEnd
        objectName: "chapterEnd"
        visible: flick.runsOn && reader.open
        readonly property bool last: reader.section + 1 >= reader.count
        x: page.x
        y: page.y + page.height + 32
        width: page.width
        height: endWords.implicitHeight + 28
        Accessible.role: last ? Accessible.StaticText : Accessible.Button
        Accessible.name: endWords.text

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width, 160)
            height: 1
            color: theme.colors.selection
        }
        Label {
            id: endWords
            objectName: "chapterEndText"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.pixelSize: 13
            color: endHover.hovered && !chapterEnd.last ? theme.foreground : theme.colors.muted
            text: {
                translator.language
                if (chapterEnd.last) return qsTr("End of the book")
                return reader.nextTitle !== "" ? qsTr("End of chapter · next: %1").arg(reader.nextTitle)
                                               : qsTr("End of chapter · on to the next")
            }
        }
        HoverHandler { id: endHover; cursorShape: chapterEnd.last ? Qt.ArrowCursor : Qt.PointingHandCursor }
        TapHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
            enabled: !chapterEnd.last
            onTapped: flick.next()
        }
    }
}
