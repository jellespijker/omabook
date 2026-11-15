import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    objectName: "window"
    width: 1180
    height: 780
    // The smallest window Omabook is laid out for: a narrow column on a
    // tiled screen, or a short strip of one. Everything below is reachable
    // at this size; tests/layout_tests.cpp looks at it.
    minimumWidth: 420
    minimumHeight: 360
    visible: true
    title: reader.open ? reader.title + " · Omabook" : "Omabook"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft

    // How much room the window has, by name. A tiling desktop hands it every
    // shape there is, so what is shown is decided by these and not by a
    // number of pixels written where it happens to be needed.
    //
    //   compact   narrower than 700: one thing at a time. Dialogs go edge to
    //             edge with less padding, a dialog of two halves shows one
    //             (the help's pages along its top, a plugin's details in
    //             place of the list), rows of fields stack.
    //   medium    700 to 899: everything in its own words, one column.
    //   expanded  900 and wider: room for two things side by side, as the
    //             plugin manager's list and details are.
    //   low       shorter than 520, at any width: what stacks takes fewer
    //             rows (one row of filter chips, a smaller cover over the
    //             details, dialogs to the window's edge).
    //
    // Two things are measured instead, because a size class can't know
    // them. The library's header drops and folds its parts in a fixed order
    // until the words it holds fit (see LibraryView.qml): how long they are
    // depends on the language. And a pane has its place beside the shelf or
    // the page for as long as that keeps `contentFloor`, and lies over it
    // when it wouldn't (see SidePane.qml): a pane's width is the reader's
    // own choice.
    readonly property string sizeClass: width < 700 ? "compact" : width < 900 ? "medium" : "expanded"
    readonly property bool compact: sizeClass === "compact"
    readonly property bool medium: sizeClass === "medium"
    readonly property bool expanded: sizeClass === "expanded"
    readonly property bool low: height < 520
    // The least width a shelf or a page keeps with a pane beside it.
    readonly property real contentFloor: 440
    // The least width the page has for two pages side by side. It is the
    // page's own width that is held against it, not the window's: a pane
    // beside the page takes from the one and not the other.
    readonly property real spreadFloor: 900
    // The space kept between a dialog and the window's edge.
    readonly property real dialogEdge: compact || low ? 8 : 24
    // The window shows the shelf, or the book that is open, opening, or failed to open.
    readonly property bool inReader: reader.open || reader.loading || reader.error !== ""
    // Single keys are shortcuts only while nothing is being typed and no dialog is up.
    readonly property bool typing: {
        const item = activeFocusItem
        return !!item && (item instanceof TextInput || (item instanceof TextEdit && !item.readOnly))
    }
    // Buttons for what the keyboard does appear where there is a touchscreen,
    // or as soon as the window is touched.
    property bool touchSeen: false
    readonly property bool touch: touchscreen || touchSeen
    readonly property bool dialogOpen: library.popupOpen || help.opened || edit.opened || lookup.opened || prefs.opened || removal.opened
                                       || libraries.opened || pluginList.opened || pluginAsk.opened
                                       || tidyUp.opened || readingSites.opened || graphBuild.opened || askLibrary.opened || changeIt.opened
                                       || collectionEditor.opened
                                       || (!!readerLoader.item && readerLoader.item.popupOpen) || tour.running
                                       || (!!graphLoader.item && graphLoader.item.popupOpen)
    readonly property bool keys: !typing && !dialogOpen

    // Whether anything is being worked on anywhere: the AI, a catalog, an
    // import, the graph.
    readonly property bool working: backend.busy || backend.searching || librarian.busy || librarian.answering
                                    || librarian.changing || assistant.busy || assistant.graphBusy
                                    || backend.graphJobs.busy || backend.graphJobs.planning
                                    || librarian.sites.working
    // What is at work where it can't be seen, because its dialog or pane is
    // closed; empty when everything at work is in view.
    readonly property string workingUnseen: {
        translator.language
        if (librarian.answering && !askLibrary.opened) return qsTr("The AI is answering about your library…")
        if (librarian.changing && !changeIt.opened) return qsTr("The AI is changing Omabook…")
        if (librarian.busy && !tidyUp.opened) return qsTr("Looking the library over…")
        if (backend.graphJobs.busy && !graphBuild.opened) return qsTr("Building the knowledge graph…")
        if (assistant.busy && !(win.inReader && !!readerLoader.item && readerLoader.item.showAssistant)) return qsTr("The AI is answering…")
        if (librarian.sites.working && !readingSites.opened) return qsTr("Syncing with your reading sites…")
        if (assistant.graphBusy) return qsTr("Noting what you have read…")
        return ""
    }
    function editBook(id) { if (id > 0) edit.show(id) }
    function newBook() { edit.showNew() }
    function lookUpBook(id) { if (id > 0) lookup.show(id) }
    function removeBook(id) { if (id !== 0) removal.show(id) }
    function showLibraries() { libraries.open() }
    // The knowledge graph in place of the shelf: of one book, or of the
    // whole library when no book is named.
    property bool graphShown: false
    function showGraph(bookId) {
        graphLoader.active = true
        graphShown = true
        if (bookId > 0) graphLoader.item.showBook(bookId)
        else graphLoader.item.showLibrary()
        graphLoader.item.takeFocus()
    }
    // The graph a plugin asked for (omabook.ui.showGraph): of a book, else
    // of a scope, which may be a world of the plugin's own. Beside the
    // page while a book is open, in place of the shelf otherwise.
    function showGraphOf(scope, bookId) {
        if (win.inReader && readerLoader.item) {
            readerLoader.item.showGraphOf(scope, bookId)
            return
        }
        graphLoader.active = true
        graphShown = true
        if (bookId > 0) graphLoader.item.showBook(bookId)
        else graphLoader.item.showScope(scope)
        graphLoader.item.takeFocus()
    }
    function resizePane(steps) {
        if (win.inReader && readerLoader.item) readerLoader.item.resizePane(steps)
        else if (win.graphShown && graphLoader.item) graphLoader.item.resizePane(steps)
        else library.resizePane(steps)
    }
    function showSettings() { prefs.open() }
    function showHelp() { help.show() }
    // The tour of the window ("tour"), or of what a release brought ("news"
    // since the version last seen, "allnews" for the newest again).
    readonly property bool touring: tour.running
    function startTour(what) { return tour.start(what) }
    // The keyboard, back with the shelf or the page.
    function restoreFocus() {
        if (win.inReader && readerLoader.item && readerLoader.item.content) readerLoader.item.content.forceActiveFocus()
        else if (win.graphShown && graphLoader.item) graphLoader.item.takeFocus()
        else library.takeFocus()
    }
    // The item with that objectName, if it is on screen: what the tour points at.
    function shown(name, from) {
        const item = from || win.contentItem
        if (item.objectName === name && item.visible && item.width > 0 && item.height > 0)
            return item
        const children = item.children
        for (let i = 0; i < children.length; ++i) {
            const found = shown(name, children[i])
            if (found)
                return found
        }
        return null
    }
    // A pane the tour can open to point at, and close again after.
    function canReveal(name) {
        return win.inReader && reader.open && !!readerLoader.item && (name === "assistantPane" || name === "contents")
    }
    function reveal(name, yes) {
        if (!canReveal(name)) return
        if (name === "assistantPane") readerLoader.item.showAssistant = yes
        else readerLoader.item.showIndex = yes
    }
    // Every Key in the window. The help runs a command through the Key that
    // is its place right now, so what a command does is written once.
    function keysUnder(item, found) {
        if (item.command !== undefined && item.when !== undefined && typeof item.activated === "function")
            found.push(item)
        const children = item.children
        for (let i = 0; i < children.length; ++i)
            keysUnder(children[i], found)
        return found
    }
    function runnableCommands() {
        const commands = {}
        for (const key of keysUnder(win.contentItem, []))
            if (key.when) commands[key.command] = true
        return commands
    }
    function run(command) {
        const key = keysUnder(win.contentItem, []).find(key => key.command === command && key.when)
        if (key) key.activated()
        return !!key
    }
    function showPlugins() { pluginList.open() }
    function showTidy() { tidyUp.show("") }
    // Only the tidy's pass that sorts books into collections.
    function showCollectionSort() { tidyUp.show("collections") }
    // A new collection, its rule filled in when one is given; or one to change.
    function newCollection(rule, parent) { collectionEditor.create(rule, parent || 0) }
    function editCollection(id) { if (id) collectionEditor.change(id) }
    function showReadingSites() { readingSites.open() }
    function showThemes() { if (theme.themes) themePicker.open() }
    function showGraphBuild(id) { graphBuild.show(id) }
    function showAskLibrary() { askLibrary.open() }
    function showSuggestions() {
        askLibrary.open()
        librarian.suggest()
    }

    Key { command: "global.help"; onActivated: help.opened ? help.close() : help.show() }
    // The help closes on the key that opened it, though it is a dialog.
    Shortcut { sequences: ["?", "F1"]; enabled: help.opened; onActivated: help.close() }
    Key { command: "global.quit"; onActivated: Qt.quit() }
    Key { command: "global.settings"; onActivated: prefs.open() }
    Key { command: "global.change"; onActivated: changeIt.open() }
    // The pane that is open beside the shelf or the page, wider or narrower.
    Key { command: "global.pane_wider"; onActivated: win.resizePane(1) }
    Key { command: "global.pane_narrower"; onActivated: win.resizePane(-1) }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        // The plugins hear which book is selected, and say when the graph
        // should be shown.
        Binding { target: backend.scripts; property: "selectedBook"; value: library.selectedId }
        Connections {
            target: backend.scripts
            function onGraphRequested(scope, book) { win.showGraphOf(scope, book) }
        }

        // What script plugins add to the keyboard, each in the place it
        // says it belongs, run for the book that is selected or open.
        Repeater {
            model: custom.pluginCommands
            delegate: Key {
                required property var modelData
                command: modelData.id
                when: modelData.place === "global" || (modelData.place === "reader") === win.inReader
                onActivated: backend.scripts.run(modelData.id, win.inReader ? reader.bookId : library.selectedId)
            }
        }

        LibraryView {
            id: library
            objectName: "library"
            anchors.fill: parent
            visible: !win.inReader && !win.graphShown
            active: !win.inReader && !win.graphShown && win.keys
        }

        // The knowledge graph, built the first time it is asked for. It
        // stays up behind a book opened from it, to come back to.
        Loader {
            id: graphLoader
            anchors.fill: parent
            active: false
            visible: win.graphShown && !win.inReader
            sourceComponent: GraphPane {
                objectName: "libraryGraph"
                active: visible && win.keys
                onVisibleChanged: if (visible) graphFocus.restart()
                onDone: {
                    win.graphShown = false
                    library.takeFocus()
                }
                // Once the reader has let go of the keyboard.
                Timer {
                    id: graphFocus
                    interval: 0
                    onTriggered: if (win.graphShown && !win.inReader) graphLoader.item.takeFocus()
                }
            }
        }

        // The reader is built when the first book opens, so the shelf is up at once.
        Loader {
            id: readerLoader
            anchors.fill: parent
            active: false
            visible: win.inReader
            sourceComponent: ReaderView { active: win.inReader && win.keys }
        }
        Connections {
            target: reader
            function onLoadingChanged() { if (win.inReader) readerLoader.active = true }
            function onBookChanged() {
                if (reader.open) readerLoader.active = true
                else library.takeFocus()
            }
        }

        PointHandler {
            acceptedDevices: PointerDevice.TouchScreen
            onActiveChanged: if (active) win.touchSeen = true
        }

        DropArea {
            anchors.fill: parent
            enabled: !win.inReader
            onDropped: (drop) => { if (drop.hasUrls) backend.addUrls(drop.urls) }
        }

        // What just happened, said once at the bottom and dismissed by a click
        // or by the next thing that happens.
        Rectangle {
            id: toast
            objectName: "toast"
            visible: backend.message !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 24
            width: Math.min(parent.width - 2 * win.dialogEdge, toastText.implicitWidth + 40)
            height: toastText.implicitHeight + 24
            radius: theme.radius
            color: theme.colors.selection
            border.color: theme.colors.muted
            Label {
                id: toastText
                objectName: "toastText"
                anchors.centerIn: parent
                width: Math.min(implicitWidth, toast.width - 40)
                text: backend.message
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }
            TapHandler { onTapped: backend.dismissMessage() }
        }

        // A Calibre library found on this computer, offered for import.
        CalibreOffer {
            id: calibreOffer
            visible: offered && !win.inReader
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: toast.visible ? toast.top : parent.bottom
            anchors.bottomMargin: toast.visible ? 12 : 24
        }

        // The tour, offered once, above whatever else is being said.
        TourNote {
            readonly property Item below: calibreOffer.visible ? calibreOffer : toast.visible ? toast : null
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: below ? below.top : parent.bottom
            anchors.bottomMargin: below ? 12 : 24
        }
        // Work that goes on with its own dialog or pane closed still shows:
        // a line along the top of the window for as long as anything is at
        // work, and in a corner, in words, what it is.
        ProgressBar {
            objectName: "workingBar"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 2
            z: 50
            visible: win.working
            indeterminate: visible
        }
        Rectangle {
            objectName: "workingNote"
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 12
            z: 50
            visible: win.workingUnseen !== ""
            width: workingSign.width + 20
            height: workingSign.height + 12
            radius: Math.min(4, theme.radius)
            color: theme.background
            border.color: theme.colors.selection
            Thinking {
                id: workingSign
                anchors.centerIn: parent
                active: parent.visible
                room: win.width - 44
                text: win.workingUnseen
            }
        }
        TourOverlay {
            id: tour
            anchors.fill: parent
        }
    }

    HelpOverlay { id: help }
    EditDialog { id: edit }
    LookupDialog { id: lookup }
    SettingsDialog { id: prefs }
    LibrariesPopup { id: libraries }
    PluginsDialog { id: pluginList }
    PluginAsk { id: pluginAsk }
    PluginForm { id: pluginForm }
    TidyDialog { id: tidyUp }
    CollectionDialog { id: collectionEditor }
    ReadingSitesDialog { id: readingSites }
    ThemePicker { id: themePicker }
    GraphBuildDialog { id: graphBuild }
    AskDialog {
        id: askLibrary
        objectName: "askLibrary"
        heading: qsTr("Ask about your library")
        placeholder: qsTr("Which of my books…?")
        reply: librarian.answer
        working: librarian.answering
        offer: qsTr("Suggest what to read next")
        onAsked: (words) => librarian.ask(words)
        onOffered: librarian.suggest()
        onlyName: backend.collection !== 0 ? backend.collectionName : ""
        only: librarian.onlyCollection
        onOnlyToggled: (only) => librarian.onlyCollection = only
        suggestions: librarian.suggestions
        onWanted: (index) => librarian.wantSuggestion(index)
    }
    AskDialog {
        id: changeIt
        objectName: "changeIt"
        heading: qsTr("Change Omabook")
        placeholder: qsTr("Make it warmer, bigger covers, Space for the next chapter…")
        reply: librarian.changeNote
        working: librarian.changing
        undoable: librarian.canUndo
        onAsked: (words) => librarian.change(words)
        onUndo: librarian.undoChange()
    }

    AppPopup {
        id: removal
        objectName: "removal"
        property var bookId: 0
        property string bookTitle: ""
        // A plugin's launcher is only taken off the shelf; its plugin hears of it.
        property bool launcher: false
        property string pluginName: ""
        function show(id) {
            const book = backend.book(id)
            bookId = id
            bookTitle = book.title || ""
            launcher = book.launcher === true
            pluginName = book.pluginName || ""
            open()
        }

        widest: 468

        DialogBody {
            Label {
                Layout.fillWidth: true
                text: removal.launcher ? qsTr("Take “%1” off the shelf?").arg(removal.bookTitle)
                      : qsTr("Remove “%1” from the library?").arg(removal.bookTitle)
                font.pixelSize: 18
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: removal.launcher ? qsTr("It is not a book: %1 put it there to start something new. %1 can put it back.").arg(removal.pluginName)
                      : qsTr("Its files in the library folder move to the trash. The file you added it from is not touched.")
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            actions: [
                AppButton { text: qsTr("Keep"); onClicked: removal.close() },
                AppButton {
                    objectName: "confirmRemove"
                    text: qsTr("Remove")
                    primary: true
                    focus: true
                    onClicked: {
                        if (removal.launcher) backend.scripts.removeLauncher(removal.bookId)
                        else backend.remove(removal.bookId)
                        removal.close()
                    }
                }
            ]
        }
    }
}
