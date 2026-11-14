import QtQuick
import QtQuick.Controls

// A flowing book: one chapter at a time, set in the reader's own font and
// the desktop's colors, in a column no wider than reads comfortably.
Flickable {
    id: flick
    objectName: "flowReader"
    readonly property string selectedText: page.selectedText
    readonly property real range: Math.max(0, contentHeight - height)
    // The place to show once the chapter is laid out, until the reader scrolls.
    property real wanted: -1
    property string wantedAnchor: ""
    property bool settling: false

    contentWidth: width
    contentHeight: page.height + 96
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    focus: true
    ScrollBar.vertical: ScrollBar {}

    function place(y) {
        settling = true
        contentY = Math.max(0, Math.min(range, y))
        settling = false
        // Before the chapter is laid out there is nowhere to be yet.
        if (range > 0)
            reader.fraction = contentY / range
    }
    // Shows the wanted place. A chapter's height keeps changing while its
    // pictures arrive, so this runs again each time it does.
    function settle() {
        if (wanted < 0)
            return
        const at = wantedAnchor !== "" ? reader.anchorPosition(page.textDocument, wantedAnchor) : -1
        place(at >= 0 ? page.y + page.positionToRectangle(at).y - 24 : wanted * range)
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
        if (settling)
            return
        wanted = -1
        reader.fraction = range > 0 ? contentY / range : 0
    }
    onContentHeightChanged: settle()
    onHeightChanged: settle()

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
            reader.pageWidth = Math.round(width)
        }
        onLinkActivated: (link) => { if (!reader.follow(link)) Qt.openUrlExternally(link) }

        HoverHandler { cursorShape: page.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor }
    }
}
