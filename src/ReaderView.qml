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
    // The "Aa" panel: how the text is set. It takes the side the AI and the
    // map are on, so it puts them away and they put it away; in a compact
    // window it lies along the foot of the page instead, so the page it
    // changes is still there to be seen.
    property bool showFormat: false
    // What there was when the panel opened, for its Reset, and whether its
    // changes go to this book alone.
    property var formatBefore: null
    property bool formatBookOnly: false
    // Whether they go to the book's collection, between the defaults and the book.
    property bool formatForCollection: false
    // A book in several collections is set as the one it was opened from.
    Binding { target: reader.format; property: "preferredCollection"; value: backend.collection }
    // While the panel has the keyboard, its keys are its own.
    readonly property bool formatting: showFormat && ((formatSide.visible && formatSide.loaded && formatSide.loaded.focused)
                                                      || (formatSheet.visible && formatSheet.loaded && formatSheet.loaded.focused))
    // The graph's things marked in the text: while the map is beside the
    // page, always, or never (`marks` in config.json). Pages that are
    // pictures (PDF, comics) have no words to mark.
    readonly property bool marksOn: reader.kind === "flow"
                                    && (settings.marks === "always" || (settings.marks === "map" && graphOpen))
    // Selecting words with the keyboard (see startSelecting).
    property bool selecting: false
    // While the keyboard selects, or works the menu beside the words, its
    // keys are theirs, not the book's.
    readonly property bool popupOpen: finder.opened || knowledge.popupOpen || selecting || (menu.shown && menu.activeFocus) || notesLayer.focused
    // While the map, or a plugin's view beside the page, has the keyboard, the keys are its own (see GraphPane, PluginView).
    readonly property bool mapping: knowledge.focused || pluginSide.focused
    // The plugin whose view beside the page has the keyboard, for its commands; "" for none.
    readonly property string pluginKeys: pluginSide.live ? pluginSide.plugin : ""
    // Whether the keys that move through the book are the book's right now.
    readonly property bool paging: win.inReader && !!content && !index.focused && !mapping && !formatting
    readonly property var content: contentLoader.item
    // A page made anew (a book opened, the layout or the window changed) has
    // the keyboard, unless something else of the reader has it, such as a
    // pane's field: the shelf behind it never keeps it.
    onContentChanged: if (content && win.inReader && !win.within(view, win.activeFocusItem)) content.forceActiveFocus()

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
        if (showFormat) closeFormat()
        else if (showAssistant) showAssistant = false
        else if (showGraph) toggleGraph()
        else if (pluginSide.shown) pluginSide.close()
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
    // The contents or the highlights (#81) beside the page: the other one
    // shown in its place, the same one put away.
    function showList(tab) {
        if (showIndex && index.tab === tab) return toggleIndex()
        index.tab = tab
        if (showIndex) index.takeFocus()
        else toggleIndex()
    }
    function openFinder() { finder.open() }
    // Words highlighted, and a highlight tapped (#81).
    readonly property QtObject highlighting: ReaderHighlighting { view: view; menu: menu; touchArea: touchArea }
    // The graph beside the page, on what a plugin asked for.
    // A plugin's view beside the page (PluginSide), on what the plugin asked for.
    function showPluginView(entry, args, toggle) { pluginSide.show(entry, args, toggle) }
    function closePluginView(plugin, id) { pluginSide.closeView(plugin, id) }
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
    function toggleFormat() {
        if (showFormat) {
            closeFormat()
            return
        }
        formatBefore = reader.format.snapshot()
        formatBookOnly = Object.keys(reader.format.own).length > 0
        formatForCollection = !formatBookOnly && Object.keys(reader.format.collectionOwn).length > 0
        // Under the page in a compact window, where the contents would lie over it.
        if (win.compact) showIndex = false
        showFormat = true
    }
    function closeFormat() {
        showFormat = false
        if (content) content.forceActiveFocus()
    }
    // The panel's Reset: what there was when it opened.
    function restoreFormat() {
        if (!formatBefore) return
        reader.format.restore(formatBefore)
        formatBookOnly = Object.keys(reader.format.own).length > 0
        formatForCollection = !formatBookOnly && Object.keys(reader.format.collectionOwn).length > 0
    }
    onShowFormatChanged: if (showFormat) { showAssistant = false; showGraph = false; pluginSide.close() }
    onShowAssistantChanged: if (showAssistant) { showFormat = false; pluginSide.close() }
    onShowGraphChanged: if (showGraph) { showFormat = false; pluginSide.close() }
    // The book and nothing else: the whole screen, no title line, no panes.
    function expand(yes) {
        immersive = yes
        if (yes) {
            showFormat = false
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
    // The menu, the card and what they lead to.
    function fromContent(rect) {
        const at = content.mapToItem(room, rect.x, rect.y)
        return Qt.rect(at.x, at.y, rect.width, rect.height)
    }
    property var pointedThing: null
    // The pointer came onto a marked thing, or left it: a card a moment
    // later, gone a moment after the pointer leaves both.
    function pointed(thing) {
        pointedThing = thing
        if (thing) {
            hideSoon.stop()
            if (!menu.shown || menu.passing) showSoon.restart()
        } else {
            showSoon.stop()
            if (menu.shown && menu.passing) hideSoon.restart()
        }
    }
    Timer {
        id: showSoon
        interval: 350
        onTriggered: if (view.pointedThing && (!menu.shown || menu.passing)) menu.openCard(view.pointedThing, view.fromContent(view.pointedThing.rect), false, true)
    }
    Timer {
        id: hideSoon
        interval: 400
        onTriggered: if (menu.passing && !menu.hovering && !view.pointedThing) menu.close()
    }
    // A press of the mouse ended: on selected words, their menu; on a
    // marked thing, its card, which stays.
    function released() {
        const chosen = content && content.selection ? content.selection() : null
        if (chosen && chosen.text.trim() !== "") {
            showSoon.stop()
            chosen.rect = fromContent(chosen.rect)
            chosen.highlight = typeof content.selectionHighlight === "function" ? content.selectionHighlight() : ({})
            menu.openMenu(chosen)
        } else if (pointedThing) {
            showSoon.stop()
            menu.openCard(pointedThing, fromContent(pointedThing.rect), false, false)
        } else if (content && typeof content.pressedHighlight === "function") {
            const clicked = content.pressedHighlight()
            if (clicked) menu.openHighlight(clicked, fromContent(clicked.rect))
        }
    }

    // A finger: a tap on a marked thing shows its card; a long press selects
    // the word under it and shows the menu.
    function tappedAt(x, y) {
        if (highlighting.tapped(x, y)) return true
        if (!content || typeof content.thingAt !== "function") return false
        const at = touchArea.mapToItem(content, x, y)
        const found = content.thingAt(at.x, at.y)
        if (!found) return false
        menu.openCard(found, fromContent(found.rect), false, false)
        return true
    }
    function pressedLongAt(x, y) {
        if (!content || typeof content.selectAt !== "function") return
        const at = touchArea.mapToItem(content, x, y)
        if (content.selectAt(at.x, at.y)) released()
    }
    // V: the first word on the screen is selected, and the keys move it on
    // or widen it (see SelectKeys); Enter shows the menu, or for a margin note writes it.
    function startSelecting(purpose) {
        menu.close()
        if (!content || typeof content.beginSelecting !== "function" || content.beginSelecting() === false) {
            backend.say(qsTr("These pages are pictures, so there are no words to select."))
            return
        }
        selecting = true
        selectKeys.purpose = purpose === "note" ? "note" : "menu"
        selectKeys.take()
    }
    // Stops; the selection kept for its menu or note, or (`kept`) for whoever goes on with it.
    function stopSelecting(keep, kept) {
        selecting = false
        if (!content) return
        content.endSelecting()
        if (keep && selectKeys.purpose === "note") notesLayer.write()
        else if (keep) released()
        else if (!kept) content.clearSelection()
        if (!keep) content.forceActiveFocus()
    }
    function showThing(id) {
        if (!showGraph) toggleGraph()
        knowledge.showThing(id)
    }
    function askAbout(text, isThing) {
        showAssistant = true
        companion.prepare(isThing ? qsTr("Tell me about %1.").arg(text) : "")
    }
    function openAssistant() {
        showAssistant = true
        companion.takeFocus()
    }
    // Shift+M: marks while the map is open, always, never, and round again.
    function cycleMarks() {
        const next = settings.marks === "map" ? "always" : settings.marks === "always" ? "never" : "map"
        settings.marks = next
        backend.say(next === "always" ? qsTr("The graph's things are marked in the text, always")
                  : next === "never" ? qsTr("Nothing is marked in the text")
                  : qsTr("The graph's things are marked in the text while the map is open"))
    }
    // Shift+P: as many pages as fit, one, two, three, and round again.
    function cyclePages() {
        settings.pages = (settings.pages + 1) % 4
        const wanted = settings.pages
        backend.say(wanted === 0 ? qsTr("Automatic: %Ln pages here", "", reader.format.automaticColumns)
                  : reader.format.pagesHonoured ? qsTr("%Ln pages side by side", "", wanted)
                  : qsTr("Only %Ln pages fit at this width with lines of at least %1 characters", "", reader.format.columns)
                        .arg(reader.format.minimumMeasure.toLocaleString(Qt.locale(), 'f', 0)))
    }
    // Shift+L: pages, one scrolling column, automatic, and round again.
    function cycleLayout() {
        const next = reader.format.nextLayout()
        backend.say(next === "pages" ? qsTr("Pages that turn, each chapter starting on a page of its own")
                  : next === "scroll" ? qsTr("One column that scrolls on through the chapters")
                  : qsTr("Automatic: pages where two fit side by side, else a column that scrolls"))
    }
    function bookmark() {
        if (!content) return
        reader.addBookmark(content.placeText())
        backend.say(qsTr("Bookmarked"))
    }

    // The reader's keys, each in the place it belongs to right now.
    ReaderKeys { host: view; notes: notesLayer; aiPane: companion }

    // The kinds' colors are the graph's, from the theme.
    Binding { target: reader.marks; property: "colors"; value: theme.colors }
    Binding { target: reader.marks; property: "foreground"; value: theme.foreground }
    Binding { target: reader.marks; property: "background"; value: theme.background }
    Binding { target: reader.highlights; property: "foreground"; value: theme.foreground }
    Binding { target: reader.highlights; property: "background"; value: theme.background }
    // Links in the text take the accent.
    Binding { target: reader.format; property: "linkColor"; value: theme.accent }
    // The room the text has, for how many pages stand side by side.
    Binding { target: reader.format; property: "available"; value: contentLoader.width }
    // What the AI noted while reading is marked as soon as it is in.
    Connections {
        target: assistant
        function onGraphChanged() { reader.marks.refresh() }
    }
    Connections {
        target: view.content
        ignoreUnknownSignals: true
        function onPointed(thing) { view.pointed(thing) }
        function onSelected() { view.released() }
        function onSelectedTextChanged() { if (menu.shown && menu.mode === "menu" && view.content.selectedText === "") menu.close() }
    }
    // A card or a menu is about what is on the page; the page moved on.
    Connections {
        target: reader
        function onSectionChanged() { menu.close() }
        function onFractionChanged() { if (!view.selecting) menu.close() }
        function onBookChanged() { menu.close() }
    }

    // The panes: the contents on the side the text starts at, the map and
    // the AI on the side it ends at. They stand beside the page for as long
    // as the page keeps its floor with every open pane at its narrowest. In
    // a window with less room than that they lie over the page instead, one
    // at a time (the AI before the map before the contents, the order
    // Escape closes them in), so the page is never squeezed and two panes
    // are never side by side in a narrow window.
    readonly property bool indexOpen: showIndex && reader.open
    readonly property bool graphOpen: showGraph && reader.open
    // The panel beside the page; under it in a compact window.
    readonly property bool formatOpen: showFormat && reader.open && !win.compact
    readonly property real slotWidth: sideSlot.visible ? sideSlot.width : 0
    readonly property bool beside: width - slotWidth - (indexOpen ? indexSide.least : 0) - (showAssistant ? assistantSide.least : 0)
                                   - (graphOpen ? graphSide.least : 0) - (formatOpen ? formatSide.least : 0)
                                   - (pluginSide.shown ? pluginSide.least : 0) >= win.contentFloor
    readonly property string topPane: formatOpen ? "format" : pluginSide.shown ? "plugin" : showAssistant ? "assistant" : graphOpen ? "graph"
                                    : indexOpen ? "contents" : ""
    // What a pane lying over the page may take: all but a strip of the
    // page, which is what is clicked to put the pane away.
    readonly property real overRoom: width - 48
    // Wider or narrower, by the keys: the pane that has the keyboard, or
    // the one on top.
    function resizePane(steps) {
        const pane = index.focused && indexSide.visible ? indexSide : knowledge.focused && graphSide.visible ? graphSide
                   : topPane === "format" ? formatSide : topPane === "plugin" ? pluginSide
                   : topPane === "assistant" ? assistantSide : topPane === "graph" ? graphSide : topPane === "contents" ? indexSide : null
        if (pane) pane.resize(steps)
    }

    Item {
        id: room
        anchors.fill: parent

        Item {
            anchors.fill: parent
            anchors.leftMargin: view.beside && indexSide.visible ? indexSide.width : 0
            anchors.rightMargin: view.slotWidth + (view.beside ? (assistantSide.visible ? assistantSide.width : 0)
                                                                 + (graphSide.visible ? graphSide.width : 0)
                                                                 + (formatSide.visible ? formatSide.width : 0)
                                                                 + (pluginSide.visible ? pluginSide.width : 0) : 0)

            // Where you are: the book and chapter, and how far through.
            RowLayout {
                id: header
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: view.immersive ? 0 : 12
                // Clear of the ⋯ at its end.
                anchors.rightMargin: view.immersive ? 0 : 12 + (readerMore.visible ? readerMore.width + 4 : 0)
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
                // The page itself is how a pointer moves through it: a finger
                // taps its edges or swipes, a mouse wheel scrolls it or, on
                // pages side by side, turns them.
                readonly property string runs: "reader.page_down reader.page_up reader.line_down reader.line_up"

                TapHandler {
                    acceptedDevices: PointerDevice.TouchScreen
                    onLongPressed: view.pressedLongAt(point.position.x, point.position.y)
                    onTapped: (point) => {
                        win.touchSeen = true
                        if (!view.content) return
                        if (menu.shown) {
                            menu.close()
                            return
                        }
                        if (view.tappedAt(point.position.x, point.position.y)) return
                        // What the page has to tap on itself: the line at a chapter's end.
                        const on = touchArea.mapToItem(view.content, point.position.x, point.position.y)
                        if (typeof view.content.activateAt === "function" && view.content.activateAt(on.x, on.y)) return
                        const x = win.rightToLeft ? touchArea.width - point.position.x : point.position.x
                        if (x < touchArea.width * 0.25) view.turn(false)
                        else if (x > touchArea.width * 0.75) view.turn(true)
                        else view.showBar = !view.showBar
                    }
                }
                DragHandler {
                    id: swipe
                    objectName: "pageSwipe"
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
                // A finger on the page: while it is down, the text doesn't
                // select under it, so it scrolls the page instead (a press
                // and hold still selects a word, see pressedLongAt).
                PointHandler {
                    id: finger
                    acceptedDevices: PointerDevice.TouchScreen
                }
                PinchHandler {
                    acceptedDevices: PointerDevice.TouchScreen
                    target: null
                    onScaleChanged: (delta) => { if (view.content) view.content.pinch(delta) }
                }
                // Pages that turn don't scroll, so the wheel turns them: a
                // notch of a mouse wheel, or as much from a touchpad.
                WheelHandler {
                    objectName: "pageWheel"
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    enabled: !!view.content && (view.content.objectName === "spreadReader" || view.content.turnsPages === true)
                    property real gathered: 0
                    onWheel: (event) => {
                        gathered += event.angleDelta.y
                        if (Math.abs(gathered) < 120) return
                        view.turn(gathered < 0)
                        gathered = 0
                    }
                }
                // A column that runs on: the wheel scrolls it as always, and
                // turned on two notches past a chapter's end (or back past
                // its start), it goes on into the next chapter (or back to
                // the end of the one before); one notch is only resistance. The column below
                // still takes every turn of the wheel.
                WheelHandler {
                    objectName: "runOnWheel"
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    enabled: !!view.content && view.content.objectName === "flowReader" && view.content.runsOn
                    blocking: false
                    property real gathered: 0
                    onWheel: (event) => {
                        const down = event.angleDelta.y < 0
                        const there = down ? view.content.atEnd() : view.content.atStart()
                        if (!there || (gathered !== 0 && (gathered < 0) !== down)) {
                            gathered = 0
                            if (!there) return
                        }
                        gathered += event.angleDelta.y
                        if (Math.abs(gathered) < 240) return
                        gathered = 0
                        if (down) view.content.next()
                        else view.content.backToEnd()
                    }
                }
            }

            // The pen over the page: gestures that highlight words, or book ink on a PDF's or a comic's pages.
            ReaderPen { id: pen; host: view; anchors.fill: contentLoader; z: 1 }

            Loader {
                id: contentLoader
                objectName: "contentLoader"
                anchors.top: header.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: progress.top
                anchors.topMargin: 4
                transform: Translate { x: view.slide }
                // The layout (readingformat.h): pages that turn, one to
                // three side by side; or one column that scrolls and runs
                // on into the next chapter. Automatic: room for more than
                // one page at the measure is an open book of two or three
                // pages that turn, otherwise a single page that scrolls
                // (PageLayout::columns decides, from this width, which is
                // what the panes beside the page leave).
                readonly property string layout: reader.format.effective.layout || "auto"
                readonly property bool paged: layout === "pages" || (layout === "auto" && reader.format.columns > 1)
                sourceComponent: reader.kind === "flow" ? (paged ? spreadPages : flow) : reader.kind === "pages" ? pages : null
                Component { id: flow; FlowReader { marking: view.marksOn; runsOn: contentLoader.layout === "scroll"; fingerDown: finger.active; notes: notesLayer } }
                Component { id: spreadPages; SpreadReader { marking: view.marksOn; notes: notesLayer } }
                Component { id: pages; PageReader { layout: contentLoader.layout; covering: pen.covering } }
            }

            ReadingNotes { id: notesLayer; anchors.fill: contentLoader; z: 2; host: view }
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
            // For a mouse: what the bar holds and the rest of the reading
            // keys, as a list, in the corner where the title line ends. In
            // the book alone on the screen it is there too, faintly, as the
            // way back out for a pointer.
            HeaderChoice {
                id: readerMore
                objectName: "readerMore"
                visible: !win.touch && reader.open
                z: 3
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: view.immersive ? 6 : 9
                anchors.rightMargin: view.immersive ? 6 : 8
                height: view.immersive ? 30 : 24
                opacity: view.immersive && !moreHover.hovered ? 0.35 : 1
                worded: false
                glyph: "more"
                text: qsTr("More")
                onTapped: win.showMenu("reader")
                HoverHandler { id: moreHover }
            }

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

            ReaderBar {
                host: view
                room: contentLoader.width
                visible: view.showBar && reader.open
                z: 2
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: progress.top
                anchors.bottomMargin: 80
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
            anchors.rightMargin: view.beside ? (assistantSide.visible ? assistantSide.width : 0) + (graphSide.visible ? graphSide.width : 0)
                                               + (formatSide.visible ? formatSide.width : 0) + (pluginSide.visible ? pluginSide.width : 0) : 0
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
                         - (view.graphOpen ? graphSide.least : 0) - (view.formatOpen ? formatSide.least : 0)
                         - (pluginSide.shown ? pluginSide.least : 0)
            visible: view.indexOpen && (view.beside || view.topPane === "contents")
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            z: 5

            ContentsPane {
                id: index
                objectName: "contents"
                host: view
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
                onThingActivated: (id, at) => {
                    const found = reader.marks.thing(id)
                    if (!found.id) return
                    const there = companion.mapToItem(room, at.x, at.y)
                    menu.openCard(found, Qt.rect(there.x - 160, there.y, 1, 18), true, false)
                }
                onDone: {
                    view.showAssistant = false
                    if (view.content) view.content.forceActiveFocus()
                }
            }
        }

        PluginSide {
            id: pluginSide
            objectName: "pluginSide"
            host: view
            over: !view.beside
            room: over ? view.overRoom : view.width - view.slotWidth - win.contentFloor - (indexSide.visible ? indexSide.width : 0)
            visible: shown && (view.beside || view.topPane === "plugin")
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            z: 5
        }
        Component {
            id: formatPane
            FormatPane {
                objectName: "formatPane"
                host: view
                onDone: view.closeFormat()
            }
        }
        SidePane {
            id: formatSide
            objectName: "formatSide"
            name: "format"
            standard: 320
            least: 260
            most: 520
            over: !view.beside
            room: over ? view.overRoom
                       : view.width - view.slotWidth - win.contentFloor - (indexSide.visible ? indexSide.width : 0)
            visible: view.formatOpen && (view.beside || view.topPane === "format")
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            z: 5
            readonly property Item loaded: formatSideLoader.item

            Loader {
                id: formatSideLoader
                anchors.fill: parent
                active: formatSide.visible
                sourceComponent: formatPane
                onLoaded: item.takeFocus()
            }
        }
        // In a compact window: along the foot of the page, no higher than
        // leaves the top of the page in sight.
        Rectangle {
            id: formatSheet
            objectName: "formatSheet"
            visible: view.showFormat && reader.open && win.compact
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.min(parent.height * 0.6, formatSheetLoader.item ? formatSheetLoader.item.wanted : 360)
            z: 5
            color: theme.background
            readonly property Item loaded: formatSheetLoader.item
            Rectangle {
                width: parent.width
                height: 1
                color: theme.colors.selection
            }
            // A press on the sheet's ground stays on the sheet.
            TapHandler {}
            Loader {
                id: formatSheetLoader
                anchors.fill: parent
                anchors.topMargin: 1
                active: formatSheet.visible
                sourceComponent: formatPane
                onLoaded: item.takeFocus()
            }
        }

        SelectKeys {
            id: selectKeys
            anchors.fill: parent
            view: view
        }
        ReadingMenu {
            id: menu
            z: 7
            onShowInGraph: (id) => view.showThing(id)
            onAddToGraph: (name) => {
                if (!view.showGraph) view.toggleGraph()
                knowledge.addNamed(name)
            }
            onAskAbout: (text, isThing) => view.askAbout(text, isThing)
            onAsk: (question, words) => {
                view.openAssistant()
                assistant.ask(question, "chapter", words)
            }
            onIdentify: (words, paragraph) => {
                view.openAssistant()
                assistant.identify(words, paragraph)
            }
            onFind: (words) => finder.findWords(words)
            onHighlight: (color) => view.highlighting.highlight(color)
            onShowHighlights: view.showList("annotations")
            onCopy: if (view.content) view.content.copySelection()
            onWriteNote: notesLayer.write()
            onGoTo: (met) => reader.goToPlace(met.section, met.fraction)
            onDismissed: (hadFocus) => { if (hadFocus && view.content) view.content.forceActiveFocus() }
        }
    }

    FindPopup { id: finder }
}
