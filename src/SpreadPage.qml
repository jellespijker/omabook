import QtQuick

// One page of a spread (SpreadReader): a window onto the chapter's column,
// as tall as its page of lines, cut off after its last whole line. Its free
// ink (#152) reaches past the page into half the gutter on either side, the
// room of its margins, and moves with the column as the pages turn.
Item {
    id: sheet
    // The spread it is a page of.
    property Item spread
    // Which of the spread's pages it is, counted from the first.
    property int offset: 0
    readonly property int index: spread.page + offset
    readonly property bool shown: offset < spread.columns
    property alias text: column
    property alias marks: pageMarks
    property alias noteMarks: sheetNotes
    readonly property bool there: shown && index < spread.pages
    // Every page takes its place in the spread, written on or not; what
    // shows of the column is cut off after the page's last whole line.
    visible: shown
    width: spread.columnWidth
    height: spread.pageHeight

    Item {
        id: shownPart
        width: parent.width
        // At most a page: what lies between the end of its lines and
        // the start of the next page's is only space.
        height: sheet.there ? Math.max(0, Math.min(spread.pageHeight, spread.breaks[sheet.index + 1] - spread.breaks[sheet.index])) : 0
        clip: true

        TextEdit {
            id: column
            y: sheet.there ? -spread.breaks[sheet.index] : 0
            width: spread.columnWidth
            visible: sheet.there
            readOnly: true
            selectByMouse: !spread.fingerDown
            persistentSelection: true
            activeFocusOnPress: false
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            color: theme.foreground
            selectionColor: theme.accent
            selectedTextColor: theme.accentForeground
            font.pixelSize: spread.format.font_size
            font.family: spread.format.font_family !== "" ? spread.format.font_family : Qt.application.font.family
            // A page that isn't part of the spread lays nothing out.
            text: sheet.shown ? reader.format.html : ""
            onLinkActivated: (link) => { if (!reader.follow(link)) Qt.openUrlExternally(link) }
            PageHighlights { anchors.fill: parent; z: -1; text: column; visible: sheet.there; others: spread.notes ? spread.notes.others : null }
            SentenceMark { anchors.fill: parent; z: -1; text: column; visible: sheet.there; onMarked: (r) => { if (sheet.offset === 0) spread.keepInSight(r) } }
            PageMarks {
                id: pageMarks
                anchors.fill: parent
                text: column
                on: spread.marking && sheet.there
                onHoveredChanged: spread.pointed(hovered ? Object.assign({}, hovered, { rect: spread.fromText(sheet, hovered.rect) }) : null)
                onReleased: spread.selected()
            }
            NoteMarks {
                id: sheetNotes
                anchors.fill: parent
                visible: sheet.there
                text: column
                reading: spread.notes
                from: sheet.there ? spread.breaks[sheet.index] : 0
                to: sheet.there ? spread.breaks[sheet.index + 1] : 0
            }
        }
    }
    // Free ink (#152), on the page and beside it, as far down as the page shows.
    Item {
        id: inked
        x: -spread.gutter / 2
        width: sheet.width + spread.gutter
        height: shownPart.height
        visible: sheet.there
        clip: true
        LineInkLayer {
            y: column.y
            width: parent.width
            height: column.height
            text: column
            view: spread
            covering: spread.covering
            others: spread.notes ? spread.notes.others : null
            within: inked
        }
    }
    // The fold before the next page, halfway across the gutter.
    Rectangle {
        visible: sheet.offset + 1 < spread.columns
        x: win.rightToLeft ? -spread.gutter / 2 : sheet.width + spread.gutter / 2
        width: 1
        height: spread.pageHeight
        color: theme.colors.selection
        opacity: 0.6
    }
}
