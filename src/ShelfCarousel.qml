import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The library as a carousel: the selected book's cover large in the middle,
// its neighbors receding to either side, and what it is underneath.
// See ShelfCovers.qml for what every view of the library answers.
FocusScope {
    id: shelf
    property alias currentRow: flow.currentIndex
    readonly property bool ownDetails: true
    signal opened(var bookId)
    readonly property var book: {
        backend.revision
        flow.count
        return backend.books.get(flow.currentIndex)
    }

    function show(row) {
        flow.currentIndex = row
        // A jump is a jump: sliding past hundreds of books would only blur.
        flow.positionViewAtIndex(row, ListView.Center)
    }
    function move(where) {
        if (where !== "left" && where !== "right")
            return
        if ((where === "right") !== win.rightToLeft) flow.incrementCurrentIndex()
        else flow.decrementCurrentIndex()
    }
    function activate() { if (flow.currentIndex >= 0 && flow.count > 0) shelf.opened(backend.books.idAt(flow.currentIndex)) }
    function back() { return false }

    Words { id: words }

    ListView {
        id: flow
        objectName: "carousel"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: about.top
        orientation: ListView.Horizontal
        model: backend.books
        focus: true
        clip: true
        readonly property real coverHeight: Math.round(Math.max(120, Math.min(height - 56, 480)))
        readonly property real coverWidth: Math.round(coverHeight / 1.5)
        // Books stand closer than they are wide, so the row overlaps.
        readonly property real step: Math.round(coverWidth * 0.5)
        // The selected book is kept in the middle, the first and the last
        // too, which is what the empty ends are for. A swipe slides the row
        // freely and selects whichever book it leaves in the middle.
        highlightRangeMode: ListView.ApplyRange
        preferredHighlightBegin: (width - step) / 2
        preferredHighlightEnd: (width + step) / 2
        highlightMoveDuration: 280
        highlightMoveVelocity: -1
        header: Item { width: (flow.width - flow.step) / 2; height: 1 }
        footer: Item { width: (flow.width - flow.step) / 2; height: 1 }
        boundsBehavior: Flickable.StopAtBounds
        // The empty ends change with the view's size, which moves the row
        // under the selected book; it is put back in the middle.
        function settle() { if (currentIndex >= 0 && !moving) positionViewAtIndex(currentIndex, ListView.Center) }
        // A timer of the view's own: a deferred call would outlive the view.
        Timer { id: settleLater; interval: 0; onTriggered: flow.settle() }
        onWidthChanged: settleLater.restart()
        onStepChanged: settleLater.restart()
        onMovementEnded: {
            const middle = indexAt(contentX + width / 2, height / 2)
            if (middle >= 0)
                currentIndex = middle
        }
        cacheBuffer: Math.max(0, step * 4)
        keyNavigationEnabled: true

        delegate: Item {
            id: slot
            required property int index
            required property var bookId
            required property string title
            required property string authors
            required property url cover
            required property real percent
            required property string status
            required property string held
            // How many places this book is from the middle, as the row slides.
            readonly property real away: (x + width / 2 - flow.contentX - flow.width / 2) / flow.step
            readonly property real near: Math.min(1, Math.abs(away))
            readonly property real side: away < 0 ? -1 : 1
            width: flow.step
            height: flow.height
            z: 1000 - Math.round(Math.abs(away) * 10)

            BookCover {
                anchors.centerIn: parent
                // The middle book gets room; the rest stand turned beside it.
                anchors.horizontalCenterOffset: slot.side * slot.near * flow.coverWidth * 0.42
                width: flow.coverWidth
                height: flow.coverHeight
                scale: 1 - 0.26 * slot.near - 0.03 * Math.min(3, Math.max(0, Math.abs(slot.away) - 1))
                cover: slot.cover
                title: slot.title
                authors: slot.authors
                percent: slot.percent
                status: slot.status
                held: slot.held
                current: slot.ListView.isCurrentItem
                found: {
                    backend.books.knownRevision
                    return backend.books.knownFor(slot.bookId)
                }
                transform: Rotation {
                    origin.x: flow.coverWidth / 2
                    origin.y: flow.coverHeight / 2
                    axis { x: 0; y: 1; z: 0 }
                    angle: -slot.side * slot.near * 36
                }
                // A tap brings a book to the middle; the one in the middle opens.
                BookTaps {
                    tapOpens: slot.ListView.isCurrentItem
                    onPicked: {
                        flow.currentIndex = slot.index
                        flow.forceActiveFocus()
                    }
                    onOpened: shelf.opened(slot.bookId)
                }
            }
        }
    }

    ColumnLayout {
        id: about
        objectName: "carouselAbout"
        visible: flow.count > 0
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 48, 620)
        height: 112
        spacing: 4

        Label {
            objectName: "carouselTitle"
            Layout.fillWidth: true
            text: shelf.book.title || ""
            font.pixelSize: 22
            font.bold: true
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
        }
        Label {
            Layout.fillWidth: true
            text: shelf.book.authors || ""
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
        }
        Label {
            Layout.fillWidth: true
            visible: text !== ""
            // "Dune, book 2"
            text: !shelf.book.series ? ""
                  : shelf.book.seriesIndex > 0 ? qsTr("%1, book %2").arg(shelf.book.series).arg(Number(shelf.book.seriesIndex).toLocaleString(Qt.locale(), 'f', Number.isInteger(shelf.book.seriesIndex) ? 0 : 1))
                  : shelf.book.series
            color: theme.accent
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
        }
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 2
            spacing: 10
            Stars {
                rating: shelf.book.rating || 0
                size: 13
                showEmpty: true
            }
            Label {
                objectName: "carouselFacts"
                // "Reading · 42% read · Paper · EPUB"
                text: {
                    translator.language
                    const book = shelf.book
                    return [book.status ? words.status(book.status) : "",
                            book.percent > 0 && book.status !== "read"
                                ? qsTr("%1% read").arg(Math.round(book.percent).toLocaleString(Qt.locale(), 'f', 0)) : "",
                            book.held === "digital" ? "" : words.held(book.held || "digital"),
                            book.formatNames || ""].filter(part => part !== "").join(" · ")
                }
                color: theme.colors.muted
                elide: Text.ElideRight
            }
        }
        Item { Layout.fillHeight: true }
    }
}
