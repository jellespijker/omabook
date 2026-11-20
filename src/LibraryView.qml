import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The library: its books shown one of several ways (as covers, as spines on
// a shelf, as a carousel, a list, or in stacks), a search, the filters and
// the order, and the selected book's details beside it. Each way is a file
// of its own, loaded when it is picked; see ShelfCovers.qml for what they
// all answer.
Item {
    id: view
    // Whether single keys are this view's shortcuts right now.
    property bool active: true
    property var selectedId: 0
    // The details beside the shelf, where there is room for them there.
    property bool showDetails: true
    // Whether there is: the pane at its narrowest still leaves the shelf its floor.
    readonly property bool detailsFit: width - detailsSide.least - (sideSlot.visible ? sideSlot.width : 0) >= win.contentFloor
    // The details over the shelf, asked for in a window with no room beside
    // it. They are put away again by the same key, Escape, or a click beside them.
    property bool detailsOver: false
    onDetailsFitChanged: detailsOver = false
    function toggleDetails() {
        if (detailsFit) showDetails = !showDetails
        else detailsOver = !detailsOver
    }
    function resizePane(steps) { if (detailsSide.visible) detailsSide.resize(steps) }
    readonly property var selectedBook: {
        backend.revision
        // A launcher a plugin put on the shelf has a number below zero.
        return selectedId !== 0 ? backend.book(selectedId) : ({})
    }
    readonly property var sorts: ["recent", "title", "author", "series", "added", "progress", "rating", "published", "finished"]
    readonly property var views: ({
        "covers": "ShelfCovers.qml", "spines": "ShelfSpines.qml", "carousel": "ShelfCarousel.qml",
        "list": "ShelfList.qml", "stacks": "ShelfStacks.qml"
    })
    // The view in use; null for the moment one is being swapped for another.
    readonly property var shelf: shelfLoader.item
    // A collection is shown that holds no book at all (not one a search or a filter emptied).
    readonly property bool emptyCollection: backend.libraryError === "" && backend.collection !== 0 && backend.books.total === 0
    // The filters in the order they are always listed in.
    readonly property var filterKinds: ["status", "held", "rating", "format", "language", "tag", "author", "series"].filter(
                                           kind => backend.books.filters[kind] !== undefined)
    readonly property bool popupOpen: chooser.opened

    function takeFocus() { if (shelf) shelf.forceActiveFocus() }
    // The first key a command has now, for text that names one.
    function keyOf(command) { return (custom.keyText[command] || "").split("  ")[0] }
    // Space and Enter: the selected book is read, or the selected stack opened.
    function openSelected() { if (shelf) shelf.activate() }
    function select(id) {
        const row = backend.books.rowOf(id)
        if (row >= 0 && shelf) shelf.show(row)
    }
    function move(where) { if (shelf) shelf.move(where) }
    // Each order starts the way it is most wanted; Shift+S turns it round.
    function nextSort() {
        backend.books.reversed = false
        backend.books.sort = sorts[(sorts.indexOf(backend.books.sort) + 1) % sorts.length]
    }
    function nextView() {
        const names = Object.keys(views)
        backend.view = names[(names.indexOf(backend.view) + 1) % names.length]
    }
    function nextGroup() {
        const kinds = ["series", "author", "tag", "status"]
        backend.group = kinds[(kinds.indexOf(backend.group) + 1) % kinds.length]
    }

    Words { id: words }
    ShelfPalette {
        id: chooser
        // Not from a dialog it opened, which has the keyboard now.
        onClosed: if (!win.dialogOpen) view.takeFocus()
        // What a list leads to: another list, or a dialog.
        onActed: (what) => {
            if (what === "collections") collectionsLater.restart()
            else if (what === "collectionNew") win.newCollection("")
            else if (what === "collectionFromShelf") win.newCollection(backend.shelfRule())
            else if (what === "collectionEdit") win.editCollection(backend.collection)
            else if (what === "collectionSort") win.showCollectionSort()
        }
    }
    // A list opens once the list it was picked from has closed.
    Timer { id: collectionsLater; interval: 0; onTriggered: chooser.show("collections") }

    Key { command: "library.search"; when: view.visible; onActivated: header.openSearch() }
    // A question for the AI: what the search field holds, or one to type.
    Key { command: "library.search_ai"; when: view.visible; onActivated: win.showAiSearch(backend.books.query) }
    // What found the selected book, when the search found it by what is known.
    readonly property var foundBy: {
        backend.books.knownRevision
        return selectedId > 0 ? backend.books.knownFor(selectedId) : ({})
    }
    Key { command: "library.found_graph"; when: view.visible && view.foundBy.node !== undefined; onActivated: win.showFound(view.foundBy) }
    Key { command: "library.found_moment"; when: view.visible && view.foundBy.node !== undefined; onActivated: win.goToFound(view.foundBy) }
    Key { command: "library.add"; when: view.visible; onActivated: backend.addBooks() }
    Key { command: "library.add_folder"; when: view.visible; onActivated: backend.addFolder() }
    Key { command: "library.edit"; when: view.visible; onActivated: win.editBook(view.selectedId) }
    Key { command: "library.new"; when: view.visible; onActivated: win.newBook() }
    Key { command: "library.sites"; when: view.visible; onActivated: win.showReadingSites() }
    Key { command: "library.lookup"; when: view.visible; onActivated: win.lookUpBook(view.selectedId) }
    Key { command: "library.remove"; when: view.visible; onActivated: win.removeBook(view.selectedId) }
    Key { command: "library.export"; when: view.visible && view.selectedId > 0; onActivated: backend.exportBook(view.selectedId) }
    Key { command: "library.sort"; when: view.visible; onActivated: view.nextSort() }
    Key { command: "library.sort_reverse"; when: view.visible; onActivated: backend.books.reversed = !backend.books.reversed }
    Key { command: "library.filter"; when: view.visible; onActivated: chooser.show("filter") }
    Key { command: "library.filter_clear"; when: view.visible; onActivated: backend.books.clearFilters() }
    Key { command: "library.view_covers"; when: view.visible; onActivated: backend.view = "covers" }
    Key { command: "library.view_spines"; when: view.visible; onActivated: backend.view = "spines" }
    Key { command: "library.view_carousel"; when: view.visible; onActivated: backend.view = "carousel" }
    Key { command: "library.view_list"; when: view.visible; onActivated: backend.view = "list" }
    Key { command: "library.view_stacks"; when: view.visible; onActivated: backend.view = "stacks" }
    Key { command: "library.view_next"; when: view.visible; onActivated: view.nextView() }
    Key { command: "library.group"; when: view.visible; onActivated: view.nextGroup() }
    Key { command: "library.libraries"; when: view.visible; onActivated: win.showLibraries() }
    Key { command: "library.collections"; when: view.visible; onActivated: chooser.show("collections") }
    Key { command: "library.collection_next"; when: view.visible; onActivated: backend.stepCollection(1) }
    Key { command: "library.collection_previous"; when: view.visible; onActivated: backend.stepCollection(-1) }
    Key { command: "library.collection_book"; when: view.visible && view.selectedId > 0; onActivated: chooser.show("in", view.selectedId) }
    Key { command: "library.sync"; when: view.visible; onActivated: backend.sync() }
    Key { command: "library.plugins"; when: view.visible; onActivated: win.showPlugins() }
    Key { command: "library.details"; when: view.visible; onActivated: view.toggleDetails() }
    Key { command: "library.tidy"; when: view.visible; onActivated: win.showTidy() }
    Key { command: "library.ask"; when: view.visible; onActivated: win.showAskLibrary() }
    Key { command: "library.suggest"; when: view.visible; onActivated: win.showSuggestions() }
    Key { command: "library.graph"; when: view.visible; onActivated: win.showGraph(0) }
    Key { command: "library.graph_book"; when: view.visible && view.selectedId > 0; onActivated: win.showGraph(view.selectedId) }
    Key { command: "library.graph_build"; when: view.visible; onActivated: win.showGraphBuild(view.selectedId) }
    Key { command: "library.read"; when: view.visible; onActivated: view.openSelected() }
    Key { command: "library.left"; when: view.visible; onActivated: view.move("left") }
    Key { command: "library.right"; when: view.visible; onActivated: view.move("right") }
    Key { command: "library.down"; when: view.visible; onActivated: view.move("down") }
    Key { command: "library.up"; when: view.visible; onActivated: view.move("up") }

    // A reload replaces every row, so the selection is kept by the book, not
    // by its place on the shelf.
    Connections {
        target: backend.books
        property var keep: 0
        function onModelAboutToBeReset() { keep = view.selectedId }
        // Once the shelf itself has taken the new rows in.
        function onModelReset() { Qt.callLater(restore) }
        function restore() {
            if (!view.shelf)
                return
            const row = backend.books.rowOf(keep)
            view.shelf.show(row >= 0 ? row : Math.min(Math.max(0, view.shelf.currentRow), backend.books.count - 1))
            view.selectedId = backend.books.idAt(view.shelf.currentRow)
        }
    }
    // The view says which book is selected and when one is to be opened.
    Connections {
        target: view.shelf
        function onCurrentRowChanged() { view.selectedId = backend.books.idAt(view.shelf.currentRow) }
        // A launcher calls its plugin; the reader is not opened for it.
        function onOpened(bookId) {
            if (bookId < 0) backend.scripts.launch(bookId)
            else reader.openBook(bookId)
        }
    }
    Connections {
        target: backend
        function onBookAdded(id) { view.select(id) }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The header: whose library, how many books, the ways to show
        // them, the filter, the order and the search. It is one row at
        // every width. When the window is too narrow for all of it, it
        // gives things up in this order, each one only once the ones
        // before it were not enough, and takes them back the same way:
        //
        //   1. the count of books goes (the shelf itself says as much);
        //   2. under fingers, the row of buttons goes (the ⋯ beside it lists
        //      them, with everything else);
        //   3. "Filter" and the order become drawings;
        //   4. the ways to show the shelf fold into the one in use, which lists them;
        //   5. the search field becomes a drawing that opens over the header;
        //   last, the library's name elides.
        //
        // Nothing is lost on the way: what folds lists itself when clicked
        // or tapped, and every one of them has its key. Which step it is
        // on is measured, not read off the window's size class: the words
        // are as long as the language makes them, and a width that holds
        // "Filter" does not hold "Filtern" and "Zuletzt gelesen".
        RowLayout {
            id: header
            objectName: "libraryHeader"
            Layout.fillWidth: true
            // The row takes the width it is given, never the width it wants.
            Layout.minimumWidth: 0
            Layout.margins: 16
            Layout.bottomMargin: 8
            spacing: 12

            // What the header needs at a step, with the name at its own
            // width (up to a point: a very long name elides sooner).
            function need(at) {
                let parts = [Math.min(Math.ceil(title.implicitWidth), 200)]
                if (at < 1) parts.push(count.implicitWidth)
                if (backend.busy) parts.push(28)
                if (backend.busy && at < 1) parts.push(Math.min(Math.ceil(status.implicitWidth), 200))
                if (win.touch && at < 2) parts.push(touchBar.implicitWidth)
                parts.push(libraryMore.drawnWidth)
                parts.push(at < 4 ? switcher.implicitWidth : viewChoice.drawnWidth)
                if (!win.touch) parts.push(at < 3 ? filterChoice.wordsWidth : filterChoice.drawnWidth)
                parts.push(at < 3 ? sortChoice.wordsWidth : sortChoice.drawnWidth)
                parts.push(at < 5 ? search.narrowest : searchChoice.drawnWidth)
                // A gap between each and the next, and one more for the
                // empty stretch that pushes the rest to the far side.
                // Each part rounded up, as the row rounds what it gives them.
                return parts.reduce((sum, part) => sum + Math.ceil(part), 0) + parts.length * spacing
            }
            readonly property int step: {
                for (let at = 0; at < 5; ++at)
                    if (need(at) <= width)
                        return at
                return 5
            }
            // The search field, opened over the rest of a header that has
            // no room to keep it out.
            property bool searchOpen: false
            readonly property bool searching: step >= 5 && searchOpen
            function openSearch() {
                searchOpen = true
                search.forceActiveFocus()
            }

            Label {
                id: title
                objectName: "libraryName"
                visible: !header.searching
                Layout.fillWidth: true
                Layout.minimumWidth: Math.min(implicitWidth, 40)
                Layout.maximumWidth: Math.ceil(implicitWidth)
                Layout.preferredHeight: Math.max(implicitHeight, win.touch ? 40 : 0)
                verticalAlignment: Text.AlignVCenter
                text: backend.libraryName
                font.pixelSize: 20
                font.bold: true
                elide: Text.ElideRight
                readonly property string runs: "library.libraries"
                TapHandler { onTapped: win.showLibraries() }
            }
            ShelfCount {
                id: count
                visible: header.step < 1 && !header.searching
                some: backend.books.query !== "" || view.filterKinds.length > 0
            }
            Item {
                visible: !header.searching
                Layout.fillWidth: true
            }
            BusyIndicator {
                visible: backend.busy && !header.searching
                running: backend.busy
                implicitWidth: 28
                implicitHeight: 28
            }
            Label {
                id: status
                visible: backend.busy && header.step < 1 && !header.searching
                Layout.maximumWidth: 200
                text: backend.status
                color: theme.colors.muted
                elide: Text.ElideRight
            }
            // What the keys do, as buttons, where fingers are the keyboard.
            Row {
                id: touchBar
                objectName: "touchBar"
                visible: win.touch && header.step < 2 && !header.searching
                spacing: 8
                AppButton { objectName: "touchAdd"; runs: "library.add"; text: qsTr("Add"); focusPolicy: Qt.NoFocus; onClicked: backend.addBooks() }
                AppButton { objectName: "touchFilter"; runs: "library.filter library.filter_clear"; text: qsTr("Filter"); focusPolicy: Qt.NoFocus; onClicked: chooser.show("filter") }
                AppButton { objectName: "touchSuggest"; runs: "library.suggest"; text: qsTr("Suggest"); focusPolicy: Qt.NoFocus; onClicked: win.showSuggestions() }
                AppButton { runs: "library.libraries"; text: qsTr("Libraries"); focusPolicy: Qt.NoFocus; onClicked: win.showLibraries() }
                AppButton { runs: "global.settings"; text: qsTr("Settings"); focusPolicy: Qt.NoFocus; onClicked: win.showSettings() }
            }
            // Everything else the shelf can do, for a mouse and a finger:
            // what has no button of its own (sync, tidy, plugins, help...),
            // and under fingers what the row above folds away. Each with its key.
            HeaderChoice {
                id: libraryMore
                objectName: "libraryMore"
                visible: !header.searching
                worded: false
                glyph: "more"
                text: qsTr("More")
                onTapped: win.showMenu("library")
            }
            ViewSwitcher {
                id: switcher
                objectName: "viewSwitcher"
                visible: header.step < 4 && !header.searching
            }
            // The way in use, which lists the others.
            HeaderChoice {
                id: viewChoice
                objectName: "viewChoice"
                visible: header.step >= 4 && !header.searching
                worded: false
                glyph: view.views[backend.view] ? backend.view : "covers"
                mark: "down"
                text: qsTr("Show as")
                keys: view.keyOf("library.view_next")
                runs: "library.view_next library.view_covers library.view_spines library.view_carousel library.view_list library.view_stacks"
                onTapped: chooser.show("view")
            }
            HeaderChoice {
                id: filterChoice
                objectName: "filterLabel"
                visible: !win.touch && !header.searching
                worded: header.step < 3
                glyph: "filter"
                lit: !worded && view.filterKinds.length > 0
                text: qsTr("Filter")
                keys: view.keyOf("library.filter")
                // The list it opens takes every filter off too.
                runs: "library.filter library.filter_clear"
                onTapped: chooser.show("filter")
            }
            // The order, and which way it runs; a click lists the others.
            HeaderChoice {
                id: sortChoice
                objectName: "sort"
                wordsName: "sortName"
                markName: "sortWay"
                visible: !header.searching
                worded: header.step < 3
                glyph: "sort"
                mark: backend.books.descending ? "down" : "up"
                text: words.sort(backend.books.sort)
                keys: view.keyOf("library.sort")
                // The list it opens turns the order round too.
                runs: "library.sort library.sort_reverse"
                onTapped: chooser.show("sort")
            }
            HeaderChoice {
                id: searchChoice
                objectName: "searchChoice"
                visible: header.step >= 5 && !header.searching
                worded: false
                glyph: "search"
                // A search that is narrowing the shelf shows, though its words don't.
                lit: backend.books.query !== ""
                text: qsTr("Search")
                keys: view.keyOf("library.search")
                runs: "library.search"
                onTapped: header.openSearch()
            }
            TextField {
                id: search
                objectName: "search"
                // The least it is of use at; narrower than this it folds.
                readonly property real narrowest: 140
                readonly property string runs: "library.search"
                visible: header.step < 5 || header.searching
                // As wide as it likes where there is room, and otherwise
                // what is over once everything else has its width; the
                // name gives way only after the field is at its narrowest.
                Layout.fillWidth: header.searching
                Layout.preferredWidth: Math.max(narrowest, Math.min(260, narrowest + header.width - header.need(header.step)))
                Layout.minimumWidth: narrowest
                placeholderText: qsTr("Search")
                text: backend.books.query
                selectByMouse: true
                Material.roundedScale: theme.radius
                onTextEdited: backend.books.query = text
                // Opened over the header, it closes when it is left.
                onActiveFocusChanged: if (!activeFocus) header.searchOpen = false
                Keys.onEscapePressed: {
                    backend.books.query = ""
                    view.takeFocus()
                }
                // Words that start with "?" are a question for the AI.
                Keys.onReturnPressed: {
                    if (text.trim().startsWith("?")) win.showAiSearch(text)
                    else view.takeFocus()
                }
                Keys.onDownPressed: view.takeFocus()
                // Room for the AI button at the field's end, whichever way the text runs.
                readonly property real askRoom: askAi.visible ? askAi.width + 4 : 0
                rightPadding: Material.textFieldHorizontalPadding + (win.rightToLeft ? 0 : askRoom)
                leftPadding: Material.textFieldHorizontalPadding + (win.rightToLeft ? askRoom : 0)
                // The words asked of the AI instead, for a mouse and a finger.
                Rectangle {
                    id: askAi
                    objectName: "searchAskAi"
                    readonly property string runs: "library.search_ai"
                    visible: librarian.hasAi
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 36
                    height: 36
                    radius: theme.radius
                    color: askHover.hovered ? theme.colors.selection : "transparent"
                    Label {
                        anchors.centerIn: parent
                        text: qsTr("AI")
                        font.pixelSize: 12
                        font.bold: true
                        color: search.text.trim().startsWith("?") ? theme.accent : theme.colors.muted
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Ask the AI to find books")
                    Accessible.onPressAction: win.showAiSearch(search.text)
                    HoverHandler { id: askHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: win.showAiSearch(search.text) }
                    ToolTip.visible: askHover.hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Ask the AI to find books") + "  " + view.keyOf("library.search_ai")
                }
            }
        }

        // Words that start with "?" don't narrow the shelf: they are a
        // question, and this says what Enter does with it.
        Label {
            objectName: "askHint"
            visible: backend.books.query.trim().startsWith("?")
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            text: librarian.hasAi ? qsTr("Enter asks the AI to find books for this question.") : librarian.aiProblem
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }

        // The collections, as tabs, once the library has any: which part of
        // it the shelf shows. Each keeps its own view and order.
        CollectionStrip {
            id: collectionStrip
            visible: backend.collections.length > 0
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 6
            onMore: chooser.show("collections")
            onCreate: (parent) => win.newCollection("", parent)
            onEdit: (id) => win.editCollection(id)
        }
        // Opened, a collection with others inside it shows them in a row
        // under it; inside one of them, the row stays, for its neighbours.
        CollectionStrip {
            id: innerStrip
            objectName: "collectionInnerStrip"
            readonly property var shown: backend.collections.find(c => c.id === backend.collection)
            parentId: !shown ? 0 : shown.children ? shown.id : shown.parent
            visible: parentId !== 0
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.leftMargin: 20
            Layout.rightMargin: 12
            Layout.bottomMargin: 6
            onMore: chooser.show("collections")
            onCreate: (parent) => win.newCollection("", parent)
            onEdit: (id) => win.editCollection(id)
        }

        // What narrows the shelf, each taken off again with a click. The
        // chips wrap onto a second row (one row in a low window); those
        // that still don't fit are counted in a last chip, "+3", which
        // opens the list of filters where each can be taken off.
        Flow {
            id: chips
            objectName: "chips"
            // Asked of what makes the search chip show, not of whether it
            // does: nothing in a row that is hidden shows.
            readonly property bool searchShown: header.step >= 5 && backend.books.query !== "" && !header.searching
            visible: view.filterKinds.length > 0 || searchShown
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 8
            spacing: 8

            readonly property int rows: win.low ? 1 : 2
            readonly property real chipHeight: win.touch ? 38 : 28
            readonly property real moreWidth: 48
            // The widest a chip gets: a row, less the room "+3" may need.
            readonly property real widest: Math.max(80, width - moreWidth - spacing)
            FontMetrics { id: kindFont; font.pixelSize: 12 }
            FontMetrics { id: valueFont; font.pixelSize: 13 }
            function chipWidth(kind) {
                return Math.min(widest, Math.ceil(kindFont.advanceWidth(words.kind(kind))
                                                  + valueFont.advanceWidth(words.value(kind, backend.books.filters[kind]))) + 12 + 12 + 20 + 2)
            }
            // How many of the chips fit in the rows there are, leaving room
            // on the last row for "+3" unless every one of them fits.
            readonly property int fitting: {
                translator.language
                const kinds = view.filterKinds
                let across = searchShown ? searchChip.width + spacing : 0, row = 1, shown = 0
                for (; shown < kinds.length; ++shown) {
                    const wide = chipWidth(kinds[shown])
                    if (across > 0 && across + wide > width) {
                        ++row
                        across = 0
                    }
                    if (row > rows)
                        break
                    if (row === rows && shown < kinds.length - 1 && across + wide + spacing + moreWidth > width)
                        break
                    across += wide + spacing
                }
                return shown
            }

            // The search, where the header has folded its field away.
            Rectangle {
                id: searchChip
                objectName: "chip-search"
                visible: chips.searchShown
                width: Math.min(chips.widest, searchRow.implicitWidth + 20)
                height: chips.chipHeight
                radius: theme.radius
                color: searchHover.hovered ? theme.colors.selection : "transparent"
                border.color: theme.colors.selection
                Row {
                    id: searchRow
                    anchors.centerIn: parent
                    spacing: 6
                    Glyph { name: "search"; size: 12; color: Qt.alpha(theme.foreground, 0.6); anchors.verticalCenter: parent.verticalCenter }
                    Label {
                        text: backend.books.query
                        font.pixelSize: 13
                        width: Math.min(implicitWidth, chips.widest - 20 - 36)
                        elide: Text.ElideRight
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Glyph { name: "cross"; size: 12; color: Qt.alpha(theme.foreground, 0.6); anchors.verticalCenter: parent.verticalCenter }
                }
                HoverHandler { id: searchHover }
                TapHandler { onTapped: backend.books.query = "" }
            }
            Repeater {
                model: view.filterKinds.slice(0, chips.fitting)
                delegate: Rectangle {
                    id: chip
                    required property string modelData
                    objectName: "chip-" + modelData
                    width: Math.min(chips.widest, chipRow.implicitWidth + 20)
                    height: chips.chipHeight
                    radius: theme.radius
                    color: chipHover.hovered ? theme.colors.selection : "transparent"
                    border.color: theme.colors.selection

                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 6
                        Label {
                            id: chipKind
                            text: words.kind(chip.modelData)
                            color: Qt.alpha(theme.foreground, 0.6)
                            font.pixelSize: 12
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            objectName: "chipValue"
                            text: words.value(chip.modelData, backend.books.filters[chip.modelData])
                            font.pixelSize: 13
                            // A long author or tag elides inside its chip.
                            width: Math.min(implicitWidth, Math.max(24, chips.widest - 20 - chipKind.width - 12 - 12))
                            elide: Text.ElideRight
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Glyph {
                            name: "cross"
                            size: 12
                            color: Qt.alpha(theme.foreground, 0.6)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    HoverHandler { id: chipHover }
                    TapHandler { onTapped: backend.books.setFilter(chip.modelData, "") }
                }
            }
            Rectangle {
                objectName: "chipsMore"
                visible: chips.fitting < view.filterKinds.length
                width: chips.moreWidth
                height: chips.chipHeight
                radius: theme.radius
                color: moreHover.hovered ? theme.colors.selection : "transparent"
                border.color: theme.colors.selection
                Label {
                    anchors.centerIn: parent
                    // "+3": the filters that are on and have no chip of their own.
                    text: qsTr("+%1").arg((view.filterKinds.length - chips.fitting).toLocaleString(Qt.locale(), 'f', 0))
                    font.pixelSize: 13
                }
                HoverHandler { id: moreHover }
                TapHandler { onTapped: chooser.show("filter") }
            }
        }

        // The shelf, and beside it or over it the selected book's details.
        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true

            Item {
                id: shelfHost
                anchors.fill: parent
                anchors.rightMargin: (sideSlot.visible ? sideSlot.width : 0) + (detailsSide.visible && !detailsSide.over ? detailsSide.width : 0)

                Loader {
                    id: shelfLoader
                    objectName: "shelfLoader"
                    anchors.fill: parent
                    focus: true
                    source: view.views[backend.view] || view.views.covers
                    // The new view starts on the book the last one was on.
                    onLoaded: {
                        const row = backend.books.rowOf(view.selectedId)
                        item.show(row >= 0 ? row : 0)
                        view.selectedId = backend.books.idAt(item.currentRow)
                        item.focus = true
                        // Not from under the page of a book being read.
                        if (!win.typing && !win.inReader)
                            item.forceActiveFocus()
                    }
                    Keys.onReturnPressed: view.openSelected()
                    Keys.onEnterPressed: view.openSelected()
                    Keys.onLeftPressed: view.move("left")
                    Keys.onRightPressed: view.move("right")
                    Keys.onUpPressed: view.move("up")
                    Keys.onDownPressed: view.move("down")
                    // Escape puts away the details lying over the shelf; it and
                    // Backspace step back out of an open stack.
                    Keys.onEscapePressed: (event) => {
                        if (view.detailsOver) {
                            view.detailsOver = false
                            event.accepted = true
                        } else {
                            event.accepted = !!view.shelf && view.shelf.back()
                        }
                    }
                    Keys.onBackPressed: (event) => { event.accepted = !!view.shelf && view.shelf.back() }
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Backspace)
                            event.accepted = !!view.shelf && view.shelf.back()
                    }
                }

                // An empty shelf says how to fill it; an empty search says so.
                // In the middle of the shelf's place, and scrolling where a
                // low window has less room than the words and buttons take.
                Flickable {
                    id: emptyScroller
                    anchors.fill: parent
                    visible: backend.books.count === 0
                    contentHeight: emptyState.height + 32
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: contentHeight > height
                    ScrollBar.vertical: ScrollBar {}

                    ColumnLayout {
                        id: emptyState
                        objectName: "emptyState"
                        x: (emptyScroller.width - width) / 2
                        y: Math.max(16, (emptyScroller.height - height) / 2)
                        width: Math.min(emptyScroller.width - 32, 460)
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            font.pixelSize: 22
                            text: backend.libraryError !== "" ? backend.libraryError
                                  : view.emptyCollection ? qsTr("Nothing is in %1 yet").arg(backend.collectionName)
                                  : backend.books.total === 0 ? qsTr("No books yet")
                                  : qsTr("No book matches")
                        }
                        // A collection that holds nothing says how it is filled.
                        Label {
                            objectName: "emptyCollection"
                            Layout.fillWidth: true
                            visible: view.emptyCollection
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            color: theme.colors.muted
                            text: qsTr("Its rule finds no book. Change the rule, put a book in with %1, or have the AI sort the books that are in no collection.")
                                  .arg(view.keyOf("library.collection_book"))
                        }
                        Flow {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.maximumWidth: parent.width
                            visible: view.emptyCollection
                            spacing: 8
                            AppButton { objectName: "emptyCollectionEdit"; primary: true; focusPolicy: Qt.NoFocus; text: qsTr("Change the collection"); onClicked: win.editCollection(backend.collection) }
                            AppButton {
                                objectName: "emptyCollectionSort"
                                visible: librarian.hasAi
                                focusPolicy: Qt.NoFocus
                                text: qsTr("Sort with AI")
                                onClicked: win.showCollectionSort()
                            }
                            AppButton { objectName: "emptyCollectionAll"; focusPolicy: Qt.NoFocus; text: qsTr("Show all books"); onClicked: backend.collection = 0 }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: backend.libraryError === "" && backend.books.total === 0 && !view.emptyCollection
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            color: theme.colors.muted
                            // The keys are named as they are bound now.
                            text: qsTr("Press %1 to add books or %2 for a folder of them, or drop them on the window. They are copied to %3; your own files are not touched.")
                                  .arg(view.keyOf("library.add")).arg(view.keyOf("library.add_folder")).arg(backend.libraryPath)
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: backend.libraryError === "" && backend.books.total === 0 && !view.emptyCollection
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            color: theme.colors.muted
                            text: qsTr("%1 adds a book you own on paper, or one you want to read. A Calibre library comes across whole, with what you have read. %2 shows every key.")
                                  .arg(view.keyOf("library.new")).arg(view.keyOf("global.help"))
                        }
                        // Every way in, for a mouse or a finger too.
                        Flow {
                            objectName: "emptyActions"
                            Layout.alignment: Qt.AlignHCenter
                            Layout.maximumWidth: parent.width
                            Layout.topMargin: 4
                            visible: backend.libraryError === "" && backend.books.total === 0 && !view.emptyCollection
                            spacing: 8
                            AppButton { objectName: "emptyAdd"; primary: true; focusPolicy: Qt.NoFocus; text: qsTr("Add Books"); onClicked: backend.addBooks() }
                            AppButton { objectName: "emptyFolder"; focusPolicy: Qt.NoFocus; text: qsTr("Add a Folder"); onClicked: backend.addFolder() }
                            AppButton {
                                objectName: "emptyCalibre"
                                // Only where a Calibre library was found to import.
                                visible: backend.calibreLibraries.length > 0
                                focusPolicy: Qt.NoFocus
                                text: qsTr("Import from Calibre")
                                onClicked: win.showLibraries()
                            }
                            AppButton { objectName: "emptyPaper"; focusPolicy: Qt.NoFocus; text: qsTr("Add a Paper Book"); onClicked: win.newBook() }
                        }
                        // Words the search finds nothing for may be a question.
                        AppButton {
                            objectName: "emptyAskAi"
                            Layout.alignment: Qt.AlignHCenter
                            Layout.maximumWidth: parent.width
                            visible: backend.books.total > 0 && backend.books.query.trim() !== "" && librarian.hasAi
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Ask the AI to find it")
                            onClicked: win.showAiSearch(backend.books.query)
                        }
                        AppButton {
                            objectName: "clearFilters"
                            Layout.alignment: Qt.AlignHCenter
                            visible: backend.books.total > 0 && view.filterKinds.length > 0
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Clear the filters")
                            onClicked: backend.books.clearFilters()
                        }
                    }
                }
            }

            ModSlot {
                id: sideSlot
                place: "library.side"
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.rightMargin: detailsSide.visible && !detailsSide.over ? detailsSide.width : 0
            }
            // The window behind the details, dimmed while they lie over the
            // shelf; a click on it puts them away.
            Rectangle {
                objectName: "detailsScrim"
                anchors.fill: parent
                visible: detailsSide.visible && detailsSide.over
                color: Qt.alpha(theme.background, 0.6)
                z: 4
                TapHandler { onTapped: view.detailsOver = false }
            }
            SidePane {
                id: detailsSide
                objectName: "detailsSide"
                name: "details"
                standard: 320
                least: 260
                most: 560
                // Beside the shelf while the shelf keeps its floor; over it,
                // and only when asked for, in a window too narrow for both.
                over: !view.detailsFit
                room: over ? body.width - 48 : body.width - win.contentFloor - (sideSlot.visible ? sideSlot.width : 0)
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                z: 5
                // A view that says what the selected book is needs no pane for it.
                visible: view.selectedId > 0 && !(view.shelf && view.shelf.ownDetails)
                         && (view.detailsFit ? view.showDetails : view.detailsOver)

                DetailsPane {
                    objectName: "details"
                    anchors.fill: parent
                    book: view.selectedBook
                }
            }
        }
    }
}
