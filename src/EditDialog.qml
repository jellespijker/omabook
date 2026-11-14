import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A book's details, to correct by hand. Nothing is saved until Save.
AppPopup {
    id: dialog
    objectName: "edit"
    widest: 620
    // Clicking outside would throw typed changes away; only Escape and Cancel close it.
    closePolicy: Popup.CloseOnEscape
    // 0 while a book that has no file is being added.
    property var bookId: 0
    readonly property var shelves: ["", "to-read", "reading", "read"]

    function show(id) {
        const book = backend.book(id)
        const reading = backend.reading(id)
        bookId = id
        shelf.currentIndex = Math.max(0, shelves.indexOf(reading.shelf))
        stars.value = reading.rating
        finished.text = reading.finished
        paper.checked = reading.paper
        title.text = book.title || ""
        authors.text = book.authors || ""
        series.text = book.series || ""
        number.text = book.seriesIndex > 0 ? String(book.seriesIndex) : ""
        tags.text = book.tags || ""
        publisher.text = book.publisher || ""
        published.text = book.published || ""
        language.text = book.language || ""
        isbn.text = book.isbn || ""
        description.text = book.description || ""
        open()
        title.forceActiveFocus()
    }
    // A new book with no file: one on a real shelf, or one to read some day.
    function showNew() {
        bookId = 0
        for (const field of [title, authors, series, number, tags, publisher, published, language, isbn, description, finished])
            field.text = ""
        shelf.currentIndex = 0
        stars.value = 0
        paper.checked = true
        open()
        title.forceActiveFocus()
    }
    function save() {
        const details = {
            title: title.text, authors: authors.text, series: series.text,
            seriesIndex: Number(number.text.replace(",", ".")) || 0, tags: tags.text,
            publisher: publisher.text, published: published.text, language: language.text,
            isbn: isbn.text, description: description.text,
            shelf: shelves[shelf.currentIndex], rating: stars.value, finished: finished.text, paper: paper.checked
        }
        if (bookId > 0) {
            backend.saveDetails(bookId, details)
        } else {
            if (title.text.trim() === "") {
                title.forceActiveFocus()
                return
            }
            backend.newEntry(details)
        }
        close()
    }

    Shortcut { sequence: "Ctrl+S"; enabled: dialog.opened; onActivated: dialog.save() }
    Shortcut { sequence: "Ctrl+Return"; enabled: dialog.opened; onActivated: dialog.save() }

    DialogBody {
        Label {
            objectName: "editHeading"
            visible: dialog.bookId === 0
            Layout.fillWidth: true
            text: qsTr("A book without a file")
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        // A Material text field floats its placeholder as its label.
        TextField { id: title; objectName: "editTitle"; Layout.fillWidth: true; placeholderText: qsTr("Title"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
        TextField { id: authors; objectName: "editAuthors"; Layout.fillWidth: true; placeholderText: qsTr("Authors, separated by &"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
        // The short fields stand in pairs, and one under the other where
        // the window is too narrow for two.
        GridLayout {
            Layout.fillWidth: true
            columns: win.compact ? 1 : 2
            columnSpacing: 12
            rowSpacing: 12
            TextField { id: series; Layout.fillWidth: true; placeholderText: qsTr("Series"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
            TextField { id: number; Layout.fillWidth: win.compact; Layout.preferredWidth: 140; placeholderText: qsTr("Number"); inputMethodHints: Qt.ImhFormattedNumbersOnly; Material.roundedScale: theme.radius; onAccepted: dialog.save() }
        }
        TextField { id: tags; objectName: "editTags"; Layout.fillWidth: true; placeholderText: qsTr("Tags, separated by commas"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
        GridLayout {
            Layout.fillWidth: true
            columns: win.compact ? 1 : 2
            columnSpacing: 12
            rowSpacing: 12
            TextField { id: publisher; Layout.fillWidth: true; placeholderText: qsTr("Publisher"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
            TextField { id: published; Layout.fillWidth: win.compact; Layout.preferredWidth: 140; placeholderText: qsTr("Published"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
            TextField { id: isbn; Layout.fillWidth: true; placeholderText: qsTr("ISBN"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
            TextField { id: language; Layout.fillWidth: win.compact; Layout.preferredWidth: 140; placeholderText: qsTr("Language"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
        }
        // Where the book stands with its reader, and whether it is on a real shelf.
        GridLayout {
            Layout.fillWidth: true
            columns: win.compact ? 1 : 2
            columnSpacing: 12
            rowSpacing: 12
            ComboBox {
                id: shelf
                objectName: "editShelf"
                Layout.fillWidth: true
                Material.roundedScale: theme.radius
                model: [qsTr("As far as read here"), qsTr("To read"), qsTr("Reading"), qsTr("Read")]
            }
            // Stars, one tap each; tapping the one that is set clears them.
            Row {
                id: stars
                objectName: "editStars"
                property int value: 0
                spacing: 2
                Repeater {
                    model: 5
                    delegate: Item {
                        id: star
                        required property int index
                        // As wide as a fingertip under fingers.
                        width: win.touch ? 38 : 26
                        height: win.touch ? 38 : 30
                        Label {
                            anchors.centerIn: parent
                            text: star.index < stars.value ? "★" : "☆"
                            font.pixelSize: 22
                            color: star.index < stars.value ? theme.accent : theme.colors.muted
                        }
                        TapHandler { onTapped: stars.value = stars.value === star.index + 1 ? 0 : star.index + 1 }
                    }
                }
            }
            TextField { id: finished; objectName: "editFinished"; Layout.fillWidth: true; placeholderText: qsTr("Finished on (2026-03-01)"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
            CheckBox { id: paper; objectName: "editPaper"; text: qsTr("On my shelf") }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            TextArea {
                id: description
                placeholderText: qsTr("Description")
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                Material.roundedScale: theme.radius
            }
        }
        actions: [
            AppButton {
                text: qsTr("Choose Cover")
                // A book that is not there yet has nothing to hang a cover on.
                visible: dialog.bookId > 0
                onClicked: backend.chooseCover(dialog.bookId)
            },
            AppButton { text: qsTr("Cancel"); onClicked: dialog.close() },
            AppButton { objectName: "editSave"; text: qsTr("Save"); primary: true; onClicked: dialog.save() }
        ]
    }
}
