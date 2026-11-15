import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// The knowledge graph, browsable: what is known as nodes with the relations
// between them drawn as a picture, or the same nodes as a list by kind, and
// everything known of the selected one beside it. It shows the whole
// library, one book or series, or a world of its own; beside the page
// (`reading`) it shows the open book only as far as its reader has got.
//
// The kinds above the picture are its legend and its filter at once. The
// arrow keys walk along the relations, Enter looks around the selected
// node, and Backspace goes back the way it came.
FocusScope {
    id: pane
    // Whether single keys are this view's shortcuts right now.
    property bool active: true
    // Beside the page: the open book's graph, up to where the reader is.
    property bool reading: false
    // The nodes as a list rather than a picture.
    property bool listing: false
    property bool showDetails: true
    // In a window too low for the picture and the details at once: whether
    // the details were asked for and stand in the picture's place.
    property bool peek: false
    // Too narrow for the picture and the details side by side: the picture
    // would not keep the floor every pane leaves what it stands beside
    // (`win.contentFloor`) next to the details at the width they come
    // with. The pane is asked, not the window: beside the page it is a
    // part of the window, and as wide as it was dragged. Compact, the
    // details stand under the picture, and names give way to marks.
    readonly property bool compact: width < win.contentFloor + detailsSide.standard
    // The least height the picture keeps over the details in a compact
    // pane. With less, which is what a low window (`win.low`) leaves of it,
    // it is one or the other: the details open on request in the picture's place.
    readonly property real pictureFloor: 300
    // The details beside the picture, wider or narrower by the keys.
    function resizePane(steps) { if (detailsSide.visible) detailsSide.resize(steps) }
    readonly property bool focused: activeFocus
    readonly property bool popupOpen: chooser.opened || sheet.opened
    readonly property alias browser: graph
    readonly property bool keysOn: active && visible && (!reading || focused)
    readonly property var node: graph.details
    readonly property bool hasNode: graph.selected > 0
    // The furthest the open book was read while the pane was up. Going back
    // to an earlier page (to where someone was first met, say) doesn't take
    // away what is known already.
    property var furthest: ({ bookId: 0, section: 0, fraction: 0 })
    // A place to go to once the book it is in has opened.
    property var pending: null
    // Whether the AI is noting what was read: the open book beside the
    // page, or books read before, asked for from the shelf.
    readonly property bool growing: reading ? assistant.graphBusy : backend.graphJobs.busy
    // Whether the picture has been settling long enough to be noticed.
    property bool arranging: false
    // Showing what is known past the reader's place is asked first.
    property bool askingWhole: false
    readonly property string wholeWarning: graph.spoilersAfter.bookId
        ? qsTr("This shows what happens after %1.").arg(words.place(graph.spoilersAfter))
        : qsTr("This shows what happens later in the book.")
    onAskingWholeChanged: if (askingWhole) Qt.callLater(() => { if (wholeConfirm.visible) wholeConfirm.forceActiveFocus() })
    signal done()

    function takeFocus() { (listing ? rows : canvas).forceActiveFocus() }
    function keyOf(command) { return (custom.keyText[command] || "").split("  ")[0] }
    function showLibrary() { graph.showScope("") }
    function showBook(id) { graph.showBook(id) }
    // A scope that was asked for by name (a plugin's world, say) stays up
    // beside the page until the pane is put away, rather than giving way
    // to the open book at the next page turned.
    property bool held: false
    function showScope(scope) {
        held = true
        graph.showScope(scope)
    }
    // The open book, as far as it was read.
    function follow() {
        if (!reader.open || held) return
        const ahead = furthest.bookId !== reader.bookId || reader.section > furthest.section
                      || (reader.section === furthest.section && reader.fraction > furthest.fraction)
        if (ahead) furthest = { bookId: reader.bookId, section: reader.section, fraction: reader.fraction }
        graph.showBookUpTo(furthest.bookId, furthest.section, furthest.fraction)
    }
    // A thing named on the page: the open book's graph, with it selected and in sight.
    function showThing(id) {
        if (reading) {
            held = false
            follow()
        }
        graph.jumpTo(id)
    }
    // Words on the page the graph doesn't know, added by hand.
    function addNamed(name) { sheet.show("add", {}, null, name) }
    function move(way) {
        if (!listing) graph.step(way)
        else if (way === "up" || way === "down") graph.advance(way === "up" ? -1 : 1)
    }
    function read(bookId) {
        if (reader.open && reader.bookId === bookId) return
        reader.openBook(bookId)
    }
    // To a moment of a book: at once when it is the open one, else once it
    // has opened. The words kept with it find the very spot, and are lit.
    function go(place) {
        if (reader.open && reader.bookId === place.bookId) {
            reader.marks.goToMoment(place.section, place.fraction, place.anchor || "")
            return
        }
        pending = place
        reader.openBook(place.bookId)
    }
    // O: a book is read; anything else is gone to where it was first met.
    function openSelected() {
        if (!hasNode) return
        if (node.kind === "book" && node.bookId > 0) {
            if (node.readable) read(node.bookId)
            else backend.say(qsTr("That book has no file to read here."))
            return
        }
        const met = (node.places || []).find(place => place.readable)
        if (met) go(met)
        else backend.say(qsTr("It wasn't met in a book that can be opened here."))
    }
    function editSelected() { if (hasNode) sheet.show("edit", node) }
    function removeSelected() {
        if (!hasNode) return
        if (node.kind === "book") backend.say(qsTr("A book is removed from the library, on the shelf."))
        else sheet.show("remove", node)
    }
    function mergeSelected() {
        if (!hasNode) return
        if (node.kind === "book") backend.say(qsTr("Books are merged in the library, not here."))
        else chooser.show("merge", graph.selected)
    }

    GraphBrowser {
        id: graph
        source: backend
        // The picture is laid out as wide or as tall as the room it has.
        // Where the details stand under it, that is about half the height.
        aspect: body.height > 0 ? body.width / (pane.compact ? body.height * 0.55 : body.height) : 1
        // The selected node is kept in sight as the keys move to it.
        onSelectedChanged: if (selected > 0) canvas.reveal(selected)
    }
    GraphWords { id: words }

    // Beside the page the graph follows the reader, a moment after they stop.
    Timer {
        id: followSoon
        interval: 400
        onTriggered: pane.follow()
    }
    Connections {
        target: reader
        enabled: pane.reading
        function onSectionChanged() { if (pane.visible) followSoon.restart() }
        function onFractionChanged() { if (pane.visible) followSoon.restart() }
        function onBookChanged() { if (pane.visible && reader.open) pane.follow() }
    }
    Connections {
        target: reader
        function onBookChanged() {
            if (!pane.pending || !reader.open || reader.bookId !== pane.pending.bookId) return
            reader.marks.goToMoment(pane.pending.section, pane.pending.fraction, pane.pending.anchor || "")
            pane.pending = null
        }
        // A book that didn't open leaves nothing to go to.
        function onLoadingChanged() { if (reader.error !== "") pane.pending = null }
    }
    // What the AI noted while reading shows as soon as it is in.
    Connections {
        target: assistant
        function onGraphChanged() { graph.refresh() }
    }
    Connections {
        target: backend.graphJobs
        function onGrown() { graph.refresh() }
    }
    Connections {
        target: graph
        function onSettlingChanged() {
            if (graph.settling) arrangingSoon.restart()
            else {
                arrangingSoon.stop()
                pane.arranging = false
            }
        }
    }
    Timer {
        id: arrangingSoon
        interval: 700
        onTriggered: pane.arranging = graph.settling
    }
    onVisibleChanged: {
        if (!visible) {
            held = false
            return
        }
        if (reading) follow()
        else graph.refresh()
    }

    Key { command: "graph.left"; when: pane.keysOn; onActivated: pane.move("left") }
    Key { command: "graph.right"; when: pane.keysOn; onActivated: pane.move("right") }
    Key { command: "graph.up"; when: pane.keysOn; onActivated: pane.move("up") }
    Key { command: "graph.down"; when: pane.keysOn; onActivated: pane.move("down") }
    Key { command: "graph.next"; when: pane.keysOn; onActivated: graph.advance(1) }
    Key { command: "graph.previous"; when: pane.keysOn; onActivated: graph.advance(-1) }
    Key { command: "graph.expand"; when: pane.keysOn; onActivated: graph.recentre() }
    Key { command: "graph.back"; when: pane.keysOn; onActivated: graph.back() }
    Key { command: "graph.close"; when: pane.keysOn; onActivated: pane.done() }
    Key { command: "graph.find"; when: pane.keysOn; onActivated: chooser.show("find") }
    Key { command: "graph.filter"; when: pane.keysOn; onActivated: chooser.show("filter") }
    Key { command: "graph.list"; when: pane.keysOn; onActivated: { pane.listing = !pane.listing; pane.takeFocus() } }
    Key {
        command: "graph.details"
        when: pane.keysOn
        onActivated: {
            if (body.either) pane.peek = !pane.peek
            else pane.showDetails = !pane.showDetails
        }
    }
    Key { command: "graph.open"; when: pane.keysOn; onActivated: pane.openSelected() }
    // The keyboard to the selected thing's moments, in the details.
    Key {
        command: "graph.moments"
        when: pane.keysOn && pane.hasNode
        onActivated: {
            if (body.either) pane.peek = true
            else pane.showDetails = true
            Qt.callLater(() => { if (!details.takeKeys()) backend.say(qsTr("Nothing is known yet of when it was revealed.")) })
        }
    }
    Key { command: "graph.further"; when: pane.keysOn; onActivated: graph.depth = Math.min(4, graph.depth + 1) }
    Key { command: "graph.nearer"; when: pane.keysOn; onActivated: graph.depth = Math.max(0, graph.depth - 1) }
    Key { command: "graph.zoom_in"; when: pane.keysOn && !pane.listing; onActivated: canvas.zoomBy(1.3) }
    Key { command: "graph.zoom_out"; when: pane.keysOn && !pane.listing; onActivated: canvas.zoomBy(1 / 1.3) }
    Key { command: "graph.fit"; when: pane.keysOn && !pane.listing; onActivated: canvas.fit() }
    Key { command: "graph.pan_left"; when: pane.keysOn && !pane.listing; onActivated: canvas.panBy(canvas.width / 5, 0) }
    Key { command: "graph.pan_right"; when: pane.keysOn && !pane.listing; onActivated: canvas.panBy(-canvas.width / 5, 0) }
    Key { command: "graph.pan_up"; when: pane.keysOn && !pane.listing; onActivated: canvas.panBy(0, canvas.height / 5) }
    Key { command: "graph.pan_down"; when: pane.keysOn && !pane.listing; onActivated: canvas.panBy(0, -canvas.height / 5) }
    Key { command: "graph.beyond"; when: pane.keysOn && graph.showing === "book"; onActivated: graph.beyond = !graph.beyond }
    Key { command: "graph.add"; when: pane.keysOn; onActivated: sheet.show("add", pane.node) }
    Key { command: "graph.edit"; when: pane.keysOn; onActivated: pane.editSelected() }
    Key { command: "graph.merge"; when: pane.keysOn; onActivated: pane.mergeSelected() }
    Key { command: "graph.remove"; when: pane.keysOn; onActivated: pane.removeSelected() }

    GraphPalette {
        id: chooser
        browser: graph
        reading: pane.reading
        // Asked a moment after the box has gone, when the keyboard is free again.
        onPicked: (entry) => {
            mergeSoon.entry = entry
            mergeSoon.restart()
        }
        onClosed: if (!sheet.opened) pane.takeFocus()
    }
    Timer {
        id: mergeSoon
        property var entry: null
        interval: 0
        onTriggered: if (entry && pane.hasNode) sheet.show("merge", pane.node, entry)
    }
    GraphEdit {
        id: sheet
        browser: graph
        onClosed: pane.takeFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: theme.background
    }
    // Beside the page, a line where the page ends.
    Rectangle {
        visible: pane.reading
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: theme.colors.selection
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            // The row takes the width it is given: the title and the count
            // give way, and the rest is no wider than the narrowest pane.
            Layout.minimumWidth: 0
            Layout.margins: 16
            Layout.bottomMargin: 8
            spacing: 12

            // Back to the shelf, for a pointer or a finger.
            Rectangle {
                objectName: "graphClose"
                readonly property string runs: "graph.close"
                visible: !pane.reading
                // The library's name with the arrow where the pane is wide,
                // the arrow alone where it isn't.
                Layout.preferredWidth: Math.max(closeRow.implicitWidth + 16, height)
                Layout.preferredHeight: win.touch ? 40 : 30
                radius: theme.radius
                color: closeHover.hovered ? theme.colors.selection : "transparent"
                Row {
                    id: closeRow
                    anchors.centerIn: parent
                    spacing: 4
                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        name: win.rightToLeft ? "forward" : "back"
                        size: 14
                        color: theme.colors.muted
                    }
                    Label {
                        visible: !pane.compact
                        text: backend.libraryName
                        color: theme.colors.muted
                    }
                }
                HoverHandler { id: closeHover }
                TapHandler { onTapped: pane.done() }
            }
            Label {
                objectName: "graphTitle"
                Layout.fillWidth: true
                Layout.minimumWidth: Math.min(implicitWidth, 40)
                Layout.maximumWidth: Math.min(Math.ceil(implicitWidth), pane.width * (pane.compact ? 0.4 : 0.3))
                text: pane.reading ? qsTr("Known so far")
                      : graph.showing === "book" ? graph.bookTitle
                      : graph.showing === "series" ? graph.series : graph.showing === "collection" ? graph.collection
                      : words.scope(graph.scope)
                font.pixelSize: pane.reading ? 16 : 20
                font.bold: true
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
            Label {
                objectName: "graphCount"
                Layout.fillWidth: true
                // "38 things · 52 relations", or "12 of 38" when some are left out.
                text: {
                    translator.language
                    if (graph.knownCount === 0) return ""
                    if (graph.nodeCount < graph.knownCount)
                        return qsTr("%1 of %2").arg(graph.nodeCount.toLocaleString(Qt.locale(), 'f', 0))
                                               .arg(graph.knownCount.toLocaleString(Qt.locale(), 'f', 0))
                    return qsTr("%Ln things", "", graph.nodeCount) + (pane.compact ? "" : " · " + qsTr("%Ln relations", "", graph.edgeCount))
                }
                color: theme.colors.muted
                elide: Text.ElideRight
                // Whole or not at all: a count cut to "4…" says nothing,
                // and the kinds under it carry the same numbers.
                opacity: width >= implicitWidth - 1 ? 1 : 0
            }
            // Some of what is known lies past the place the book was left.
            AppButton {
                objectName: "graphShowWhole"
                visible: !pane.reading && graph.knownCount > 0 && graph.hiddenCount > 0
                focusPolicy: Qt.NoFocus
                text: qsTr("%Ln more past your page", "", graph.hiddenCount)
                onClicked: pane.askingWhole = !pane.askingWhole
            }
            // The one sign that the graph is being worked on: the AI noting
            // what was read, or a large picture still finding its shape.
            Thinking {
                objectName: "graphBusy"
                active: (pane.growing && graph.knownCount > 0) || pane.arranging
                text: pane.growing && graph.knownCount > 0 ? qsTr("Noting what you have read…") : qsTr("Arranging…")
            }
            // How far around the centre is shown.
            Label {
                visible: !pane.compact && graph.knownCount > 0
                text: words.reach(graph.depth)
                color: theme.colors.muted
            }
            Slider {
                id: reach
                objectName: "graphReach"
                readonly property string runs: "graph.further graph.nearer"
                visible: !pane.compact && graph.knownCount > 0
                Layout.preferredWidth: 110
                focusPolicy: Qt.NoFocus
                from: 0
                to: 4
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: graph.depth
                onMoved: graph.depth = Math.round(value)
                Accessible.name: qsTr("How far around the selected thing is shown")
            }
            // The picture or the list.
            Row {
                spacing: 2
                Repeater {
                    model: ["graph", "list"]
                    delegate: Rectangle {
                        id: face
                        required property string modelData
                        readonly property bool current: (modelData === "list") === pane.listing
                        objectName: "graphAs-" + modelData
                        readonly property string runs: "graph.list"
                        width: win.touch ? 40 : 30
                        height: win.touch ? 40 : 30
                        radius: theme.radius
                        color: current ? theme.colors.selection : "transparent"
                        Glyph {
                            anchors.centerIn: parent
                            name: face.modelData
                            size: 18
                            color: face.current || faceHover.hovered ? theme.foreground : theme.colors.muted
                        }
                        HoverHandler { id: faceHover }
                        TapHandler {
                            onTapped: {
                                pane.listing = face.modelData === "list"
                                pane.takeFocus()
                            }
                        }
                        ToolTip.visible: faceHover.hovered
                        ToolTip.delay: 600
                        ToolTip.text: (face.modelData === "list" ? qsTr("List") : qsTr("Picture")) + "  " + pane.keyOf("graph.list")
                    }
                }
            }
            // Everything else the graph does, each with its key, for a
            // pointer and a finger: what the keys do that has no button.
            HeaderChoice {
                objectName: "graphMore"
                worded: false
                glyph: "more"
                text: qsTr("More")
                onTapped: {
                    pane.takeFocus()
                    win.showMenu("graph", undefined, pane.listing ? rows : canvas)
                }
            }
            Rectangle {
                objectName: "graphHide"
                readonly property string runs: "graph.close"
                visible: pane.reading
                width: win.touch ? 40 : 30
                height: win.touch ? 40 : 30
                radius: theme.radius
                color: hideHover.hovered ? theme.colors.selection : "transparent"
                Glyph {
                    anchors.centerIn: parent
                    name: "cross"
                    size: 14
                    color: theme.colors.muted
                }
                HoverHandler { id: hideHover }
                TapHandler { onTapped: pane.done() }
            }
        }

        // What the keys do, as buttons, where fingers are the keyboard. A row
        // of their own under the header, which wraps: beside the title they
        // ran off a narrow pane, in any language.
        Flow {
            objectName: "graphTouchBar"
            visible: win.touch
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            spacing: 8
            AppButton { objectName: "graphTouchFind"; runs: "graph.find"; text: qsTr("Find"); focusPolicy: Qt.NoFocus; onClicked: chooser.show("find") }
            AppButton { objectName: "graphTouchFilter"; runs: "graph.filter"; text: qsTr("Filter"); focusPolicy: Qt.NoFocus; onClicked: chooser.show("filter") }
            AppButton { objectName: "graphTouchAdd"; runs: "graph.add"; text: qsTr("Add"); focusPolicy: Qt.NoFocus; onClicked: sheet.show("add", pane.node) }
        }

        // Before what is known past the page is shown: where that is, and
        // the choice, under the header rather than over the picture.
        ColumnLayout {
            objectName: "graphWholeAsk"
            visible: pane.askingWhole && graph.knownCount > 0 && graph.hiddenCount > 0
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            spacing: 6
            Label {
                objectName: "graphWholeWarning"
                Layout.fillWidth: true
                text: pane.wholeWarning
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                AppButton {
                    id: wholeConfirm
                    objectName: "graphWholeConfirm"
                    primary: true
                    text: qsTr("Show Everything Known")
                    onClicked: {
                        pane.askingWhole = false
                        graph.showWhole()
                    }
                }
                AppButton {
                    objectName: "graphWholeCancel"
                    text: qsTr("Not Now")
                    onClicked: {
                        pane.askingWhole = false
                        pane.takeFocus()
                    }
                }
            }
        }

        // The kinds there are: the picture's legend, and each a switch that
        // leaves its kind out. After them, what else narrows the graph,
        // each taken off again with a click.
        Flow {
            id: legend
            objectName: "graphKinds"
            visible: graph.knownCount > 0
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            spacing: 6

            Repeater {
                model: graph.kinds
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    objectName: "graphKind-" + modelData.kind
                    readonly property bool on: modelData.shown
                    width: chipRow.implicitWidth + 18
                    height: win.touch ? 38 : 28
                    radius: theme.radius
                    color: chipHover.hovered ? theme.colors.selection : "transparent"
                    border.color: theme.colors.selection
                    opacity: on ? 1 : 0.45
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: words.kinds(modelData.kind)
                    Accessible.checked: on

                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 6
                        GraphKind {
                            anchors.verticalCenter: parent.verticalCenter
                            kind: chip.modelData.kind
                            size: 16
                        }
                        // Where room is short the mark and the count say it;
                        // the name is a pointer's rest away.
                        Label {
                            visible: !pane.compact
                            anchors.verticalCenter: parent.verticalCenter
                            text: words.kinds(chip.modelData.kind)
                            font.pixelSize: 13
                            font.strikeout: !chip.on
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: chip.modelData.count.toLocaleString(Qt.locale(), 'f', 0)
                            color: Qt.alpha(theme.foreground, 0.6)
                            font.pixelSize: 12
                        }
                    }
                    HoverHandler { id: chipHover }
                    TapHandler { onTapped: graph.setKindShown(chip.modelData.kind, !chip.on) }
                    ToolTip.visible: pane.compact && chipHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: words.kinds(modelData.kind)
                }
            }
            // What the book connects to in other books: the ring around the
            // picture, switched like a kind.
            Rectangle {
                id: beyondChip
                objectName: "graphBeyond"
                readonly property string runs: "graph.beyond"
                visible: graph.showing === "book" && graph.beyondCount > 0
                width: beyondRow.implicitWidth + 18
                height: win.touch ? 38 : 28
                radius: theme.radius
                color: beyondHover.hovered ? theme.colors.selection : "transparent"
                border.color: theme.colors.selection
                opacity: graph.beyond ? 1 : 0.45
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("In other books")
                Accessible.checked: graph.beyond

                Row {
                    id: beyondRow
                    anchors.centerIn: parent
                    spacing: 6
                    // The dashed line such a connection is drawn with.
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Repeater {
                            model: 3
                            delegate: Rectangle {
                                width: 4
                                height: 2
                                color: theme.accent
                            }
                        }
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: pane.compact ? qsTr("Other books") : qsTr("In other books")
                        font.pixelSize: 13
                        font.strikeout: !graph.beyond
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: graph.beyondCount.toLocaleString(Qt.locale(), 'f', 0)
                        color: Qt.alpha(theme.foreground, 0.6)
                        font.pixelSize: 12
                    }
                }
                HoverHandler { id: beyondHover }
                TapHandler { onTapped: graph.beyond = !graph.beyond }
            }
            Repeater {
                model: {
                    const chips = []
                    if (!pane.reading && (graph.showing !== "scope" || graph.scope !== ""))
                        chips.push({ name: "whole", label: qsTr("Everything read") })
                    if (graph.onlyAcross && graph.showing === "scope") chips.push({ name: "across", label: qsTr("Between books") })
                    if (graph.onlyAsked) chips.push({ name: "asked", label: qsTr("Asked about") })
                    if (graph.depth > 0) chips.push({ name: "reach", label: words.reach(graph.depth) })
                    return chips
                }
                delegate: Rectangle {
                    id: narrow
                    required property var modelData
                    objectName: "graphChip-" + modelData.name
                    width: narrowRow.implicitWidth + 18
                    height: win.touch ? 38 : 28
                    radius: theme.radius
                    color: narrowHover.hovered ? theme.colors.selection : "transparent"
                    border.color: theme.accent

                    Row {
                        id: narrowRow
                        anchors.centerIn: parent
                        spacing: 6
                        // Going back out to everything is a way back, not a filter taken off.
                        Glyph {
                            visible: narrow.modelData.name === "whole"
                            anchors.verticalCenter: parent.verticalCenter
                            name: win.rightToLeft ? "forward" : "back"
                            size: 12
                            color: Qt.alpha(theme.foreground, 0.6)
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: narrow.modelData.label
                            font.pixelSize: 13
                        }
                        Glyph {
                            visible: narrow.modelData.name !== "whole"
                            anchors.verticalCenter: parent.verticalCenter
                            name: "cross"
                            size: 12
                            color: Qt.alpha(theme.foreground, 0.6)
                        }
                    }
                    HoverHandler { id: narrowHover }
                    TapHandler {
                        onTapped: {
                            if (narrow.modelData.name === "whole") graph.showScope("")
                            else if (narrow.modelData.name === "asked") graph.onlyAsked = false
                            else if (narrow.modelData.name === "across") graph.onlyAcross = false
                            else graph.depth = 0
                        }
                    }
                }
            }
        }

        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Where the details would take the picture's place, they wait to
            // be asked for: by a tap on a thing, or by the key for them.
            readonly property bool detailed: pane.showDetails && pane.hasNode && (!either || pane.peek)
            // Beside the picture where there is room, under it where there isn't.
            readonly property real sideWidth: detailed && !pane.compact ? detailsSide.implicitWidth : 0
            // In a window too low for both, one thing at a time: the details
            // take the picture's place until they are closed.
            readonly property bool either: pane.compact && height < pane.pictureFloor
            readonly property real underHeight: !detailed || !pane.compact ? 0
                                                : either ? height : Math.min(Math.round(height * 0.48), 280)

            GraphCanvas {
                id: canvas
                objectName: "graphCanvas"
                // A tap selects what it lands on and a drag moves the view,
                // which is what the arrow keys and their Shift do; the
                // wheel and a pinch zoom.
                readonly property string runs: "graph.left graph.right graph.up graph.down graph.pan_left graph.pan_right "
                                               + "graph.pan_up graph.pan_down graph.zoom_in graph.zoom_out"
                anchors.fill: parent
                anchors.rightMargin: body.sideWidth
                anchors.bottomMargin: body.underHeight
                // Up even when there is nothing to draw, so the keys have somewhere to be.
                visible: !pane.listing
                focus: !pane.listing
                browser: graph
                colors: theme.colors
                foreground: theme.foreground
                background: theme.background
                accent: theme.accent
                Accessible.role: Accessible.Graphic
                Accessible.name: qsTr("The graph as a picture")

                HoverHandler {
                    id: pointer
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    cursorShape: canvas.hovered > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onPointChanged: canvas.hovered = canvas.nodeAt(point.position.x, point.position.y)
                    onHoveredChanged: if (!hovered) canvas.hovered = 0
                }
                // A tap selects, a second one looks around what it selected.
                TapHandler {
                    onTapped: (point) => {
                        canvas.forceActiveFocus()
                        graph.select(canvas.nodeAt(point.position.x, point.position.y, win.touch ? 16 : 4))
                        pane.peek = pane.hasNode
                    }
                    onDoubleTapped: (point) => {
                        const hit = canvas.nodeAt(point.position.x, point.position.y, win.touch ? 16 : 4)
                        if (hit <= 0) return
                        graph.select(hit)
                        graph.recentre()
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
                    onWheel: (event) => canvas.zoomBy(Math.pow(1.0018, event.angleDelta.y), event.x, event.y)
                }
                PinchHandler {
                    target: null
                    onScaleChanged: (delta) => canvas.zoomBy(delta, centroid.position.x, centroid.position.y)
                }
            }

            // The same nodes as rows, by kind.
            ListView {
                id: rows
                objectName: "graphList"
                anchors.fill: parent
                anchors.rightMargin: body.sideWidth
                // The line about the keys stands under the list, not on it.
                anchors.bottomMargin: body.underHeight + (hint.visible ? 36 : 0)
                anchors.leftMargin: 8
                visible: pane.listing && graph.nodeCount > 0
                focus: pane.listing
                clip: true
                // Each kind's heading, then its nodes.
                readonly property var entries: {
                    const entries = []
                    let kind = null
                    for (const row of graph.rows) {
                        // What is in other books comes last, under one heading.
                        const group = row.outer ? "" : row.kind
                        if (group !== kind) {
                            kind = group
                            entries.push({ heading: true, kind: group })
                        }
                        entries.push(row)
                    }
                    return entries
                }
                readonly property int selectedIndex: entries.findIndex(entry => !entry.heading && entry.id === graph.selected)
                model: entries
                onSelectedIndexChanged: if (selectedIndex >= 0) positionViewAtIndex(selectedIndex, ListView.Contain)
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: entry
                    required property int index
                    required property var modelData
                    readonly property bool heading: !!modelData.heading
                    readonly property bool current: !heading && modelData.id === graph.selected
                    objectName: heading ? "graphHeading" : "graphRow"
                    width: rows.width - 8
                    height: heading ? 34 : (win.touch ? 46 : 34)
                    Accessible.role: heading ? Accessible.Heading : Accessible.ListItem
                    Accessible.name: !heading ? modelData.name : modelData.kind === "" ? qsTr("In other books") : words.kinds(modelData.kind)

                    Label {
                        visible: entry.heading
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 6
                        text: !entry.heading ? "" : entry.modelData.kind === "" ? qsTr("In other books") : words.kinds(entry.modelData.kind)
                        color: theme.colors.muted
                        font.pixelSize: 12
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: entry.current
                        radius: Math.min(4, theme.radius)
                        color: theme.colors.selection
                    }
                    RowLayout {
                        visible: !entry.heading
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 10
                        spacing: 10
                        GraphKind {
                            kind: entry.modelData.kind
                            size: 16
                        }
                        Label {
                            Layout.maximumWidth: entry.width * 0.45
                            text: entry.heading ? "" : entry.modelData.name
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                        Label {
                            Layout.fillWidth: true
                            // A thing of another book says which, before what it is.
                            text: entry.heading ? "" : entry.modelData.outer && entry.modelData.from !== ""
                                                       ? entry.modelData.from + (entry.modelData.summary !== "" ? " · " + entry.modelData.summary : "")
                                                       : entry.modelData.summary
                            color: theme.colors.muted
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                        Label {
                            visible: !entry.heading && entry.modelData.links > 0
                            text: entry.heading ? "" : entry.modelData.links.toLocaleString(Qt.locale(), 'f', 0)
                            color: Qt.alpha(theme.foreground, 0.6)
                            font.pixelSize: 12
                        }
                    }
                    TapHandler {
                        enabled: !entry.heading
                        onTapped: {
                            rows.forceActiveFocus()
                            graph.select(entry.modelData.id)
                            pane.peek = true
                        }
                        onDoubleTapped: {
                            graph.select(entry.modelData.id)
                            graph.recentre()
                        }
                    }
                }
            }

            // What the keys do here, said once, quietly, under the picture.
            Label {
                id: hint
                objectName: "graphHint"
                visible: !win.touch && !pane.compact && graph.nodeCount > 0
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.right: canvas.right
                anchors.margins: 16
                text: pane.hasNode
                      ? qsTr("%1 looks around it · %2 goes back · %3 edits it · %4 opens it")
                            .arg(pane.keyOf("graph.expand")).arg(pane.keyOf("graph.back")).arg(pane.keyOf("graph.edit")).arg(pane.keyOf("graph.open"))
                      : qsTr("The arrow keys walk the relations · %1 finds a thing · %2 filters · %3 adds one")
                            .arg(pane.keyOf("graph.find")).arg(pane.keyOf("graph.filter")).arg(pane.keyOf("graph.add"))
                color: theme.colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            // The library's graph before anything was read into it.
            Label {
                objectName: "graphGrows"
                visible: !pane.reading && graph.showing === "scope" && graph.scope === "" && graph.knownCount > 0 && !graph.storied
                         && !pane.listing
                         // Over a picture with room under it, not on top of a small one.
                         && canvas.height >= pane.pictureFloor
                anchors.horizontalCenter: canvas.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 4
                width: Math.min(canvas.width - 32, 560)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                // No more of it than a small picture has room for over itself.
                maximumLineCount: Math.max(1, Math.min(4, Math.floor((canvas.height - 8) / 18)))
                elide: Text.ElideRight
                color: theme.colors.muted
                text: qsTr("These are your books, their authors, series and tags. The people, places and ideas in them join as you read, with the knowledge graph switched on in the settings.")
            }

            // Nothing to show says why, and how the graph comes to be: in
            // the middle of the pane, and scrolling where a low or narrow
            // one has less room than the words and the buttons take.
            Flickable {
                id: emptyScroller
                anchors.fill: parent
                visible: graph.nodeCount === 0
                contentHeight: emptyColumn.height + 24
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height
                ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: emptyColumn
                objectName: "graphEmpty"
                x: (emptyScroller.width - width) / 2
                y: Math.max(12, (emptyScroller.height - height) / 2)
                width: Math.min(emptyScroller.width - 32, 460)
                spacing: 12

                Label {
                    objectName: "graphEmptyTitle"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    font.pixelSize: pane.compact ? 18 : 22
                    text: graph.knownCount > 0 ? qsTr("Nothing matches")
                          : graph.hiddenCount > 0 ? qsTr("Nothing up to your page")
                          : qsTr("Nothing noted yet")
                }
                Thinking {
                    objectName: "graphEmptyBusy"
                    Layout.alignment: Qt.AlignHCenter
                    active: pane.growing && graph.knownCount === 0
                    text: qsTr("Noting what you have read…")
                }
                // Known, but all of it past where the book was left: said, and
                // offered, rather than looking as if nothing were known.
                Label {
                    objectName: "graphEmptyHidden"
                    Layout.fillWidth: true
                    visible: graph.knownCount === 0 && graph.hiddenCount > 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: theme.colors.muted
                    text: qsTr("%Ln things are known of this book past the place you left it.", "", graph.hiddenCount) + " " + pane.wholeWarning
                }
                Label {
                    objectName: "graphEmptyText"
                    Layout.fillWidth: true
                    visible: graph.knownCount === 0 && graph.hiddenCount === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: theme.colors.muted
                    text: graph.showing === "scope" && graph.scope !== ""
                          ? qsTr("This world has nothing in it yet. %1 adds the first thing.").arg(pane.keyOf("graph.add"))
                          : settings.graph === "off"
                          ? qsTr("Who and what a book is about (its people, places, ideas and the threads of its plot) can be noted here as you read, never past your page. The knowledge graph is switched off; switch it on in the settings.")
                          : !assistant.available
                          ? qsTr("Who and what a book is about (its people, places, ideas and the threads of its plot) is noted here as you read, never past your page. That takes an AI model; set one up in the settings.")
                          : settings.graph === "auto"
                          ? qsTr("Who and what a book is about (its people, places, ideas and the threads of its plot) is noted here as you finish its chapters, never past your page.")
                          : qsTr("Who and what a book is about (its people, places, ideas and the threads of its plot) is noted here when you ask for it, never past your page.")
                }
                Label {
                    Layout.fillWidth: true
                    visible: graph.knownCount === 0 && !(graph.showing === "scope" && graph.scope !== "")
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: theme.colors.muted
                    text: qsTr("%1 adds something by hand.").arg(pane.keyOf("graph.add"))
                }
                // Side by side where there is room and wrapped where there
                // isn't, in the middle either way, and never wider than the pane.
                DialogActions {
                    centred: true
                    Layout.topMargin: 4
                    AppButton {
                        objectName: "graphGrow"
                        // Reading it in is the AI's work, for the open book.
                        visible: pane.reading && graph.knownCount === 0 && settings.graph !== "off" && assistant.available
                                 && !pane.growing
                        primary: true
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Note what I have read")
                        onClicked: assistant.growGraph()
                    }
                    AppButton {
                        objectName: "graphEmptyWhole"
                        visible: graph.knownCount === 0 && graph.hiddenCount > 0 && !pane.reading
                        primary: true
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Show Everything Known")
                        onClicked: graph.showWhole()
                    }
                    AppButton {
                        objectName: "graphEmptyAdd"
                        visible: graph.knownCount === 0
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Add Something")
                        onClicked: sheet.show("add", pane.node)
                    }
                    AppButton {
                        objectName: "graphShowAll"
                        visible: graph.knownCount > 0
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Show everything again")
                        onClicked: {
                            graph.showEveryKind()
                            graph.depth = 0
                        }
                    }
                }
            }
            }

            // The details are a pane like those beside the shelf and the
            // page: beside the picture their edge drags and their width is
            // remembered. Under the picture they are as wide as the pane.
            SidePane {
                id: detailsSide
                objectName: "graphDetailsSide"
                name: "graph_details"
                standard: 320
                least: 260
                most: 560
                room: body.width - win.contentFloor
                fixed: pane.compact
                lined: false
                visible: body.detailed
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: pane.compact ? parent.width : implicitWidth
                height: pane.compact ? body.underHeight : parent.height

            GraphDetails {
                id: details
                objectName: "graphDetails"
                anchors.fill: parent
                below: pane.compact
                node: pane.node
                onJump: (id) => graph.jumpTo(id)
                onLook: graph.recentre()
                onGo: (place) => pane.go(place)
                onRead: (bookId) => pane.read(bookId)
                onEdit: pane.editSelected()
                onMerge: pane.mergeSelected()
                onRemove: pane.removeSelected()
                onUnlink: (relation) => sheet.show("unlink", pane.node, relation)
                onShowAhead: (id, shown) => graph.showAhead(id, shown)
                onLeave: pane.takeFocus()
                closable: body.either
                onClose: pane.peek = false
            }
            }
        }
    }
}
