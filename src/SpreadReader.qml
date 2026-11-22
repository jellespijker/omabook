import QtQuick
import QtQuick.Controls

// A flowing book as pages that turn: one, or two or three side by side like
// the book lying open (how many is PageLayout::columns' to say, through
// reader.format). The chapter is laid out once per page as a column and cut
// into pages where its lines allow; the first page shows one, the next the
// one after, and a turn moves on by the whole spread. A chapter starts on a
// spread of its own and its last holds what is left of it. Where the reader
// is, is the start of the first page.
Item {
    id: spread
    objectName: "spreadReader"
    // Without the soft hyphens hyphenation puts in (hyphenation.h), which are only for the eye.
    readonly property string selectedText: {
        for (const sheet of sheets)
            if (sheet.text.selectedText !== "") return sheet.text.selectedText.replace(/\u00ad/g, "")
        return ""
    }
    // Whether the graph's things are marked in the text.
    property bool marking: false
    // A finger is on the page: the text doesn't select under it as under a mouse.
    property bool fingerDown: false
    // The book's margin notes (ReadingNotes), and the room their column
    // takes beyond the spread's last page (PageLayout::columns).
    property var notes: null
    readonly property real noteRoom: notes && reader.format.noteWidth > 0 ? reader.format.noteWidth + gutter : 0
    property alias marginColumn: margin
    // What lies over the pages (the ink's bar), where the pen doesn't write on them.
    property Item covering: null
    // Counts the pages being cut anew or turned, which moves the notes.
    property int cut: 0
    onBreaksChanged: cut++
    onPageChanged: cut++
    // The thing under the pointer changed (null for none), and a selection
    // was made with the mouse.
    signal pointed(var thing)
    signal selected()
    // Nothing here scrolls sideways, which is what lets a swipe turn the page.
    readonly property real contentWidth: width
    // The options the text is set with (readingformat.h), and how they are
    // laid out across the room (pagelayout.h). The pages are as wide as the
    // measure allows; the margins are the room at the outer edges and above
    // and below.
    readonly property var format: reader.format.effective
    readonly property int columns: Math.max(1, Math.min(3, reader.format.columns))
    readonly property real gutter: reader.format.gutter
    readonly property real above: format.margins + 4
    readonly property real columnWidth: Math.max(200, reader.format.columnWidth)
    readonly property real pageHeight: Math.max(120, height - above * 2)
    // The pages that show, the first first.
    readonly property var sheets: columns === 3 ? [page0, page1, page2] : [page0, page1]
    readonly property Item firstText: page0.text
    // Where each page starts in the column, and where the last one ends.
    property var breaks: [0, 0]
    readonly property int pages: Math.max(1, breaks.length - 1)
    // The first page of the last spread.
    readonly property int lastLeft: Math.max(0, pages - 1 - (pages - 1) % columns)
    // The first page of the spread that shows.
    property int page: 0
    // The place to show once the chapter is laid out, until the reader turns a page.
    property real wanted: -1
    property string wantedAnchor: ""
    // Where in the chapter's text the first page starts, while the reader
    // is turning through it: what is shown again when the pages are cut
    // anew, or there are more or fewer of them side by side.
    property int placeAt: -1
    // Cutting the pages again waits a moment for the layout to finish, on a
    // timer that goes when this view does.
    Timer { id: later; interval: 0; onTriggered: spread.paginate() }

    function pageOf(y) {
        let index = 0
        for (let i = 0; i < pages; ++i)
            if (breaks[i] <= y + 0.5) index = i
        return index
    }
    function show(index) { page = Math.max(0, Math.min(lastLeft, index - index % columns)) }
    function startOf(index) { return firstText.positionAt(0, breaks[Math.min(index, breaks.length - 1)] + 1) }
    // Shows the wanted place. The column's height keeps changing while its
    // pictures arrive, so this runs each time the pages are cut again.
    function settle() {
        if (wanted < 0)
            return
        // A place in the chapter is as far through its text (which is how
        // a found word is told), and shows on the spread that has it.
        const anchored = wantedAnchor !== "" ? reader.anchorPosition(firstText.textDocument, wantedAnchor) : -1
        const at = anchored >= 0 ? anchored : Math.round(wanted * Math.max(0, firstText.length - 1))
        show(wanted >= 1 && anchored < 0 ? lastLeft : pageOf(firstText.positionToRectangle(at).y))
    }
    function paginate() {
        breaks = reader.pageBreaks(firstText.textDocument, pageHeight)
        // Cut again into more or fewer pages (the window or a pane beside
        // the page changed width, the type changed), the spread still holds
        // the words that began it, which the same page number no longer does.
        if (wanted >= 0) settle()
        else if (placeAt >= 0) show(pageOf(firstText.positionToRectangle(placeAt).y))
        else show(Math.round(reader.fraction * lastLeft))
    }
    function turnTo(index) {
        wanted = -1
        show(index)
        kept()
    }
    // A turn past the last spread goes on to the next chapter.
    function pageDown() {
        if (page >= lastLeft) next()
        else turnTo(page + columns)
    }
    function pageUp() {
        if (page <= 0) { if (reader.section > 0) reader.goTo(reader.section - 1, "", true) }
        else turnTo(page - columns)
    }
    function lineDown() { pageDown() }
    function lineUp() { pageUp() }
    function next() { reader.goTo(reader.section + 1) }
    // To the chapter's start, and from there to the one before.
    function previous() {
        if (page > 0) turnTo(0)
        else reader.goTo(reader.section - 1)
    }
    function toStart() { turnTo(0) }
    function toEnd() { turnTo(lastLeft) }
    function zoom(steps) { reader.format.zoom(steps) }
    property real pinched: 1
    function pinch(delta) {
        pinched *= delta
        if (pinched > 1.08) { zoom(1); pinched = 1 }
        else if (pinched < 0.92) { zoom(-1); pinched = 1 }
    }
    // The reading view's ways into the text; points and rectangles are in
    // this item's coordinates. Every page holds the whole chapter, each
    // showing its own stretch of it, so a point is asked of the page it is on.
    function pageAt(x, y) {
        for (const sheet of sheets) {
            if (!sheet.there) continue
            const at = sheet.text.mapFromItem(spread, x, y)
            if (at.x >= 0 && at.x <= sheet.width && at.y >= spread.breaks[sheet.index] && at.y <= spread.breaks[sheet.index + 1])
                return sheet
        }
        return null
    }
    // The page a place in the column shows on.
    function sheetShowing(y) {
        for (const sheet of sheets)
            if (sheet.there && y >= breaks[sheet.index] && y < breaks[sheet.index + 1])
                return sheet
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
        for (const sheet of sheets) {
            const chosen = sheet.marks.selection()
            if (!chosen) continue
            // On the page it shows on.
            chosen.rect = fromText(sheetShowing(chosen.rect.y) || sheet, chosen.rect)
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
    // The keyboard's selection starts at the first word of the first page,
    // and is shown on every page, each showing its part of it.
    function beginSelecting() {
        for (const sheet of sheets) if (sheet !== page0) sheet.text.deselect()
        page0.marks.begin(firstText.positionAt(0, breaks[page] + firstText.font.pixelSize))
        mirror()
    }
    function mirror() {
        for (const sheet of sheets)
            if (sheet !== page0) sheet.text.select(firstText.selectionStart, firstText.selectionEnd)
    }
    function stepSelecting(words, lines, extend) {
        page0.marks.step(words, lines, extend)
        mirror()
        // Kept in sight: on past the last page, back before the first.
        const r = page0.marks.activeRect()
        if (r.y >= breaks[Math.min(breaks.length - 1, page + columns)] && page < lastLeft) show(page + columns)
        else if (r.y < breaks[page] && page > 0) show(page - columns)
        kept()
    }
    // Where the reader is, after the selection took the spread on or back.
    function kept() {
        placeAt = startOf(page)
        reader.fraction = lastLeft > 0 ? page / lastLeft : 0
    }
    function endSelecting() { page0.marks.finish() }
    // A finger's selection (SelectHandles, #140), held on the first page's
    // text and shown on every page, as the keyboard's is. Its ends, on the
    // pages that show them; a point between or beside the pages is asked of
    // the nearest; a finger below the last page's lines turns the spread on,
    // above the first's back.
    function selectionEnds() {
        const sheet = selectedSheet()
        const ends = sheet ? sheet.marks.ends() : null
        if (!ends) return null
        const shown = (r) => { const on = sheetShowing(r.y); return on ? fromText(on, r) : null }
        return { start: shown(ends.start), end: shown(ends.end), rtl: ends.rtl }
    }
    function holdSelection(which) {
        const sheet = selectedSheet()
        if (!sheet) return false
        if (sheet !== page0) page0.text.select(sheet.text.selectionStart, sheet.text.selectionEnd)
        mirror()
        return page0.marks.hold(which)
    }
    function nearestSheet(x) {
        let best = null, off = Infinity
        for (const sheet of sheets) {
            const at = sheet.mapFromItem(spread, x, 0).x
            const away = at < 0 ? -at : Math.max(0, at - sheet.width)
            if (sheet.there && away < off) { off = away; best = sheet }
        }
        return best
    }
    function extendSelection(x, y) {
        const sheet = nearestSheet(x)
        if (!sheet) return false
        const at = sheet.text.mapFromItem(spread, x, y)
        const down = Math.max(breaks[sheet.index], Math.min(breaks[sheet.index + 1] - 2, at.y))
        const done = page0.marks.extendTo(sheet.text.positionAt(Math.max(0, Math.min(sheet.width, at.x)), down))
        mirror()
        return done
    }
    function edgeOf(x, y) {
        const sheet = nearestSheet(x)
        if (sheet && sheet.offset === Math.min(columns, pages - page) - 1 && y > Math.min(above + pageHeight, height - 24)) return 1
        return sheet === page0 && y < Math.max(above, 24) ? -1 : 0
    }
    function edgeStep(direction) {
        if (direction > 0 ? page >= lastLeft : page <= 0) return false
        show(page + direction * columns)
        kept()
        return true
    }
    function clearSelection() {
        page0.marks.finish()
        for (const sheet of sheets) sheet.text.deselect()
    }
    function copySelection() { if (selectedText !== "") plainCopy.copyText(selectedText) }
    // The selection, for a margin note: {text, start, end}; null for none.
    function selectionSpan() {
        for (const sheet of sheets) {
            if (sheet.text.selectedText === "") continue
            const start = sheet.text.selectionStart
            // The page it starts on, which a card about it stands by.
            const shown = sheetShowing(firstText.positionToRectangle(start).y) || sheet
            return { text: shown.text, start: start, end: sheet.text.selectionEnd }
        }
        return null
    }
    // Where a character of the chapter is on the spread: how far down the
    // page that shows it, and which page that is; null where none does.
    function onSpread(position) {
        const y = firstText.positionToRectangle(position).y
        const sheet = sheetShowing(y)
        return sheet ? { y: y - breaks[sheet.index], sheet: sheet } : null
    }
    // The notes whose passages start on the spread, page by page, top to bottom.
    function notesInSight() {
        if (!notes) return []
        const seen = []
        for (const span of notes.notes.spans(firstText.textDocument)) {
            const at = onSpread(span.start)
            if (at) seen.push({ id: span.id, text: at.sheet.text, start: span.start, end: span.end, order: at.sheet.offset * 1e6 + at.y })
        }
        return seen.sort((a, b) => a.order - b.order)
    }
    // Highlights (#81), as FlowReader's: a page asked about is the one under the point.
    function highlightAtPoint(x, y) {
        const sheet = pageAt(x, y)
        if (!sheet) return null
        const at = sheet.text.mapFromItem(spread, x, y)
        const found = sheet.marks.highlightAt(at.x, at.y)
        if (found) found.rect = fromText(sheet, found.rect)
        return found
    }
    function pressedHighlight() {
        for (const sheet of sheets) {
            const at = sheet.marks.pressedAt
            if (!sheet.there || at.y < spread.breaks[sheet.index] || at.y > spread.breaks[sheet.index + 1]) continue
            const found = sheet.marks.highlightAt(at.x, at.y)
            if (found) found.rect = fromText(sheet, found.rect)
            if (found) return found
        }
        return null
    }
    // The selection is the same on every page that shows part of it; the first that has it says.
    function selectedSheet() {
        for (const sheet of sheets) if (sheet.marks.selectionSpan()) return sheet
        return null
    }
    function highlightSelection(color) {
        const sheet = selectedSheet()
        const made = sheet ? sheet.marks.highlightSelection(color) : 0
        clearSelection()
        return made
    }
    function selectionHighlight() { const sheet = selectedSheet(); return sheet ? sheet.marks.selectionHighlight() : ({}) }
    function placeSelection(id) {
        const sheet = selectedSheet()
        const placed = sheet ? sheet.marks.placeSelection(id) : false
        clearSelection()
        return placed
    }
    function penStroke(points) {
        const sheet = points.length > 0 ? pageAt(points[0].x, points[0].y) : null
        return sheet ? sheet.marks.penned(points.map(p => sheet.text.mapFromItem(spread, p.x, p.y))) : { kind: "" }
    }
    // The line at a point of a page, or beside the pages in the margin
    // column on the page nearest it, for a note written there by hand.
    function lineAt(x, y) {
        const under = pageAt(x, y)
        let sheet = under
        let nearest = Infinity
        for (const one of sheets) {
            const off = Math.abs(one.mapFromItem(spread, x, y).x - one.width / 2)
            if (!under && one.there && off < nearest) {
                nearest = off
                sheet = one
            }
        }
        return sheet ? sheet.noteMarks.lineAt(sheet.text.mapFromItem(spread, x, y).y) : null
    }
    function placeText() {
        const start = firstText.positionAt(columnWidth / 2, breaks[page] + 8)
        return firstText.getText(start, start + 120).replace(/\u00ad/g, "")
    }
    // The sentence read aloud (#73) on a page past the spread, or before it: the spread that has it.
    function keepInSight(r) {
        const index = pageOf(r.y)
        if (index < page || index >= page + columns) turnTo(index)
    }
    PlainCopy { id: plainCopy }

    onPageHeightChanged: later.restart()
    // Another page more or fewer: the same pages, as a spread of another size.
    onColumnsChanged: later.restart()
    Connections {
        target: reader
        function onMoved(anchor, fraction) {
            spread.wanted = fraction
            spread.wantedAnchor = anchor
            spread.placeAt = -1
            spread.settle()
        }
        function onSectionChanged() { spread.placeAt = -1 }
    }

    Row {
        id: sheetsRow
        anchors.horizontalCenter: parent.horizontalCenter
        // With the margin column, the two are centred together; the
        // window's mirroring turns the offset round in a right-to-left language.
        anchors.horizontalCenterOffset: -spread.noteRoom / 2
        y: spread.above
        spacing: spread.gutter
        // The spread reads in the direction of the language: the window's
        // mirroring turns this round in a right-to-left one.
        layoutDirection: Qt.LeftToRight

        SpreadPage { id: page0; objectName: "page0"; offset: 0; spread: spread }
        SpreadPage { id: page1; objectName: "page1"; offset: 1; spread: spread }
        SpreadPage { id: page2; objectName: "page2"; offset: 2; spread: spread }
    }

    // The margin notes of the spread's pages, beyond its last, each at the
    // height of its passage on the page that shows it.
    MarginColumn {
        id: margin
        visible: spread.noteRoom > 0
        x: win.rightToLeft ? sheetsRow.x - spread.noteRoom : sheetsRow.x + sheetsRow.width + spread.gutter
        y: spread.above
        width: reader.format.noteWidth
        height: spread.pageHeight
        foot: spread.pageHeight
        reading: spread.notes
        text: spread.firstText
        laid: spread.cut
        place: function(position) {
            const at = spread.onSpread(position)
            return at ? at.y : NaN
        }
    }

    Binding { target: page0.text; property: "objectName"; value: "flowText" }
    Connections {
        target: page0.text
        // The column was laid out again: new text, another size of type, a
        // picture that arrived.
        function onContentHeightChanged() { later.restart() }
        // Set anew: another chapter, or the reading format changed, which
        // can move where lines fall without changing the column's height.
        function onTextChanged() { later.restart() }
    }
    onColumnWidthChanged: reader.pageWidth = Math.round(columnWidth)
    // A picture fits on a page: no taller than the page less a line, with
    // the line spacing it is set in taken off.
    readonly property int pictureHeight: Math.floor((pageHeight - format.font_size * 2) * 100 / Math.max(100, format.line_height))
    onPictureHeightChanged: reader.pageHeight = pictureHeight
    Component.onCompleted: {
        // Where the reader was, taken once: this view leads from here on.
        wanted = reader.fraction
        reader.pageWidth = Math.round(columnWidth)
        reader.pageHeight = pictureHeight
        later.restart()
    }
}
