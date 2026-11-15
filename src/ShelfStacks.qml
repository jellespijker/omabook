import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The library in stacks: a pile per series, author, tag or reading status,
// which opens into its books. Opening a pile is filtering the shelf by it,
// so the open pile shows among the filters and closes when that is removed.
// See ShelfCovers.qml for what every view of the library answers.
FocusScope {
    id: shelf
    readonly property string kind: backend.group
    readonly property bool inPile: backend.books.filters[kind] !== undefined
    readonly property int currentRow: inPile ? covers.currentRow : -1
    readonly property bool ownDetails: false
    signal opened(var bookId)
    signal detailsWanted()
    // The pile that is selected, by what it is a pile of: the piles are made
    // anew whenever the books change, and the selection is kept through it.
    property string lastPile
    property bool settling: false
    readonly property var piles: {
        backend.revision
        backend.books.count
        backend.books.query
        backend.books.filters
        backend.books.sort
        backend.books.reversed
        return inPile ? [] : backend.books.groups(kind)
    }

    function name(value) { return words.value(kind, value) }
    function show(row) { if (inPile) covers.show(row) }
    function move(where) {
        if (inPile) covers.move(where)
        else if (where === "left") stacks.moveCurrentIndexLeft()
        else if (where === "right") stacks.moveCurrentIndexRight()
        else if (where === "up") stacks.moveCurrentIndexUp()
        else stacks.moveCurrentIndexDown()
    }
    function activate() {
        if (inPile) covers.activate()
        else if (stacks.currentIndex >= 0 && stacks.currentIndex < piles.length) openPile(piles[stacks.currentIndex])
    }
    // A plugin's launcher is a pile of its own, which opens itself.
    function openPile(pile) {
        if (pile.launcher !== undefined) opened(pile.launcher)
        else open(pile.value)
    }
    function open(value) {
        lastPile = value
        backend.books.setFilter(kind, value)
    }
    function back() {
        if (!inPile)
            return false
        lastPile = backend.books.filters[kind]
        backend.books.setFilter(kind, "")
        return true
    }
    function settle() {
        settling = true
        stacks.model = piles
        stacks.currentIndex = Math.max(0, piles.findIndex(pile => pile.value === lastPile))
        settling = false
    }
    onPilesChanged: settle()
    Component.onCompleted: settle()
    onInPileChanged: {
        if (activeFocus)
            (inPile ? covers : stacks).forceActiveFocus()
    }

    Words { id: words }

    RowLayout {
        id: heading
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 22
        anchors.rightMargin: 22
        height: win.touch ? 44 : 32
        spacing: 8

        // In a pile: the way back, and which pile this is.
        Glyph {
            visible: shelf.inPile
            name: win.rightToLeft ? "forward" : "back"
            size: 16
            color: theme.colors.muted
        }
        Label {
            objectName: "stacksHeading"
            text: shelf.inPile ? shelf.name(backend.books.filters[shelf.kind]) : words.group(shelf.kind)
            font.pixelSize: shelf.inPile ? 16 : 13
            font.bold: shelf.inPile
            color: shelf.inPile ? theme.foreground : theme.colors.muted
            elide: Text.ElideRight
            Layout.maximumWidth: heading.width - 60
        }
        Item { Layout.fillWidth: true }
        TapHandler {
            onTapped: {
                if (!shelf.back()) {
                    const kinds = ["series", "author", "tag", "status"]
                    backend.group = kinds[(kinds.indexOf(shelf.kind) + 1) % kinds.length]
                }
            }
        }
    }

    GridView {
        id: stacks
        objectName: "stacks"
        visible: !shelf.inPile
        anchors.top: heading.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        clip: true
        focus: !shelf.inPile
        onCurrentIndexChanged: {
            if (!shelf.settling && currentIndex >= 0 && currentIndex < shelf.piles.length)
                shelf.lastPile = shelf.piles[currentIndex].value
        }
        readonly property int columns: Math.max(1, Math.floor(width / (settings.coverSize + 24)))
        cellWidth: width / columns
        cellHeight: (cellWidth - 52) * 1.5 + 84
        keyNavigationEnabled: true
        highlight: Item {}
        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: pile
            required property int index
            required property var modelData
            readonly property bool current: GridView.isCurrentItem
            width: stacks.cellWidth
            height: stacks.cellHeight

            // Up to three of its books, the first in front and the others
            // showing behind it, as a pile seen a little from above.
            Item {
                id: art
                anchors.top: parent.top
                anchors.topMargin: 30
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.horizontalCenterOffset: -8
                width: parent.width - 52
                height: width * 1.5

                Repeater {
                    model: pile.modelData.books.length
                    delegate: BookCover {
                        id: member
                        required property int index
                        // Drawn from the back of the pile forward.
                        readonly property int depth: pile.modelData.books.length - 1 - index
                        readonly property var book: pile.modelData.books[depth]
                        x: depth * 11
                        y: -depth * 10
                        width: art.width
                        height: art.height
                        cover: book.cover
                        title: book.title
                        authors: book.authors
                        held: book.held || ""
                        current: depth === 0 && pile.current
                        Rectangle {
                            visible: member.depth > 0
                            anchors.fill: parent
                            radius: Math.min(4, theme.radius)
                            color: Qt.alpha(theme.background, 0.22 * member.depth)
                        }
                    }
                }
            }
            Label {
                anchors.top: art.bottom
                anchors.topMargin: 10
                anchors.left: art.left
                anchors.right: art.right
                text: pile.modelData.launcher !== undefined ? pile.modelData.title : shelf.name(pile.modelData.value)
                elide: Text.ElideRight
                font.pixelSize: 13
                font.bold: true
            }
            Label {
                anchors.top: art.bottom
                anchors.topMargin: 29
                anchors.left: art.left
                anchors.right: art.right
                text: pile.modelData.launcher !== undefined ? pile.modelData.authors : qsTr("%Ln books", "", pile.modelData.count)
                elide: Text.ElideRight
                color: theme.colors.muted
                font.pixelSize: 12
            }
            BookTaps {
                onPicked: {
                    stacks.currentIndex = pile.index
                    stacks.forceActiveFocus()
                }
                onOpened: shelf.openPile(pile.modelData)
            }
        }
    }

    ShelfCovers {
        id: covers
        visible: shelf.inPile
        anchors.top: heading.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        focus: shelf.inPile
        onOpened: (bookId) => shelf.opened(bookId)
        onDetailsWanted: shelf.detailsWanted()
    }
}
