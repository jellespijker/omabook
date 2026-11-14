import QtQuick
import QtQuick.Controls

// A flowing book: one chapter at a time, set in the reader's own font and
// the desktop's colors, in a column no wider than reads comfortably.
Flickable {
    id: flick
    objectName: "flowReader"
    readonly property string selectedText: page.selectedText
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

    contentWidth: width
    contentHeight: page.height + 96
    boundsBehavior: Flickable.StopAtBounds
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
    // A page down at the end of a chapter turns to the next one.
    function pageDown() {
        if (contentY >= range - 1) next()
        else scrollBy(height - page.font.pixelSize * 3)
    }
    function pageUp() {
        if (contentY <= 1) { if (reader.section > 0) reader.goTo(reader.section - 1, "", true) }
        else scrollBy(-(height - page.font.pixelSize * 3))
    }
    function lineDown() { scrollBy(page.font.pixelSize * 4) }
    function lineUp() { scrollBy(-page.font.pixelSize * 4) }
    function next() { reader.goTo(reader.section + 1) }
    function previous() { reader.goTo(reader.section - 1) }
    function toStart() { scrollBy(-contentY) }
    function toEnd() { scrollBy(range) }
    function zoom(steps) { settings.fontSize = settings.fontSize + steps }
    // A pinch changes the type a size at a time as the fingers spread.
    property real pinched: 1
    function pinch(delta) {
        pinched *= delta
        if (pinched > 1.08) { zoom(1); pinched = 1 }
        else if (pinched < 0.92) { zoom(-1); pinched = 1 }
    }
    // The words at the top of the window, to name a bookmark by.
    function placeText() {
        const start = page.positionAt(page.width / 2, Math.max(0, contentY - page.y + 8))
        return page.getText(start, start + 120)
    }

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
        y: 40
        x: Math.round((flick.width - width) / 2)
        width: Math.max(200, Math.min(settings.lineWidth, flick.width - 64))
        readOnly: true
        selectByMouse: true
        persistentSelection: true
        activeFocusOnPress: false
        textFormat: TextEdit.RichText
        wrapMode: TextEdit.Wrap
        color: theme.foreground
        selectionColor: theme.accent
        selectedTextColor: theme.accentForeground
        font.pixelSize: settings.fontSize
        font.family: settings.fontFamily !== "" ? settings.fontFamily : Qt.application.font.family
        // Links take the accent; paragraphs get room to breathe.
        text: "<style>a { color: " + theme.accent + "; text-decoration: none; } "
              + "p, li, blockquote, dd { line-height: " + settings.lineHeight + "%; } "
              + "p { margin-top: 0; margin-bottom: " + settings.paragraphSpacing + "px;"
              + (settings.justify ? " text-align: justify;" : "") + " } "
              + "h1, h2, h3 { margin-top: 24px; margin-bottom: 16px; }</style>" + reader.html
        onWidthChanged: reader.pageWidth = Math.round(width)
        Component.onCompleted: {
            // Where the reader was, taken once: this view leads from here on.
            flick.wanted = reader.fraction
            flick.held = reader.fraction
            flick.ready = true
            reader.pageWidth = Math.round(width)
            flick.settle()
            settleSoon.restart()
        }
        onLinkActivated: (link) => { if (!reader.follow(link)) Qt.openUrlExternally(link) }

        HoverHandler { cursorShape: page.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor }
    }
}
