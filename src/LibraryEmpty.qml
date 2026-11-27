import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// In the shelf's place when it has nothing to show: how to fill an empty
// library or collection, or that a search or a filter finds nothing, with the
// ways on. In the middle of that place, and scrolling where a low window has
// less room than the words and buttons take.
Flickable {
    id: emptyScroller
    // The library's view (LibraryView.qml): what it filters by, and its keys.
    property Item shelfView
    visible: backend.books.count === 0 && backend.libraryError === ""
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
            text: shelfView.emptyCollection ? qsTr("Nothing is in %1 yet").arg(backend.collectionName)
                  : backend.books.total === 0 ? qsTr("No books yet")
                  : qsTr("No book matches")
        }
        // A collection that holds nothing says how it is filled.
        Label {
            objectName: "emptyCollection"
            Layout.fillWidth: true
            visible: shelfView.emptyCollection
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("Its rule finds no book. Change the rule, put a book in with %1, or have the AI sort the books that are in no collection.")
                  .arg(shelfView.keyOf("library.collection_book"))
        }
        Flow {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: parent.width
            visible: shelfView.emptyCollection
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
            visible: backend.libraryError === "" && backend.books.total === 0 && !shelfView.emptyCollection
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: theme.colors.muted
            // The keys are named as they are bound now.
            text: qsTr("Press %1 to add books or %2 for a folder of them, or drop them on the window. They are copied to %3; your own files are not touched.")
                  .arg(shelfView.keyOf("library.add")).arg(shelfView.keyOf("library.add_folder")).arg(backend.libraryPath)
        }
        Label {
            Layout.fillWidth: true
            visible: backend.libraryError === "" && backend.books.total === 0 && !shelfView.emptyCollection
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("%1 adds a book you own on paper, or one you want to read. A Calibre library comes across whole, with what you have read. %2 shows every key.")
                  .arg(shelfView.keyOf("library.new")).arg(shelfView.keyOf("global.help"))
        }
        // Every way in, for a mouse or a finger too.
        Flow {
            objectName: "emptyActions"
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: parent.width
            Layout.topMargin: 4
            visible: backend.libraryError === "" && backend.books.total === 0 && !shelfView.emptyCollection
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
            visible: backend.books.total > 0 && shelfView.filterKinds.length > 0
            focusPolicy: Qt.NoFocus
            text: qsTr("Clear the filters")
            onClicked: backend.books.clearFilters()
        }
    }
}
