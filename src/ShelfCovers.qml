import QtQuick
import QtQuick.Controls

// The library as covers: every book face out, in a grid.
//
// Every way of showing the library is a file like this one, and they all
// answer the same few things, which is all LibraryView knows of them:
// `currentRow` (the selected book's row in backend.books), `show(row)`,
// `move("left" | "right" | "up" | "down")`, `activate()` (Space and Enter),
// `back()` (Escape; true when it did something), `ownDetails` (the view
// shows the selected book's details itself), and the signals `opened(id)`
// and `detailsWanted()`.
FocusScope {
    id: shelf
    property alias currentRow: grid.currentIndex
    readonly property bool ownDetails: false
    signal opened(var bookId)
    signal detailsWanted()

    function show(row) { grid.currentIndex = row }
    function move(where) {
        if (where === "left") grid.moveCurrentIndexLeft()
        else if (where === "right") grid.moveCurrentIndexRight()
        else if (where === "up") grid.moveCurrentIndexUp()
        else grid.moveCurrentIndexDown()
    }
    function activate() { if (grid.currentIndex >= 0 && grid.count > 0) shelf.opened(backend.books.idAt(grid.currentIndex)) }
    function back() { return false }

    GridView {
        id: grid
        objectName: "grid"
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        clip: true
        focus: true
        model: backend.books
        readonly property int columns: Math.max(1, Math.floor(width / settings.coverSize))
        cellWidth: width / columns
        cellHeight: cellWidth * 1.42 + 52
        keyNavigationEnabled: true
        highlightFollowsCurrentItem: true
        highlight: Item {}
        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: cell
            required property int index
            required property var bookId
            required property string title
            required property string authors
            required property url cover
            required property real percent
            required property string status
            required property string held
            required property int rating
            width: grid.cellWidth
            height: grid.cellHeight

            BookCover {
                id: tile
                anchors.top: parent.top
                anchors.topMargin: 10
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - 28
                height: width * 1.5
                cover: cell.cover
                title: cell.title
                authors: cell.authors
                percent: cell.percent
                status: cell.status
                held: cell.held
                current: grid.currentIndex === cell.index
            }
            Label {
                anchors.top: tile.bottom
                anchors.topMargin: 8
                anchors.left: tile.left
                anchors.right: tile.right
                text: cell.title
                elide: Text.ElideRight
                font.pixelSize: 13
            }
            Label {
                anchors.top: tile.bottom
                anchors.topMargin: 26
                anchors.left: tile.left
                anchors.right: rated.visible ? rated.left : tile.right
                anchors.rightMargin: rated.visible ? 6 : 0
                text: cell.authors
                elide: Text.ElideRight
                color: theme.colors.muted
                font.pixelSize: 12
            }
            Stars {
                id: rated
                anchors.right: tile.right
                anchors.top: tile.bottom
                anchors.topMargin: 29
                rating: cell.rating
                size: 10
                color: theme.colors.muted
            }
            BookTaps {
                onPicked: {
                    grid.currentIndex = cell.index
                    grid.forceActiveFocus()
                }
                onOpened: shelf.opened(cell.bookId)
                onHeld: shelf.detailsWanted()
            }
        }
    }
}
