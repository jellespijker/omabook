import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// The chronology (#44): when the events of the books happen in their
// stories, not the order they are read in. One lane for each timeline (a
// book's own, a series', a world's, real history) under the ruler of its
// calendar, lanes that line up on one axis together; an event a mark at
// its time, a span a bar; what has no time or order listed apart. All of
// it only as far as the reader has read.
//
// A tap on an event shows its card, which goes back to the page it was
// revealed on, or shows it in the graph. The filters narrow it by
// timeline, book, series or collection, by who or what an event involves,
// to what was met in a book, and to a stretch of time.
FocusScope {
    id: pane
    // Whether single keys are this view's shortcuts right now.
    property bool active: true
    readonly property alias browser: chronology
    readonly property bool popupOpen: chooser.opened || range.opened
    readonly property bool keysOn: active && visible
    readonly property var event: chronology.details
    readonly property bool hasEvent: chronology.selected !== 0
    property bool showCard: true
    // The card beside the picture where there is room for both, under it
    // where there isn't, and in its place where the window is too low.
    readonly property bool cardBeside: width >= 760
    readonly property bool compact: width < 560
    // A place to go to once the book it is in has opened.
    property var pending: null
    signal done()

    function takeFocus() { canvas.forceActiveFocus() }
    function fitSoon() { Qt.callLater(canvas.fit) }
    function keyOf(command) { return (custom.keyText[command] || "").split("  ")[0] }
    // To a moment of a book: at once when it is the open one, else once it has opened.
    function go(place) {
        if (reader.open && reader.bookId === place.bookId) {
            reader.marks.goToMoment(place.section, place.fraction, place.anchor || "")
            return
        }
        pending = place
        reader.openBook(place.bookId)
    }
    function backToPage() {
        if (!hasEvent) return
        const page = event.page || {}
        if (!(page.bookId > 0)) backend.say(qsTr("No book says this; it was put in by hand."))
        else if (!page.readable) backend.say(qsTr("That book has no file to read here."))
        else {
            done()
            go(page)
        }
    }
    function showInGraph() {
        if (!hasEvent) return
        const node = event.graph.node
        const bookId = event.graph.bookId
        done()
        if (win.inReader && readerLoader.item) readerLoader.item.showThing(node)
        else win.showFound({ node: node, bookId: bookId })
    }
    function select(id) {
        chronology.select(id)
        showCard = true
    }

    ChronologyBrowser {
        id: chronology
        source: backend
    }
    GraphWords { id: words }
    Connections {
        target: reader
        function onBookChanged() {
            if (!pane.pending || !reader.open || reader.bookId !== pane.pending.bookId) return
            reader.marks.goToMoment(pane.pending.section, pane.pending.fraction, pane.pending.anchor || "")
            pane.pending = null
        }
        function onLoadingChanged() { if (reader.error !== "") pane.pending = null }
    }

    Key { command: "chronology.close"; when: pane.keysOn; onActivated: pane.done() }
    Key { command: "chronology.left"; mirrored: win.rightToLeft; when: pane.keysOn; onActivated: chronology.step("left") }
    Key { command: "chronology.right"; mirrored: win.rightToLeft; when: pane.keysOn; onActivated: chronology.step("right") }
    Key { command: "chronology.up"; when: pane.keysOn; onActivated: chronology.step("up") }
    Key { command: "chronology.down"; when: pane.keysOn; onActivated: chronology.step("down") }
    Key { command: "chronology.zoom_in"; when: pane.keysOn; onActivated: canvas.zoomBy(1.4) }
    Key { command: "chronology.zoom_out"; when: pane.keysOn; onActivated: canvas.zoomBy(1 / 1.4) }
    Key { command: "chronology.fit"; when: pane.keysOn; onActivated: canvas.fit() }
    Key { command: "chronology.pan_left"; mirrored: win.rightToLeft; when: pane.keysOn; onActivated: canvas.panBy(canvas.width / 5, 0) }
    Key { command: "chronology.pan_right"; mirrored: win.rightToLeft; when: pane.keysOn; onActivated: canvas.panBy(-canvas.width / 5, 0) }
    Key { command: "chronology.pan_up"; when: pane.keysOn; onActivated: canvas.panBy(0, canvas.height / 5) }
    Key { command: "chronology.pan_down"; when: pane.keysOn; onActivated: canvas.panBy(0, -canvas.height / 5) }
    Key { command: "chronology.find"; when: pane.keysOn; onActivated: chooser.show("find") }
    Key { command: "chronology.filter"; when: pane.keysOn; onActivated: chooser.show("filter") }
    Key { command: "chronology.range"; when: pane.keysOn; onActivated: range.show() }
    Key { command: "chronology.filter_clear"; when: pane.keysOn; onActivated: chronology.clearFilters() }
    Key { command: "chronology.unplaced"; when: pane.keysOn; onActivated: chronology.showUnplaced = !chronology.showUnplaced }
    Key { command: "chronology.details"; when: pane.keysOn; onActivated: pane.showCard = !pane.showCard }
    Key { command: "chronology.page"; when: pane.keysOn && pane.hasEvent; onActivated: pane.backToPage() }
    Key { command: "chronology.graph"; when: pane.keysOn && pane.hasEvent; onActivated: pane.showInGraph() }

    ChronologyPalette {
        id: chooser
        browser: chronology
        onPicked: (id) => pane.select(id)
        onClosed: pane.takeFocus()
    }
    ChronologyRange {
        id: range
        browser: chronology
        onClosed: pane.takeFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: theme.background
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ChronologyHeader {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.margins: 16
            Layout.bottomMargin: 8
            browser: chronology
            compact: pane.compact
            onClose: pane.done()
            onFilter: chooser.show("filter")
            onMore: {
                pane.takeFocus()
                win.showMenu("chronology", undefined, canvas)
            }
        }

        // What the keys do, as buttons, where fingers are the keyboard.
        Flow {
            objectName: "chronologyTouchBar"
            visible: win.touch
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            spacing: 8
            AppButton { runs: "chronology.find"; text: qsTr("Find"); focusPolicy: Qt.NoFocus; onClicked: chooser.show("find") }
            AppButton { runs: "chronology.range"; text: qsTr("Time"); focusPolicy: Qt.NoFocus; onClicked: range.show() }
            AppButton { runs: "chronology.fit"; text: qsTr("Fit"); focusPolicy: Qt.NoFocus; onClicked: canvas.fit() }
        }

        // The filters that are on, each taken off by a tap.
        Flow {
            objectName: "chronologyFilters"
            visible: chronology.filters.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            spacing: 6
            Repeater {
                model: chronology.filters
                delegate: ChronologyChip {
                    required property var modelData
                    text: modelData.label
                    room: pane.width - 32
                    onTapped: chronology.setFilter(modelData.act, modelData.value, false)
                }
            }
            ChronologyChip {
                runs: "chronology.filter_clear"
                text: qsTr("Show everything again")
                room: pane.width - 32
                removes: false
                onTapped: chronology.clearFilters()
            }
        }

        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            readonly property bool carded: pane.showCard && pane.hasEvent
            // Too low for the picture over the card: one at a time.
            readonly property bool either: !pane.cardBeside && height < 380
            readonly property real sideWidth: carded && pane.cardBeside ? Math.min(360, Math.round(width * 0.36)) : 0
            readonly property real underHeight: !carded || pane.cardBeside ? 0 : either ? height : Math.min(Math.round(height * 0.5), 300)
            readonly property real listHeight: unplacedList.visible ? unplacedList.height : 0

            ChronologyCanvas {
                id: canvas
                objectName: "chronologyCanvas"
                // A tap selects what it lands on and a drag moves the view,
                // which is what the arrow keys and their Shift do; the
                // wheel and a pinch zoom.
                readonly property string runs: "chronology.left chronology.right chronology.up chronology.down chronology.pan_left "
                                               + "chronology.pan_right chronology.pan_up chronology.pan_down chronology.zoom_in chronology.zoom_out"
                anchors.fill: parent
                anchors.rightMargin: body.sideWidth
                anchors.bottomMargin: body.underHeight + body.listHeight
                visible: !(body.carded && body.either)
                // Lanes scrolled past its edges are not drawn over what is around it.
                clip: true
                focus: true
                browser: chronology
                colors: theme.colors
                foreground: theme.foreground
                background: theme.background
                accent: theme.accent
                mirrored: win.rightToLeft
                hereText: qsTr("You are here")
                Accessible.role: Accessible.Graphic
                Accessible.name: qsTr("The chronology as a picture")

                HoverHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    cursorShape: canvas.hovered !== 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onPointChanged: canvas.hovered = canvas.eventAt(point.position.x, point.position.y)
                    onHoveredChanged: if (!hovered) canvas.hovered = 0
                }
                TapHandler {
                    onTapped: (point) => {
                        canvas.forceActiveFocus()
                        pane.select(canvas.eventAt(point.position.x, point.position.y, win.touch ? 16 : 4))
                    }
                    onDoubleTapped: (point) => {
                        if (canvas.eventAt(point.position.x, point.position.y, win.touch ? 16 : 4) !== 0) pane.backToPage()
                    }
                }
                DragHandler {
                    target: null
                    property point last
                    onActiveChanged: if (active) last = centroid.position
                    onCentroidChanged: {
                        if (!active) return
                        canvas.panBy(centroid.position.x - last.x, centroid.position.y - last.y)
                        last = centroid.position
                    }
                }
                WheelHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (event) => {
                        if (event.angleDelta.y !== 0) canvas.zoomBy(Math.pow(1.0018, event.angleDelta.y), event.x)
                        if (event.angleDelta.x !== 0) canvas.panBy(event.angleDelta.x / 2, 0)
                    }
                }
                PinchHandler {
                    target: null
                    onScaleChanged: (delta) => canvas.zoomBy(delta, centroid.position.x)
                }
            }

            ChronologyEmpty {
                anchors.fill: canvas
                visible: canvas.visible && chronology.shownCount === 0
                browser: chronology
            }

            // What has no time or order is listed apart, under the lanes.
            ChronologyUnplaced {
                id: unplacedList
                objectName: "chronologyUnplaced"
                visible: chronology.unplaced.length > 0 && !(body.carded && body.either)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.rightMargin: body.sideWidth
                anchors.bottom: parent.bottom
                anchors.bottomMargin: body.underHeight
                browser: chronology
                onPicked: (id) => pane.select(id)
            }

            ChronologyCard {
                id: card
                objectName: "chronologyCard"
                visible: body.carded
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: pane.cardBeside ? body.sideWidth : parent.width
                height: pane.cardBeside ? parent.height : body.underHeight
                beside: pane.cardBeside
                event: pane.event
                onPage: pane.backToPage()
                onGraph: pane.showInGraph()
                onClose: {
                    pane.showCard = false
                    pane.takeFocus()
                }
                onInvolve: (id) => chronology.setFilter("involved", id, true)
            }
        }
    }
}
