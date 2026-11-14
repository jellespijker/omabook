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
    // Whether the graph's things are marked in the text.
    property bool marking: false
    // The thing under the pointer changed (null for none), and a selection
    // was made with the mouse.
    signal pointed(var thing)
    signal selected()
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
        // Cut again into more or fewer pages (the window or a pane beside
        // the page changed width), the spread stays as far through the
        // chapter as it was, which the same page number no longer is.
        if (wanted >= 0) settle()
        else show(Math.round(reader.fraction * lastLeft))
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
    // The reading view's ways into the text; points and rectangles are in
    // this item's coordinates. Both pages hold the whole chapter, each
    // showing its own stretch of it, so a point is asked of the page it is on.
    function pageAt(x, y) {
        for (const sheet of [leftPage, rightPage]) {
            if (!sheet.there) continue
            const at = sheet.text.mapFromItem(spread, x, y)
            if (at.x >= 0 && at.x <= sheet.width && at.y >= spread.breaks[sheet.index] && at.y <= spread.breaks[sheet.index + 1])
                return sheet
        }
        return null
    }
    function fromText(sheet, rect) {
        const at = sheet.text.mapToItem(spread, rect.x, rect.y)
        return Qt.rect(at.x, at.y, rect.width, rect.height)
    }
    function thingAt(x, y) {
        const sheet = pageAt(x, y)
        if (!sheet) return null
        const at = sheet.text.mapFromItem(spread, x, y)
        const found = sheet.marks.thingAt(at.x, at.y)
        if (found) found.rect = fromText(sheet, found.rect)
        return found
    }
    function selection() {
        for (const sheet of [leftPage, rightPage]) {
            const chosen = sheet.marks.selection()
            if (!chosen) continue
            // On the page it shows on.
            const onRight = sheet === leftPage && rightPage.there && chosen.rect.y >= spread.breaks[rightPage.index]
            chosen.rect = fromText(onRight ? rightPage : sheet, chosen.rect)
            return chosen
        }
        return null
    }
    function selectAt(x, y) {
        const sheet = pageAt(x, y)
        if (!sheet) return false
        const at = sheet.text.mapFromItem(spread, x, y)
        return sheet.marks.selectWordAt(at.x, at.y)
    }
    // The keyboard's selection starts at the first word of the left page,
    // and is shown on both pages, each showing its part of it.
    function beginSelecting() {
        rightText.deselect()
        leftPage.marks.begin(leftText.positionAt(0, breaks[page] + leftText.font.pixelSize))
        mirror()
    }
    function mirror() { rightText.select(leftText.selectionStart, leftText.selectionEnd) }
    function stepSelecting(words, lines, extend) {
        leftPage.marks.step(words, lines, extend)
        mirror()
        // Kept in sight: on past the right page, back before the left.
        const r = leftPage.marks.activeRect()
        if (r.y >= breaks[Math.min(breaks.length - 1, page + 2)] && page < lastLeft) show(page + 2)
        else if (r.y < breaks[page] && page > 0) show(page - 2)
        reader.fraction = lastLeft > 0 ? page / lastLeft : 0
    }
    function endSelecting() { leftPage.marks.finish() }
    function clearSelection() {
        leftPage.marks.finish()
        leftText.deselect()
        rightText.deselect()
    }
    function copySelection() {
        if (leftText.selectedText !== "") leftText.copy()
        else rightText.copy()
    }
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
        property alias marks: pageMarks
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
            PageMarks {
                id: pageMarks
                anchors.fill: parent
                text: column
                on: spread.marking && sheet.there
                onHoveredChanged: spread.pointed(hovered ? Object.assign({}, hovered, { rect: spread.fromText(sheet, hovered.rect) }) : null)
                onReleased: spread.selected()
            }
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
