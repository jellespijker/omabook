import QtQuick
import QtQuick.Controls

// A book of fixed pages (a PDF, a comic): the pages one under another, each
// drawn off the GUI thread at the size it is shown.
ListView {
    id: view
    objectName: "pageReader"
    // Pages fit the window's width up to this many pixels, times the zoom.
    property real zoomFactor: 1
    // A wide window shows two pages side by side, like the book lying open.
    readonly property bool twoUp: width >= win.spreadFloor && width > height * 1.15 && reader.count > 1
    readonly property int across: twoUp ? 2 : 1
    readonly property int pageCount: reader.count
    readonly property real pageWidth: Math.max(120, Math.min((width - 32 - (across - 1) * spacing) / across, 920) * zoomFactor)
    readonly property string selectedText: ""

    // A row of the view is one page, or a pair of them.
    model: Math.ceil(reader.count / across)
    spacing: 12
    clip: true
    focus: true
    boundsBehavior: Flickable.StopAtBounds
    cacheBuffer: Math.max(0, height)
    contentWidth: Math.max(width, pageWidth * across + (across - 1) * spacing + 32)
    flickableDirection: Flickable.AutoFlickIfNeeded
    ScrollBar.vertical: ScrollBar {}
    ScrollBar.horizontal: ScrollBar {}

    function scrollBy(distance) {
        contentY = Math.max(originY, Math.min(originY + contentHeight - height, contentY + distance))
    }
    // Side by side, a turn is a whole pair; one under another, most of a window.
    function pageDown() { if (twoUp) next(); else scrollBy(height * 0.9) }
    function pageUp() { if (twoUp) previous(); else scrollBy(-height * 0.9) }
    function lineDown() { scrollBy(80) }
    function lineUp() { scrollBy(-80) }
    readonly property int row: Math.floor(reader.section / across)
    function next() { positionViewAtIndex(Math.min(count - 1, row + 1), ListView.Beginning) }
    function previous() { positionViewAtIndex(Math.max(0, row - 1), ListView.Beginning) }
    function toStart() { positionViewAtBeginning() }
    function toEnd() { positionViewAtEnd() }
    function zoom(steps) { zoomFactor = Math.max(0.4, Math.min(4, zoomFactor * (steps > 0 ? 1.15 : 1 / 1.15))) }
    function pinch(delta) { zoomFactor = Math.max(0.4, Math.min(4, zoomFactor * delta)) }
    function placeText() { return qsTr("Page %1").arg(reader.section + 1) }

    // The page under the upper part of the window is the one being read.
    onContentYChanged: {
        const at = indexAt(contentX + width / 2, contentY + height / 3)
        if (at >= 0)
            reader.section = Math.min(reader.count - 1, at * across)
    }
    Component.onCompleted: positionViewAtIndex(row, ListView.Beginning)
    // Turning the window on its side keeps the page being read.
    onAcrossChanged: Qt.callLater(() => positionViewAtIndex(Math.floor(reader.section / across), ListView.Beginning))
    Connections {
        target: reader
        function onMoved(anchor, fraction) { view.positionViewAtIndex(view.row, ListView.Beginning) }
    }

    delegate: Item {
        id: sheets
        required property int index
        width: view.contentWidth
        height: pair.height

        Row {
            id: pair
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: view.spacing
            layoutDirection: win.rightToLeft ? Qt.RightToLeft : Qt.LeftToRight

            Repeater {
                model: Math.min(view.across, reader.count - sheets.index * view.across)
                delegate: Rectangle {
                    id: sheet
                    required property int index
                    readonly property int page: sheets.index * view.across + index
                    // A page's shape is known at once for a PDF and once drawn for a comic.
                    readonly property real aspect: {
                        const known = reader.pageAspect(page)
                        if (known > 0) return known
                        return picture.status === Image.Ready && picture.implicitWidth > 0
                            ? picture.implicitHeight / picture.implicitWidth : 1.414
                    }
                    width: view.pageWidth
                    height: Math.round(view.pageWidth * aspect)
                    color: "white"

                    Image {
                        id: picture
                        anchors.fill: parent
                        asynchronous: true
                        // The width in the address is in device pixels, so pages are sharp on a HiDPI screen.
                        source: reader.imagePrefix + "page/" + sheet.page + "/" + Math.round(view.pageWidth * Screen.devicePixelRatio)
                        fillMode: Image.PreserveAspectFit
                    }
                }
            }
        }
    }
}
