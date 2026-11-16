import QtQuick
import QtQuick.Controls

// A book of fixed pages (a PDF, a comic), each drawn off the GUI thread at
// the size it is shown. As the layout says (readingformat.h): one under
// another, scrolling, each as wide as the window has room for; or, as
// pages, each shown whole in the window, a turn showing exactly the next.
// A wide window shows two side by side, like the book lying open; the first
// stands alone, as a printed book opens (1, 2|3, 4|5), or pairs from the
// start (1|2, 3|4), as the first_page option says.
ListView {
    id: view
    objectName: "pageReader"
    // "auto", "pages" or "scroll".
    property string layout: "auto"
    readonly property bool turnsPages: layout === "pages"
    // Pages fit the window's width up to this many pixels, times the zoom.
    property real zoomFactor: 1
    // A wide window shows two pages side by side, like the book lying open.
    readonly property bool twoUp: width >= win.spreadFloor && width > height * 1.15 && reader.count > 1
    readonly property int across: twoUp ? 2 : 1
    // The first page alone: chosen, or by itself for a PDF whose first page
    // is taller than wide (a book's, a paper's), and not for slides or a
    // comic, whose first page's shape isn't known before it is drawn.
    readonly property string firstPage: reader.format.effective.first_page || "auto"
    readonly property bool firstAlone: twoUp && (firstPage === "alone" || (firstPage === "auto" && reader.pageAspect(0) > 1))
    readonly property int pageCount: reader.count
    // The room one page has across, and, as pages, down.
    readonly property real columnRoom: Math.max(120, (width - 32 - (across - 1) * gap) / across)
    readonly property real rowRoom: Math.max(120, height - 24)
    readonly property real pageWidth: Math.max(120, Math.min(columnRoom, 920) * zoomFactor)
    readonly property string selectedText: ""
    // Between pages, across and down.
    readonly property real gap: 12

    // A page as wide as it is shown: fitting the width, or, as pages, the
    // window, whole.
    function widthOf(aspect) {
        const fits = turnsPages ? Math.min(columnRoom, rowRoom / Math.max(0.05, aspect)) : Math.min(columnRoom, 920)
        return Math.max(120, fits * zoomFactor)
    }
    // Rows: a page, or two side by side.
    readonly property int rows: !twoUp ? reader.count : firstAlone ? 1 + Math.ceil((reader.count - 1) / 2) : Math.ceil(reader.count / 2)
    function rowOf(page) { return !twoUp ? page : firstAlone ? (page === 0 ? 0 : Math.floor((page + 1) / 2)) : Math.floor(page / 2) }
    function firstOf(row) { return !twoUp ? row : firstAlone ? (row === 0 ? 0 : 2 * row - 1) : 2 * row }
    function sizeOf(row) { return Math.max(0, Math.min(twoUp && !(firstAlone && row === 0) ? 2 : 1, reader.count - firstOf(row))) }
    // The page a row stands for: its first, and the last page for the last
    // row, so that reaching it is the whole book read.
    function placeOf(row) { return row >= rows - 1 ? reader.count - 1 : firstOf(row) }

    model: rows
    spacing: turnsPages ? 0 : gap
    clip: true
    focus: true
    boundsBehavior: Flickable.StopAtBounds
    // As pages they turn, by the keys, the wheel, a swipe and the page's
    // edges; only a page zoomed larger than the window is moved by hand.
    interactive: !turnsPages || zoomFactor > 1.01
    cacheBuffer: Math.max(0, height)
    contentWidth: Math.max(width, widthOf(1.414) * across + (across - 1) * gap + 32)
    flickableDirection: Flickable.AutoFlickIfNeeded
    ScrollBar.vertical: ScrollBar {}
    ScrollBar.horizontal: ScrollBar {}

    function scrollBy(distance) {
        contentY = Math.max(originY, Math.min(originY + contentHeight - height, contentY + distance))
    }
    readonly property int row: rowOf(reader.section)
    function showRow(index) {
        const to = Math.max(0, Math.min(rows - 1, index))
        positionViewAtIndex(to, ListView.Beginning)
        reader.section = placeOf(to)
    }
    function nextRow() { showRow(rowAtTop() + 1) }
    function previousRow() { showRow(rowAtTop() - 1) }
    function rowAtTop() {
        const at = indexAt(contentX + width / 2, contentY + 1)
        return at >= 0 ? at : row
    }
    // Scrolling, a screen's step lines up with the next page's top when it
    // is in sight, so a page is never shown in part again: a page taller
    // than the window goes a window at a time, never showing a line twice.
    function screen(forward) {
        const top = rowAtTop()
        if (forward) {
            const next = itemAtIndex(top + 1)
            const to = next && next.y > contentY + 1 && next.y <= contentY + height ? next.y : contentY + height
            scrollBy(to - contentY)
        } else {
            // The highest page top that is less than a window above.
            let to = contentY - height
            for (let index = top; index >= 0; --index) {
                const item = itemAtIndex(index)
                if (!item || item.y < contentY - height - 1) break
                if (item.y < contentY - 1) to = item.y
            }
            scrollBy(to - contentY)
        }
    }
    // As pages a turn is the next page or pair; side by side too. Scrolling,
    // a screen; automatically, most of a window, as it always was.
    function pageDown() { if (turnsPages || (twoUp && layout === "auto")) nextRow(); else if (layout === "scroll") screen(true); else scrollBy(height * 0.9) }
    function pageUp() { if (turnsPages || (twoUp && layout === "auto")) previousRow(); else if (layout === "scroll") screen(false); else scrollBy(-height * 0.9) }
    function lineDown() { if (turnsPages && !interactive) nextRow(); else scrollBy(80) }
    function lineUp() { if (turnsPages && !interactive) previousRow(); else scrollBy(-80) }
    // A chapter is an entry of the outline; a book without one goes a page
    // (or a pair) at a time.
    function chapterStarts() {
        const starts = []
        for (const entry of reader.toc)
            if (entry.section !== undefined && starts.indexOf(entry.section) < 0) starts.push(entry.section)
        return starts.sort((a, b) => a - b)
    }
    function next() {
        const after = chapterStarts().filter(page => rowOf(page) > row)
        if (after.length > 0) showRow(rowOf(after[0]))
        else nextRow()
    }
    // To the start of the chapter, and from there to the one before.
    function previous() {
        const starts = chapterStarts()
        const before = starts.filter(page => rowOf(page) < row)
        if (starts.length === 0) previousRow()
        else showRow(before.length > 0 ? rowOf(before[before.length - 1]) : 0)
    }
    function toStart() { positionViewAtBeginning(); reader.section = 0 }
    function toEnd() { positionViewAtEnd(); reader.section = reader.count - 1 }
    function zoom(steps) { zoomFactor = Math.max(0.4, Math.min(4, zoomFactor * (steps > 0 ? 1.15 : 1 / 1.15))) }
    function pinch(delta) { zoomFactor = Math.max(0.4, Math.min(4, zoomFactor * delta)) }
    function placeText() { return qsTr("Page %1").arg(reader.section + 1) }

    // The page under the upper part of the window is the one being read.
    onContentYChanged: {
        if (!settled) return
        const at = indexAt(contentX + width / 2, contentY + height / 3)
        if (at >= 0)
            reader.section = placeOf(at)
    }
    property bool settled: false
    Component.onCompleted: {
        positionViewAtIndex(row, ListView.Beginning)
        settled = true
    }
    // Turning the window on its side, another layout, the first page alone
    // or not: the same page, in its new row.
    function keepPlace() {
        const page = reader.section
        settled = false
        Qt.callLater(() => {
            positionViewAtIndex(rowOf(page), ListView.Beginning)
            reader.section = page
            settled = true
        })
    }
    onAcrossChanged: keepPlace()
    onFirstAloneChanged: keepPlace()
    onTurnsPagesChanged: keepPlace()
    Connections {
        target: reader
        function onMoved(anchor, fraction) { view.positionViewAtIndex(view.row, ListView.Beginning) }
    }

    delegate: Item {
        id: sheets
        required property int index
        width: view.contentWidth
        // As pages, a row is the window's height, the pages in its middle.
        height: view.turnsPages ? Math.max(view.height, pair.height) : pair.height

        Row {
            id: pair
            objectName: "pageRow" + sheets.index
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: view.turnsPages ? parent.verticalCenter : undefined
            spacing: view.gap
            layoutDirection: win.rightToLeft ? Qt.RightToLeft : Qt.LeftToRight

            Repeater {
                model: view.sizeOf(sheets.index)
                delegate: Rectangle {
                    id: sheet
                    required property int index
                    readonly property int page: view.firstOf(sheets.index) + index
                    objectName: "pdfPage" + page
                    // A page's shape is known at once for a PDF and once drawn for a comic.
                    readonly property real aspect: {
                        const known = reader.pageAspect(page)
                        if (known > 0) return known
                        return picture.status === Image.Ready && picture.implicitWidth > 0
                            ? picture.implicitHeight / picture.implicitWidth : 1.414
                    }
                    width: Math.round(view.widthOf(aspect))
                    height: Math.round(width * aspect)
                    color: "white"

                    Image {
                        id: picture
                        anchors.fill: parent
                        asynchronous: true
                        // The width in the address is in device pixels, so pages are sharp on a HiDPI screen.
                        source: reader.imagePrefix + "page/" + sheet.page + "/" + Math.round(sheet.width * Screen.devicePixelRatio)
                        fillMode: Image.PreserveAspectFit
                    }
                }
            }
        }
    }
}
