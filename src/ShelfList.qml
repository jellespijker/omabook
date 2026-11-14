import QtQuick
import QtQuick.Controls

// The library as a list: a line a book, in columns that sort the shelf when
// their heading is clicked, or picked with the keys that go left and right.
// See ShelfCovers.qml for what every view of the library answers.
FocusScope {
    id: shelf
    property alias currentRow: table.currentIndex
    readonly property bool ownDetails: false
    signal opened(var bookId)
    signal detailsWanted()

    // Each column, the order its heading sorts by, its width (the title
    // takes what is left) and how much it matters: 1 is the last to go.
    readonly property var allColumns: [
        { key: "title", name: qsTr("Title"), sort: "title", width: 0, rank: 0 },
        { key: "authors", name: qsTr("Author"), sort: "author", width: 190, rank: 1 },
        { key: "series", name: qsTr("Series"), sort: "series", width: 170, rank: 4 },
        { key: "status", name: qsTr("Status"), sort: "progress", width: 110, rank: 2 },
        { key: "rating", name: qsTr("Rating"), sort: "rating", width: 96, rank: 3 },
        { key: "format", name: qsTr("Format"), sort: "", width: 120, rank: 6 },
        { key: "added", name: qsTr("Added"), sort: "added", width: 104, rank: 5 }
    ]
    // The least the title keeps. The list is as wide as the details pane
    // beside it leaves it, which no size class of the window knows, so the
    // rule is the list's own: a column shows while the title keeps this
    // much with it and every column that matters more, and they go in
    // order, least first: format, added, series, rating, status, author.
    readonly property real titleFloor: 180
    readonly property var columns: {
        let rest = shelf.width - 2 * gutter - titleFloor
        const kept = {}
        for (const column of allColumns.slice().sort((a, b) => a.rank - b.rank)) {
            if (column.rank > 0 && rest < column.width)
                break
            rest -= column.width
            kept[column.key] = true
        }
        return allColumns.filter(column => kept[column.key])
    }
    readonly property real gutter: 16
    readonly property real titleWidth: {
        let rest = shelf.width - 2 * gutter
        for (const column of columns)
            rest -= column.width
        return Math.max(80, rest)
    }
    // Secondary text reads on the selected line as well as off it.
    readonly property color quiet: Qt.alpha(theme.foreground, 0.7)

    function show(row) { table.currentIndex = row }
    function sortBy(key) {
        if (key === "")
            return
        if (backend.books.sort === key) {
            backend.books.reversed = !backend.books.reversed
        } else {
            backend.books.reversed = false
            backend.books.sort = key
        }
    }
    function move(where) {
        if (where === "up") table.decrementCurrentIndex()
        else if (where === "down") table.incrementCurrentIndex()
        else {
            // Left and right walk the order along the headings.
            const sortable = columns.filter(column => column.sort !== "")
            let at = sortable.findIndex(column => column.sort === backend.books.sort)
            const on = (where === "right") !== win.rightToLeft
            at = at < 0 ? 0 : (at + (on ? 1 : sortable.length - 1)) % sortable.length
            backend.books.reversed = false
            backend.books.sort = sortable[at].sort
        }
    }
    function activate() { if (table.currentIndex >= 0 && table.count > 0) shelf.opened(backend.books.idAt(table.currentIndex)) }
    function back() { return false }

    Words { id: words }

    Row {
        id: headings
        objectName: "listHeadings"
        x: shelf.gutter
        width: parent.width - 2 * shelf.gutter
        height: win.touch ? 44 : 32

        Repeater {
            model: shelf.columns
            delegate: Item {
                id: heading
                required property var modelData
                readonly property bool sorted: modelData.sort !== "" && backend.books.sort === modelData.sort
                objectName: "heading-" + modelData.key
                width: modelData.width || shelf.titleWidth
                height: headings.height

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Label {
                        // A heading longer than its column, in a language
                        // of long words, elides like what is under it.
                        width: Math.min(implicitWidth, heading.width - 16 - (heading.sorted ? 16 : 0))
                        elide: Text.ElideRight
                        text: heading.modelData.name
                        font.pixelSize: 12
                        font.bold: heading.sorted
                        color: heading.sorted ? theme.foreground : theme.colors.muted
                    }
                    Glyph {
                        visible: heading.sorted
                        anchors.verticalCenter: parent.verticalCenter
                        size: 12
                        name: backend.books.descending ? "down" : "up"
                    }
                }
                TapHandler { onTapped: shelf.sortBy(heading.modelData.sort) }
            }
        }
    }
    Rectangle {
        anchors.top: headings.bottom
        x: shelf.gutter
        width: parent.width - 2 * shelf.gutter
        height: 1
        color: theme.colors.selection
    }

    ListView {
        id: table
        objectName: "list"
        anchors.top: headings.bottom
        anchors.topMargin: 5
        anchors.bottom: parent.bottom
        width: parent.width
        clip: true
        focus: true
        model: backend.books
        keyNavigationEnabled: true
        highlightMoveDuration: 0
        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: line
            required property int index
            required property var bookId
            required property string title
            required property string authors
            required property string series
            required property real seriesIndex
            required property string status
            required property real percent
            required property int rating
            required property string formatNames
            required property string held
            required property var added
            readonly property bool current: ListView.isCurrentItem
            width: table.width
            height: win.touch ? 44 : 32

            function cell(key) {
                if (key === "title") return title
                if (key === "authors") return authors
                if (key === "series")
                    return series === "" || seriesIndex <= 0 ? series
                           : series + " · " + Number(seriesIndex).toLocaleString(Qt.locale(), 'f', Number.isInteger(seriesIndex) ? 0 : 1)
                if (key === "status")
                    return status === "reading" && percent > 0
                           ? qsTr("%1% read").arg(Math.round(percent).toLocaleString(Qt.locale(), 'f', 0))
                           : status === "" ? "" : words.status(status)
                if (key === "format")
                    return [formatNames, held === "digital" ? "" : words.held(held)].filter(part => part !== "").join(" · ")
                if (key === "added") return words.day(added)
                return ""
            }

            Rectangle {
                visible: line.current
                x: shelf.gutter
                width: parent.width - 2 * shelf.gutter
                height: parent.height
                radius: Math.min(4, theme.radius)
                color: theme.colors.selection
            }
            Row {
                x: shelf.gutter
                height: parent.height
                Repeater {
                    model: shelf.columns
                    delegate: Item {
                        id: cell
                        required property var modelData
                        width: modelData.width || shelf.titleWidth
                        height: line.height

                        Label {
                            visible: cell.modelData.key !== "rating"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: {
                                translator.language
                                return line.cell(cell.modelData.key)
                            }
                            elide: Text.ElideRight
                            // Said outright, so it is mirrored with its heading
                            // whatever the text's own direction.
                            horizontalAlignment: Text.AlignLeft
                            font.pixelSize: 13
                            font.bold: cell.modelData.key === "title" && line.current
                            // Figures line up down a column.
                            font.features: { "tnum": 1 }
                            color: cell.modelData.key === "title" ? theme.foreground : shelf.quiet
                        }
                        Stars {
                            visible: cell.modelData.key === "rating" && line.rating > 0
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            rating: line.rating
                            size: 11
                            color: shelf.quiet
                        }
                    }
                }
            }
            BookTaps {
                onPicked: {
                    table.currentIndex = line.index
                    table.forceActiveFocus()
                }
                onOpened: shelf.opened(line.bookId)
                onHeld: shelf.detailsWanted()
            }
        }
    }
}
