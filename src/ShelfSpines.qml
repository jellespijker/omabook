import QtQuick
import QtQuick.Controls

// The library as a bookshelf: books standing spine out, each as thick as it
// is long and bound in its cover's color, with a few lying flat in piles in
// between. The selected book is pulled out and shows its cover.
// See ShelfCovers.qml for what every view of the library answers.
FocusScope {
    id: shelf
    property int currentRow: -1
    readonly property bool ownDetails: false
    signal opened(var bookId)

    // A standing book is as tall as a cover in the grid is.
    readonly property real bookHeight: Math.round(settings.coverSize * 1.25)
    readonly property real margin: 32
    // What stands on each shelf, from the model, and where each row ended
    // up: [shelf, place on it].
    property var shelves: []
    property var places: []

    function arrange() {
        const room = rows.width - 2 * margin
        if (room <= 0)
            return
        const laid = backend.books.shelve(room, bookHeight)
        const where = []
        for (let s = 0; s < laid.length; ++s)
            for (let i = 0; i < laid[s].length; ++i)
                where[laid[s][i].row] = [s, i]
        places = where
        shelves = laid
        show(Math.max(0, Math.min(currentRow, backend.books.count - 1)))
    }
    function show(row) {
        currentRow = backend.books.count > 0 ? row : -1
        if (places[row] !== undefined)
            rows.positionViewAtIndex(places[row][0], ListView.Contain)
    }
    function move(where) {
        const at = places[currentRow]
        if (at === undefined)
            return
        if (where === "left" || where === "right") {
            // Along the shelf is on through the books, as the eye reads them.
            const on = (where === "right") !== win.rightToLeft
            show(Math.max(0, Math.min(currentRow + (on ? 1 : -1), backend.books.count - 1)))
            return
        }
        const here = shelves[at[0]][at[1]]
        // Up and down a pile first: the book above was laid on this one.
        const beside = shelves[at[0]][at[1] + (where === "up" ? -1 : 1)]
        if (here.flat && beside !== undefined && beside.flat && (where === "up" ? beside.y > here.y : beside.y < here.y)) {
            show(beside.row)
            return
        }
        // Then to the shelf above or below, to the book nearest overhead.
        const other = shelves[at[0] + (where === "up" ? -1 : 1)]
        if (other === undefined)
            return
        let best = other[0]
        for (const slot of other)
            if (Math.abs(slot.x + slot.w / 2 - here.x - here.w / 2) < Math.abs(best.x + best.w / 2 - here.x - here.w / 2))
                best = slot
        show(best.row)
    }
    function activate() { if (currentRow >= 0) shelf.opened(backend.books.idAt(currentRow)) }
    function back() { return false }

    Component.onCompleted: arrange()
    // A timer of the view's own rather than a deferred call, which outlives
    // the view when another one takes its place and then runs on nothing.
    Timer { id: later; interval: 0; onTriggered: shelf.arrange() }
    onBookHeightChanged: later.restart()
    Connections {
        target: backend.books
        function onModelReset() { shelf.arrange() }
    }

    ListView {
        id: rows
        objectName: "spines"
        anchors.fill: parent
        clip: true
        model: shelf.shelves.length
        keyNavigationEnabled: false
        ScrollBar.vertical: ScrollBar {}
        onWidthChanged: later.restart()

        delegate: Item {
            id: board
            required property int index
            readonly property var slots: shelf.shelves[index] || []
            // Where the books stand: the top of the plank.
            readonly property real floor: height - 22
            width: rows.width
            height: shelf.bookHeight + 52
            // The shelf with the pulled-out book draws over its neighbors.
            z: (shelf.places[shelf.currentRow] || [-1])[0] === index ? 1 : 0

            Rectangle {
                x: shelf.margin - 14
                y: board.floor
                width: board.width - 2 * (shelf.margin - 14)
                height: 8
                radius: Math.min(2, theme.radius)
                color: theme.colors.selection
            }

            Repeater {
                model: board.slots
                delegate: Item {
                    id: spine
                    required property var modelData
                    readonly property var book: backend.books.get(modelData.row)
                    readonly property bool selected: shelf.currentRow === modelData.row
                    readonly property bool flat: modelData.flat
                    readonly property bool wish: book.held === "none"
                    readonly property bool paper: book.held === "paper" || book.held === "both"
                    readonly property bool marked: book.status === "read" || book.status === "to-read"
                    // The cover's main color; a book without a cover is bound in
                    // one of the theme's, by its number, so it keeps it.
                    readonly property color cloth: {
                        if (book.spineColor.valid)
                            return book.spineColor
                        const hues = [theme.colors.red, theme.colors.green, theme.colors.blue, theme.colors.yellow,
                                      theme.colors.magenta, theme.colors.cyan, theme.colors.orange]
                        return Qt.tint(theme.colors.selection, Qt.alpha(hues[book.bookId % hues.length], 0.4))
                    }
                    readonly property color ink: wish ? theme.foreground : backend.books.inkOn(cloth)
                    // The spine's length and breadth, whichever way it lies.
                    readonly property real along: flat ? width : height
                    readonly property real across: flat ? height : width

                    // Places come from the shelf's start, which is the right in a
                    // right-to-left language; what is on a spine is not mirrored.
                    LayoutMirroring.enabled: false
                    LayoutMirroring.childrenInherit: true
                    x: win.rightToLeft ? board.width - shelf.margin - modelData.x - modelData.w : shelf.margin + modelData.x
                    y: board.floor - modelData.y - modelData.h
                    width: modelData.w
                    height: modelData.h
                    z: selected ? 2 : 0

                    Rectangle {
                        anchors.fill: parent
                        radius: Math.min(2, theme.radius)
                        color: spine.wish ? theme.background : spine.cloth
                        border.width: 1
                        border.color: spine.wish ? theme.colors.muted : Qt.darker(spine.cloth, 1.3)
                        // A spine is rounded: lighter where it bulges, darker at the hinge.
                        gradient: spine.wish ? null : sheen
                        Gradient {
                            id: sheen
                            orientation: spine.flat ? Gradient.Vertical : Gradient.Horizontal
                            GradientStop { position: 0; color: Qt.lighter(spine.cloth, 1.12) }
                            GradientStop { position: 0.35; color: spine.cloth }
                            GradientStop { position: 1; color: Qt.darker(spine.cloth, 1.22) }
                        }
                    }

                    // The title, and the author at the foot, set along the spine.
                    Item {
                        width: spine.along - 16 - (spine.marked ? 14 : 0) - (spine.paper ? 14 : 0)
                        height: spine.across
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: spine.flat ? 0 : ((spine.marked ? 7 : 0) - (spine.paper ? 7 : 0))
                        anchors.horizontalCenterOffset: spine.flat ? ((spine.marked ? 7 : 0) - (spine.paper ? 7 : 0)) : 0
                        rotation: spine.flat ? 0 : 90

                        Label {
                            anchors.left: parent.left
                            anchors.right: by.visible ? by.left : parent.right
                            anchors.rightMargin: by.visible ? 10 : 0
                            anchors.verticalCenter: parent.verticalCenter
                            text: spine.book.title
                            color: spine.ink
                            elide: Text.ElideRight
                            font.pixelSize: Math.round(Math.max(9, Math.min(13, spine.across * 0.42)))
                            font.bold: true
                        }
                        Label {
                            id: by
                            visible: text !== "" && parent.width > 150
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, parent.width * 0.36)
                            text: spine.book.authors
                            color: spine.ink
                            opacity: 0.78
                            elide: Text.ElideRight
                            font.pixelSize: Math.round(Math.max(8, Math.min(11, spine.across * 0.36)))
                        }
                    }

                    // Finished is a check and meant-to-read a ribbon, at the head.
                    Glyph {
                        visible: spine.marked
                        x: spine.flat ? 5 : (spine.width - size) / 2
                        y: spine.flat ? (spine.height - size) / 2 : 6
                        size: Math.min(12, spine.across - 4)
                        name: spine.book.status === "read" ? "check" : "ribbon"
                        color: spine.ink
                    }
                    // A paper book carries a label at the foot, as a library's does.
                    Rectangle {
                        objectName: "paperLabel"
                        visible: spine.paper
                        x: spine.flat ? spine.width - 16 : 3
                        y: spine.flat ? 3 : spine.height - 16
                        width: spine.flat ? 9 : spine.width - 6
                        height: spine.flat ? spine.height - 6 : 9
                        radius: 1
                        color: spine.ink
                        opacity: 0.85
                    }
                    // How far it has been read, along the foot.
                    Rectangle {
                        visible: spine.book.percent > 0 && spine.book.status !== "read"
                        x: 1
                        y: spine.height - 4
                        height: 3
                        width: (spine.width - 2) * Math.min(1, spine.book.percent / 100)
                        color: theme.accent
                    }

                    BookTaps {
                        onPicked: {
                            shelf.currentRow = spine.modelData.row
                            shelf.forceActiveFocus()
                        }
                        onOpened: shelf.opened(spine.book.bookId)
                    }

                    // Pulled out, the book shows its face, standing on the shelf
                    // over its neighbors.
                    Loader {
                        active: spine.selected
                        readonly property real faceWidth: Math.round(shelf.bookHeight / 1.5)
                        x: Math.max(shelf.margin - 8, Math.min(spine.x + (spine.width - faceWidth) / 2,
                                                              board.width - shelf.margin + 8 - faceWidth)) - spine.x
                        y: board.floor - shelf.bookHeight - spine.y
                        sourceComponent: BookCover {
                            id: face
                            objectName: "pulledOut"
                            width: Math.round(shelf.bookHeight / 1.5)
                            height: shelf.bookHeight
                            cover: spine.book.cover
                            title: spine.book.title
                            authors: spine.book.authors
                            percent: spine.book.percent
                            status: spine.book.status
                            held: spine.book.held
                            current: true
                            found: {
                                backend.books.knownRevision
                                return backend.books.knownFor(spine.book.bookId)
                            }
                            transformOrigin: Item.Bottom
                            // It comes off the shelf: up from its spine, quickly, then settles.
                            ParallelAnimation {
                                running: true
                                NumberAnimation { target: face; property: "scale"; from: 0.9; to: 1; duration: 180; easing.type: Easing.OutExpo }
                                NumberAnimation { target: face; property: "opacity"; from: 0; to: 1; duration: 120; easing.type: Easing.OutQuad }
                            }
                            BookTaps {
                                onOpened: shelf.opened(spine.book.bookId)
                            }
                        }
                    }
                }
            }
        }
    }
}
