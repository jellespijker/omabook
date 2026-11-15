import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What the library knows about the selected book, and what can be done with it.
Item {
    id: pane
    property var book: ({})
    readonly property bool hasBook: book.bookId !== undefined
    // A plugin's launcher: something to start, which is opened and can be
    // taken off the shelf, and has nothing else a book has.
    readonly property bool launcher: book.launcher === true
    GraphWords { id: graphWords }

    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: theme.colors.selection
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 20
        contentHeight: column.implicitHeight
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            width: parent.width
            spacing: 10

            BookCover {
                Layout.alignment: Qt.AlignHCenter
                // Smaller in a low window, where it would fill the pane.
                Layout.preferredWidth: win.low ? 100 : 180
                Layout.preferredHeight: win.low ? 150 : 270
                Layout.bottomMargin: 8
                cover: pane.book.cover || ""
                title: pane.book.title || ""
                authors: pane.book.authors || ""
                percent: pane.book.percent || 0
                status: pane.book.status || ""
                held: pane.book.held || "digital"
            }
            Label {
                objectName: "detailsLauncher"
                Layout.fillWidth: true
                visible: pane.launcher
                // "From the plugin Storyteller: not a book, a way to start one"
                text: qsTr("From the plugin %1. It starts something new; it is not a book of the library.").arg(pane.book.pluginName || "")
                textFormat: Text.PlainText
                color: theme.colors.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "detailsTitle"
                Layout.fillWidth: true
                text: pane.book.title || ""
                font.pixelSize: 19
                font.bold: true
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "detailsAuthors"
                Layout.fillWidth: true
                visible: text !== ""
                text: pane.book.authors || ""
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: (pane.book.series || "") !== ""
                // "Dune, book 2"
                text: pane.book.seriesIndex > 0
                      ? qsTr("%1, book %2").arg(pane.book.series || "").arg(Number(pane.book.seriesIndex || 0).toLocaleString(Qt.locale(), 'f', Number.isInteger(pane.book.seriesIndex || 0) ? 0 : 1))
                      : (pane.book.series || "")
                color: theme.accent
                wrapMode: Text.Wrap
            }
            Stars {
                objectName: "detailsStars"
                visible: !pane.launcher
                rating: pane.book.rating || 0
                size: 16
                color: theme.accent
            }
            // Where the book stands, when a shelf or a reading site said so.
            Label {
                objectName: "detailsShelf"
                Layout.fillWidth: true
                visible: text !== ""
                text: {
                    const shelf = pane.book.shelf || ""
                    const held = pane.book.held === "paper" ? qsTr("On my shelf") : pane.book.held === "none" ? qsTr("Not owned") : ""
                    const where = shelf === "read" ? (pane.book.finished ? qsTr("Read, finished %1").arg(pane.book.finished) : qsTr("Read"))
                                : shelf === "reading" ? qsTr("Reading") : shelf === "to-read" ? qsTr("To read") : ""
                    return [held, where].filter(part => part !== "").join(" · ")
                }
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: pane.book.percent > 0
                text: {
                    translator.language
                    return qsTr("%1% read").arg(Math.round(pane.book.percent || 0).toLocaleString(Qt.locale(), 'f', 0))
                }
                color: theme.colors.muted
            }
            // Found by the search through what is known of it rather than its
            // details: the thing that did, to see in the graph or to go back
            // to where it was learned.
            ColumnLayout {
                id: foundBy
                objectName: "detailsFoundBy"
                readonly property string runs: "library.found_graph library.found_moment"
                readonly property var known: {
                    backend.books.knownRevision
                    return pane.hasBook && !pane.launcher ? backend.books.knownFor(pane.book.bookId) : ({})
                }
                Layout.fillWidth: true
                Layout.topMargin: 4
                visible: foundBy.known.node !== undefined
                spacing: 6
                Label {
                    objectName: "detailsFoundByText"
                    Layout.fillWidth: true
                    // "Found by what you know: Mara Venn (Character)"
                    text: foundBy.visible ? qsTr("Found by what you know: %1 (%2)").arg(foundBy.known.name).arg(graphWords.kind(foundBy.known.kind)) : ""
                    textFormat: Text.PlainText
                    color: theme.accent
                    wrapMode: Text.Wrap
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    AppButton {
                        objectName: "foundGraph"
                        text: qsTr("Show in the graph")
                        focusPolicy: Qt.NoFocus
                        onClicked: win.showFound(foundBy.known)
                    }
                    AppButton {
                        objectName: "foundMoment"
                        visible: foundBy.known.readable === true && foundBy.known.metIn > 0
                        text: qsTr("Go to the moment")
                        focusPolicy: Qt.NoFocus
                        onClicked: win.goToFound(foundBy.known)
                    }
                }
            }
            // A Flow, so longer words in other languages wrap to a second row.
            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 8
                AppButton {
                    objectName: "readButton"
                    runs: "library.read"
                    text: pane.launcher ? qsTr("Open") : qsTr("Read")
                    primary: true
                    focusPolicy: Qt.NoFocus
                    onClicked: pane.launcher ? backend.scripts.launch(pane.book.bookId) : reader.openBook(pane.book.bookId)
                }
                AppButton {
                    objectName: "editButton"
                    runs: "library.edit"
                    visible: !pane.launcher
                    text: qsTr("Edit")
                    focusPolicy: Qt.NoFocus
                    onClicked: win.editBook(pane.book.bookId)
                }
                AppButton {
                    objectName: "lookUpButton"
                    runs: "library.lookup"
                    visible: !pane.launcher
                    text: qsTr("Look Up")
                    focusPolicy: Qt.NoFocus
                    onClicked: win.lookUpBook(pane.book.bookId)
                }
                AppButton {
                    objectName: "graphButton"
                    runs: "library.graph_book"
                    visible: !pane.launcher
                    text: qsTr("Graph")
                    focusPolicy: Qt.NoFocus
                    onClicked: win.showGraph(pane.book.bookId)
                }
                AppButton {
                    objectName: "removeButton"
                    runs: "library.remove"
                    text: qsTr("Remove")
                    focusPolicy: Qt.NoFocus
                    onClicked: win.removeBook(pane.book.bookId)
                }
                // The rest of what can be done with it (save a copy, its
                // collections...), as a right click on the book lists it.
                AppButton {
                    objectName: "bookMore"
                    visible: !pane.launcher
                    text: qsTr("More")
                    focusPolicy: Qt.NoFocus
                    onClicked: win.showMenu("book")
                }
            }
            // The collections there are, those the book is in filled in: a
            // tap puts it in or takes it out, by hand, whatever the rule.
            Label {
                Layout.fillWidth: true
                Layout.topMargin: 6
                visible: inCollections.visible
                text: qsTr("Collections")
                color: theme.colors.muted
                font.pixelSize: 12
            }
            Flow {
                id: inCollections
                objectName: "detailsCollections"
                readonly property string runs: "library.collection_book"
                Layout.fillWidth: true
                visible: pane.hasBook && !pane.launcher && backend.collections.length > 0
                spacing: 6
                Repeater {
                    model: {
                        backend.revision
                        return inCollections.visible ? backend.bookCollections(pane.book.bookId) : []
                    }
                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        objectName: "detailsCollection-" + modelData.id
                        width: Math.min(inCollections.width, chipRow.implicitWidth + 20)
                        height: win.touch ? 38 : 28
                        radius: theme.radius
                        color: modelData.in ? Qt.alpha(theme.accent, chipHover.hovered ? 0.3 : 0.18)
                               : chipHover.hovered ? theme.colors.selection : "transparent"
                        border.color: modelData.in ? Qt.alpha(theme.accent, 0.6) : theme.colors.selection
                        Accessible.role: Accessible.CheckBox
                        Accessible.name: modelData.name
                        Accessible.checked: modelData.in
                        Accessible.onToggleAction: backend.setInCollection(modelData.id, pane.book.bookId, !modelData.in)
                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 6
                            Glyph {
                                name: chip.modelData.in ? "check" : "plus"
                                size: 12
                                color: chip.modelData.in ? theme.foreground : Qt.alpha(theme.foreground, 0.6)
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Label {
                                text: chip.modelData.name
                                font.pixelSize: 13
                                width: Math.min(implicitWidth, inCollections.width - 44)
                                elide: Text.ElideRight
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        HoverHandler { id: chipHover }
                        TapHandler { onTapped: backend.setInCollection(chip.modelData.id, pane.book.bookId, !chip.modelData.in) }
                        ToolTip.visible: chipHover.hovered
                        ToolTip.delay: 600
                        ToolTip.text: (chip.modelData.in ? qsTr("Take it out of %1") : qsTr("Put it in %1")).arg(chip.modelData.name)
                                      + "  " + (custom.keyText["library.collection_book"] || "").split("  ")[0]
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                Layout.topMargin: 6
                visible: text !== ""
                text: pane.book.description || ""
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            Label {
                Layout.fillWidth: true
                visible: (pane.book.tags || "") !== ""
                text: pane.book.tags || ""
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                Layout.topMargin: 6
                // "Tor · 1965 · EPUB, PDF"
                text: [pane.book.publisher, pane.book.published, pane.book.formatNames].filter(part => !!part).join(" · ")
                color: theme.colors.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
    }
}
