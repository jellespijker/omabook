import QtQuick
import QtQuick.Controls

// A flowing book in a wide window: two pages side by side, like the book
// lying open. The chapter is laid out once as a column and cut into pages
// where its lines allow; the left page shows one, the right the next, and a
// turn moves on by two.
Item {
    id: spread
    objectName: "spreadReader"
    readonly property string selectedText: leftText.selectedText !== "" ? leftText.selectedText : rightText.selectedText
    // Nothing here scrolls sideways, which is what lets a swipe turn the page.
    readonly property real contentWidth: width
    readonly property real gutter: 56
    readonly property real columnWidth: Math.max(200, Math.min(settings.lineWidth, (width - gutter * 3) / 2))
    readonly property real pageHeight: Math.max(120, height - 72)
    // Where each page starts in the column, and where the last one ends.
    property var breaks: [0, 0]
    readonly property int pages: Math.max(1, breaks.length - 1)
    // The left page of the last spread.
    readonly property int lastLeft: Math.max(0, pages - 1 - (pages - 1) % 2)
    // The page on the left.
    property int page: 0
    // The place to show once the chapter is laid out, until the reader turns a page.
    property real wanted: -1
    property string wantedAnchor: ""
    // Cutting the pages again waits a moment for the layout to finish, on a
    // timer that goes when this view does.
    Timer { id: later; interval: 0; onTriggered: spread.paginate() }

    function pageOf(y) {
        let index = 0
        for (let i = 0; i < pages; ++i)
            if (breaks[i] <= y + 0.5) index = i
        return index
    }
    function show(index) { page = Math.max(0, Math.min(lastLeft, index - index % 2)) }
    // Shows the wanted place. The column's height keeps changing while its
    // pictures arrive, so this runs each time the pages are cut again.
    function settle() {
        if (wanted < 0)
            return
        const at = wantedAnchor !== "" ? reader.anchorPosition(leftText.textDocument, wantedAnchor) : -1
        show(at >= 0 ? pageOf(leftText.positionToRectangle(at).y) : Math.round(wanted * lastLeft))
    }
    function paginate() {
        breaks = reader.pageBreaks(leftText.textDocument, pageHeight)
        if (wanted >= 0) settle()
        else show(page)
    }
    function turnTo(index) {
        wanted = -1
        show(index)
        reader.fraction = lastLeft > 0 ? page / lastLeft : 0
    }
    // A turn past the last spread goes on to the next chapter.
    function pageDown() {
        if (page >= lastLeft) next()
        else turnTo(page + 2)
    }
    function pageUp() {
        if (page <= 0) { if (reader.section > 0) reader.goTo(reader.section - 1, "", true) }
        else turnTo(page - 2)
    }
    function lineDown() { pageDown() }
    function lineUp() { pageUp() }
    function next() { reader.goTo(reader.section + 1) }
    function previous() { reader.goTo(reader.section - 1) }
    function toStart() { turnTo(0) }
    function toEnd() { turnTo(lastLeft) }
    function zoom(steps) { settings.fontSize = settings.fontSize + steps }
    property real pinched: 1
    function pinch(delta) {
        pinched *= delta
        if (pinched > 1.08) { zoom(1); pinched = 1 }
        else if (pinched < 0.92) { zoom(-1); pinched = 1 }
    }
    // The words at the top of the left page, to name a bookmark by.
    function placeText() {
        const start = leftText.positionAt(columnWidth / 2, breaks[page] + 8)
        return leftText.getText(start, start + 120)
    }

    onPageHeightChanged: later.restart()
    Connections {
        target: reader
        function onMoved(anchor, fraction) {
            spread.wanted = fraction
            spread.wantedAnchor = anchor
            spread.settle()
        }
    }

    // One page: a window onto the column, as tall as its page of lines.
    component Page: Item {
        id: sheet
        property int index: 0
        property alias text: column
        readonly property bool there: index < spread.pages
        // Every page takes its place in the spread, written on or not; what
        // shows of the column is cut off after the page's last whole line.
        width: spread.columnWidth
        height: spread.pageHeight

        Item {
            width: parent.width
            height: sheet.there ? Math.max(0, spread.breaks[sheet.index + 1] - spread.breaks[sheet.index]) : 0
            clip: true

        TextEdit {
            id: column
            y: sheet.there ? -spread.breaks[sheet.index] : 0
            width: spread.columnWidth
            visible: sheet.there
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
            text: "<style>a { color: " + theme.accent + "; text-decoration: none; } "
              + "p, li, blockquote, dd { line-height: " + settings.lineHeight + "%; } "
              + "p { margin-top: 0; margin-bottom: " + settings.paragraphSpacing + "px;"
              + (settings.justify ? " text-align: justify;" : "") + " } "
              + "h1, h2, h3 { margin-top: 24px; margin-bottom: 16px; }</style>" + reader.html
            onLinkActivated: (link) => { if (!reader.follow(link)) Qt.openUrlExternally(link) }
            HoverHandler { cursorShape: column.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor }
        }
        }
    }

    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 36
        spacing: spread.gutter
        // The spread reads in the direction of the language.
        layoutDirection: win.rightToLeft ? Qt.RightToLeft : Qt.LeftToRight

        Page {
            id: leftPage
            objectName: "leftPage"
            index: spread.page
        }
        Page {
            id: rightPage
            objectName: "rightPage"
            index: spread.page + 1
        }
    }
    readonly property alias leftText: leftPage.text
    readonly property alias rightText: rightPage.text

    Binding { target: leftPage.text; property: "objectName"; value: "flowText" }
    Connections {
        target: leftPage.text
        // The column was laid out again: new text, another size of type, a
        // picture that arrived.
        function onContentHeightChanged() { later.restart() }
    }
    onColumnWidthChanged: reader.pageWidth = Math.round(columnWidth)
    Component.onCompleted: {
        // Where the reader was, taken once: this view leads from here on.
        wanted = reader.fraction
        reader.pageWidth = Math.round(columnWidth)
        later.restart()
    }

    // The fold between the pages.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 36
        width: 1
        height: spread.pageHeight
        color: theme.colors.selection
        opacity: 0.6
    }
}
