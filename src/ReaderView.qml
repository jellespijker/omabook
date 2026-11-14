import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The open book and nothing else: its text or pages, a line saying where you
// are, and the AI companion beside it when asked for.
Item {
    id: view
    objectName: "readerView"
    // Whether single keys are this view's shortcuts right now.
    property bool active: true
    property bool showAssistant: false
    // The on-screen bar of what the keys do; a tap mid-page shows or hides it.
    property bool showBar: false
    // The index beside the page, and the page with nothing around it at all.
    property bool showIndex: false
    property bool immersive: false
    // The map of what the book has told so far, beside the page.
    property bool showGraph: false
    readonly property bool popupOpen: finder.opened || knowledge.popupOpen
    // While the map has the keyboard, the keys are its own (see GraphPane).
    readonly property bool mapping: knowledge.focused
    // Whether the keys that move through the book are the book's right now.
    readonly property bool paging: win.inReader && !!content && !index.focused && !mapping
    readonly property var content: contentLoader.item

    // Turning a page by hand: the page follows the finger sideways, and past
    // a point it slides away while the next one slides in from the other
    // side. Forward is leftwards, as in a book, and the other way round in a
    // right-to-left language.
    property real slide: 0
    readonly property int forwardSign: win.rightToLeft ? 1 : -1
    readonly property bool turningPage: turning.running || settleBack.running
    function turn(forward) {
        if (!content || turning.running) return
        turning.forward = forward
        turning.restart()
    }
    SequentialAnimation {
        id: turning
        property bool forward: true
        readonly property real away: (forward ? view.forwardSign : -view.forwardSign) * contentLoader.width
        NumberAnimation { target: view; property: "slide"; to: turning.away; duration: 130; easing.type: Easing.InQuad }
        ScriptAction {
            script: {
                if (turning.forward) view.content.pageDown()
                else view.content.pageUp()
                view.slide = -turning.away
            }
        }
        NumberAnimation { target: view; property: "slide"; to: 0; duration: 170; easing.type: Easing.OutQuad }
    }
    NumberAnimation { id: settleBack; target: view; property: "slide"; to: 0; duration: 150; easing.type: Easing.OutQuad }

    function back() {
        if (showAssistant) showAssistant = false
        else if (showGraph) toggleGraph()
        else if (showIndex) showIndex = false
        else if (immersive) expand(false)
        else reader.close()
    }
    // In a window where panes lie over the page, the one that is asked for
    // is the one that shows: those that would lie over it are put away.
    function toggleIndex() {
        showIndex = !showIndex
        if (showIndex && !beside) {
            showAssistant = false
            showGraph = false
        }
        if (showIndex) index.takeFocus()
        else if (content) content.forceActiveFocus()
    }
    // The graph beside the page, on what a plugin asked for.
    function showGraphOf(scope, bookId) {
        if (!showGraph) toggleGraph()
        if (bookId > 0) knowledge.showBook(bookId)
        else knowledge.showScope(scope)
    }
    function toggleGraph() {
        showGraph = !showGraph
        if (showGraph && !beside)
            showAssistant = false
        if (showGraph) knowledge.takeFocus()
        else if (content) content.forceActiveFocus()
    }
    // The book and nothing else: the whole screen, no title line, no panes.
    function expand(yes) {
        immersive = yes
        if (yes) {
            showIndex = false
            showAssistant = false
            showGraph = false
        }
        win.visibility = yes ? Window.FullScreen : Window.Windowed
    }
    function catchUp() {
        showAssistant = true
        assistant.catchUp()
        companion.takeFocus()
    }
    function askAssistant() {
        showAssistant = true
        companion.takeFocus()
    }
    function bookmark() {
        if (!content) return
        reader.addBookmark(content.placeText())
        backend.say(qsTr("Bookmarked"))
    }

    Key { command: "reader.back"; when: win.inReader && !view.mapping; onActivated: view.back() }
    Key { command: "reader.page_down"; when: view.paging; onActivated: view.content.pageDown() }
    Key { command: "reader.page_up"; when: view.paging; onActivated: view.content.pageUp() }
    Key { command: "reader.line_down"; when: view.paging; onActivated: view.content.lineDown() }
    Key { command: "reader.line_up"; when: view.paging; onActivated: view.content.lineUp() }
    Key { command: "reader.next"; when: view.paging; onActivated: view.content.next() }
    Key { command: "reader.previous"; when: view.paging; onActivated: view.content.previous() }
    Key { command: "reader.start"; when: view.paging; onActivated: view.content.toStart() }
    Key { command: "reader.end"; when: view.paging; onActivated: view.content.toEnd() }
    Key { command: "reader.larger"; when: win.inReader && !!view.content && !view.mapping; onActivated: view.content.zoom(1) }
    Key { command: "reader.smaller"; when: win.inReader && !!view.content && !view.mapping; onActivated: view.content.zoom(-1) }
    Key { command: "reader.contents"; when: reader.open; onActivated: view.toggleIndex() }
    Key { command: "reader.catch_up"; when: reader.open; onActivated: view.catchUp() }
    Key { command: "reader.cast"; when: reader.open; onActivated: { view.showAssistant = true; assistant.whoIsWho(); companion.takeFocus() } }
    Key { command: "reader.find"; when: reader.open && !view.mapping; onActivated: finder.open() }
    Key { command: "reader.ask"; when: reader.open; onActivated: view.askAssistant() }
    Key { command: "reader.bookmark"; when: reader.open; onActivated: view.bookmark() }
    Key { command: "reader.expand"; when: win.inReader && !view.mapping; onActivated: view.expand(!view.immersive) }
    Key { command: "reader.graph"; when: reader.open; onActivated: view.toggleGraph() }

    // The panes: the contents on the side the text starts at, the map and
    // the AI on the side it ends at. They stand beside the page for as long
    // as the page keeps its floor with every open pane at its narrowest. In
    // a window with less room than that they lie over the page instead, one
    // at a time (the AI before the map before the contents, the order
    // Escape closes them in), so the page is never squeezed and two panes
    // are never side by side in a narrow window.
    readonly property bool indexOpen: showIndex && reader.open
    readonly property bool graphOpen: showGraph && reader.open
    readonly property real slotWidth: sideSlot.visible ? sideSlot.width : 0
    readonly property bool beside: width - slotWidth - (indexOpen ? indexSide.least : 0) - (showAssistant ? assistantSide.least : 0)
                                   - (graphOpen ? graphSide.least : 0) >= win.contentFloor
    readonly property string topPane: showAssistant ? "assistant" : graphOpen ? "graph" : indexOpen ? "contents" : ""
    // What a pane lying over the page may take: all but a strip of the
    // page, which is what is clicked to put the pane away.
    readonly property real overRoom: width - 48
    // Wider or narrower, by the keys: the pane that has the keyboard, or
    // the one on top.
    function resizePane(steps) {
        const pane = index.focused && indexSide.visible ? indexSide : knowledge.focused && graphSide.visible ? graphSide
                   : topPane === "assistant" ? assistantSide : topPane === "graph" ? graphSide : topPane === "contents" ? indexSide : null
        if (pane) pane.resize(steps)
    }

    Item {
        anchors.fill: parent

        Item {
            anchors.fill: parent
            anchors.leftMargin: view.beside && indexSide.visible ? indexSide.width : 0
            anchors.rightMargin: view.slotWidth + (view.beside ? (assistantSide.visible ? assistantSide.width : 0)
                                                                 + (graphSide.visible ? graphSide.width : 0) : 0)

            // Where you are: the book and chapter, and how far through.
            RowLayout {
                id: header
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: view.immersive ? 0 : 12
                height: view.immersive ? 0 : 24
                visible: !view.immersive
                spacing: 12
                opacity: 0.7

                Label {
                    objectName: "readerTitle"
                    Layout.fillWidth: true
                    text: reader.sectionTitle !== "" ? reader.title + " · " + reader.sectionTitle : reader.title
                    elide: Text.ElideRight
                    color: theme.colors.muted
                    font.pixelSize: 12
                }
                Label {
                    objectName: "readerPlace"
                    visible: reader.open
                    text: {
                        translator.language
                        return reader.kind === "pages"
                            ? qsTr("%1 of %2").arg((reader.section + 1).toLocaleString(Qt.locale(), 'f', 0)).arg(reader.count.toLocaleString(Qt.locale(), 'f', 0))
                            : qsTr("%1%").arg(Math.round(reader.percent).toLocaleString(Qt.locale(), 'f', 0))
                    }
                    color: theme.colors.muted
                    font.pixelSize: 12
                }
            }

            // Fingers: a tap on the page's right or left edge turns it, a tap
            // in the middle shows the bar, a sideways swipe changes chapter,
            // and a pinch makes the text or the page larger or smaller. The
            // page itself scrolls under a finger as it always does.
            Item {
                id: touchArea
                objectName: "touchArea"
                anchors.fill: contentLoader
                z: 1

                TapHandler {
                    acceptedDevices: PointerDevice.TouchScreen
                    onTapped: (point) => {
                        win.touchSeen = true
                        if (!view.content) return
                        const x = win.rightToLeft ? touchArea.width - point.position.x : point.position.x
                        if (x < touchArea.width * 0.25) view.turn(false)
                        else if (x > touchArea.width * 0.75) view.turn(true)
                        else view.showBar = !view.showBar
                    }
                }
                DragHandler {
                    id: swipe
                    acceptedDevices: PointerDevice.TouchScreen
                    target: null
                    yAxis.enabled: false
                    // A zoomed page pans sideways instead.
                    enabled: !!view.content && view.content.contentWidth <= view.content.width + 1
                    property real travelled: 0
                    // How far the finger is from where it came down. Measured
                    // from the finger itself, so a fast flick that arrives as
                    // one jump counts as much as a slow drag.
                    function track() {
                        if (turning.running) return
                        const moved = centroid.position.x - centroid.pressPosition.x
                        if (moved === 0) return
                        travelled = moved
                        view.slide = moved
                    }
                    onCentroidChanged: if (active) track()
                    onActiveChanged: {
                        if (active) {
                            win.touchSeen = true
                            track()
                            return
                        }
                        // A third of the way across, or a decent flick, turns the page.
                        const far = Math.abs(travelled) > Math.min(120, touchArea.width / 3)
                        if (far && view.content) view.turn(travelled * view.forwardSign > 0)
                        else settleBack.restart()
                        travelled = 0
                    }
                }
                PinchHandler {
                    acceptedDevices: PointerDevice.TouchScreen
                    target: null
                    onScaleChanged: (delta) => { if (view.content) view.content.pinch(delta) }
                }
            }

            Loader {
                id: contentLoader
                objectName: "contentLoader"
                anchors.top: header.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: progress.top
                anchors.topMargin: 4
                transform: Translate { x: view.slide }
                // A wide window is an open book: two pages. A tall one is a
                // single page that scrolls.
                readonly property bool wide: settings.spread === "always"
                                             || (settings.spread !== "never" && width >= win.spreadFloor && width > height * 1.15)
                sourceComponent: reader.kind === "flow" ? (wide ? spreadPages : flow) : reader.kind === "pages" ? pages : null
                Component { id: flow; FlowReader {} }
                Component { id: spreadPages; SpreadReader {} }
                Component { id: pages; PageReader {} }
            }

            // Opening, or the reason it can't be opened.
            ColumnLayout {
                objectName: "readerState"
                visible: !reader.open
                anchors.centerIn: parent
                width: Math.min(parent.width - 64, 520)
                spacing: 16

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    visible: reader.loading
                    running: reader.loading
                }
                Label {
                    objectName: "readerError"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    font.pixelSize: reader.loading ? 14 : 18
                    color: reader.loading ? theme.colors.muted : theme.foreground
                    text: reader.loading ? qsTr("Opening %1…").arg(reader.title) : reader.error
                }
                AppButton {
                    objectName: "backToLibrary"
                    Layout.alignment: Qt.AlignHCenter
                    visible: !reader.loading
                    focusPolicy: Qt.NoFocus
                    text: qsTr("Back to the Library")
                    onClicked: reader.close()
                }
            }

            // For fingers: one button that is always there, which shows the
            // bar of everything else (the contents, find, the AI, ...).
            AppButton {
                objectName: "menuButton"
                visible: win.touch && reader.open
                z: 3
                anchors.right: parent.right
                anchors.bottom: progress.top
                anchors.margins: 16
                width: 52
                height: 52
                focusPolicy: Qt.NoFocus
                opacity: view.showBar ? 1 : 0.75
                text: view.showBar ? "✕" : "☰"
                font.pixelSize: 20
                onClicked: view.showBar = !view.showBar
            }

            Rectangle {
                objectName: "readerBar"
                visible: view.showBar && reader.open
                z: 2
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: progress.top
                anchors.bottomMargin: 80
                width: bar.childrenRect.width + 24
                height: bar.childrenRect.height + 24
                radius: theme.radius
                color: theme.background
                border.color: theme.colors.selection

                Flow {
                    id: bar
                    // One row where the window has room for it, more where it doesn't.
                    x: 12
                    y: 12
                    width: Math.min(660, contentLoader.width - 48)
                    spacing: 8
                    AppButton { objectName: "barBack"; text: qsTr("Library"); focusPolicy: Qt.NoFocus; onClicked: reader.close() }
                    AppButton { text: qsTr("Contents"); focusPolicy: Qt.NoFocus; onClicked: { view.showBar = false; view.toggleIndex() } }
                    AppButton { text: qsTr("Find"); focusPolicy: Qt.NoFocus; onClicked: { view.showBar = false; finder.open() } }
                    AppButton { text: qsTr("Bookmark"); focusPolicy: Qt.NoFocus; onClicked: { view.showBar = false; view.bookmark() } }
                    AppButton { objectName: "barGraph"; text: qsTr("Graph"); focusPolicy: Qt.NoFocus; onClicked: { view.showBar = false; view.toggleGraph() } }
                    AppButton { objectName: "barAsk"; text: qsTr("Ask AI"); focusPolicy: Qt.NoFocus; onClicked: { view.showBar = false; view.askAssistant() } }
                    AppButton { text: qsTr("Smaller"); focusPolicy: Qt.NoFocus; onClicked: if (view.content) view.content.zoom(-1) }
                    AppButton { objectName: "barLarger"; text: qsTr("Larger"); focusPolicy: Qt.NoFocus; onClicked: if (view.content) view.content.zoom(1) }
                }
            }

            Rectangle {
                id: progress
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                height: 2
                width: parent.width * Math.min(1, reader.percent / 100)
                color: theme.accent
                visible: reader.open
            }
        }

        ModSlot {
            id: sideSlot
            place: "reader.side"
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.rightMargin: view.beside ? (assistantSide.visible ? assistantSide.width : 0) + (graphSide.visible ? graphSide.width : 0) : 0
        }
        // The page behind a pane that lies over it, dimmed; a click on it
        // puts the pane away, as Escape does.
        Rectangle {
            objectName: "paneScrim"
            anchors.fill: parent
            visible: !view.beside && view.topPane !== ""
            color: Qt.alpha(theme.background, 0.6)
            z: 4
            TapHandler { onTapped: view.back() }
        }
        SidePane {
            id: indexSide
            objectName: "contentsSide"
            name: "contents"
            leading: true
            standard: 300
            least: 220
            most: 520
            over: !view.beside
            room: over ? view.overRoom
                       : view.width - view.slotWidth - win.contentFloor - (view.showAssistant ? assistantSide.least : 0)
                         - (view.graphOpen ? graphSide.least : 0)
            visible: view.indexOpen && (view.beside || view.topPane === "contents")
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            z: 5

            ContentsPane {
                id: index
                objectName: "contents"
                anchors.fill: parent
                onDone: {
                    view.showIndex = false
                    if (view.content) view.content.forceActiveFocus()
                }
            }
        }
        SidePane {
            id: graphSide
            objectName: "graphSide"
            name: "graph"
            standard: 480
            // The map has a narrow form of its own (see GraphPane's compact).
            least: 280
            most: 900
            over: !view.beside
            room: over ? view.overRoom
                       : view.width - view.slotWidth - win.contentFloor - (indexSide.visible ? indexSide.width : 0)
                         - (assistantSide.visible ? assistantSide.width : 0)
            visible: view.graphOpen && (view.beside || view.topPane === "graph")
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.rightMargin: view.beside && assistantSide.visible ? assistantSide.width : 0
            z: 5

            GraphPane {
                id: knowledge
                objectName: "readerGraph"
                anchors.fill: parent
                reading: true
                active: view.active
                onDone: {
                    view.showGraph = false
                    if (view.content) view.content.forceActiveFocus()
                }
            }
        }
        SidePane {
            id: assistantSide
            objectName: "assistantSide"
            name: "assistant"
            standard: 400
            least: 280
            most: 720
            over: !view.beside
            room: over ? view.overRoom
                       : view.width - view.slotWidth - win.contentFloor - (indexSide.visible ? indexSide.width : 0)
                         - (view.graphOpen ? graphSide.least : 0)
            visible: view.showAssistant
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            z: 5

            AssistantPane {
                id: companion
                objectName: "assistantPane"
                anchors.fill: parent
                selection: view.content ? view.content.selectedText : ""
                onDone: {
                    view.showAssistant = false
                    if (view.content) view.content.forceActiveFocus()
                }
            }
        }
    }

    FindPopup { id: finder }
}
