import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A book's details, to correct by hand. Nothing is saved until Save, and then
// only what was changed here (#223).
AppPopup {
    id: dialog
    objectName: "edit"
    widest: 620
    // Clicking outside would throw typed changes away; only Escape and Cancel close it.
    closePolicy: Popup.CloseOnEscape
    // 0 while a book that has no file is being added.
    property var bookId: 0
    readonly property var shelves: ["", "to-read", "reading", "read"]
    // Whether it has an EPUB to write the details into, and whether that file
    // as it came in is kept to put back (#74).
    property var file: ({})
    // The book's fields as the library had them when they were shown, and as
    // the dialog's own controls took them: only what the person changed from
    // the latter is saved, and the former says whether someone else (the
    // command line, an agent, a sync) changed the same since.
    property var before: ({})
    property var shown: ({})
    // The fields changed both here and elsewhere, and whether the book went.
    property var clashes: []
    property bool gone: false
    // What was typed for the number in the series when it is no number (#230).
    property string noNumber: ""
    readonly property var texts: ({
        title: title, authors: authors, series: series, seriesIndex: number, tags: tags, publisher: publisher,
        published: published, language: language, isbn: isbn, description: description, finished: finished
    })
    readonly property var fieldNames: ({
        title: qsTr("Title"), authors: qsTr("Authors"), series: qsTr("Series"), seriesIndex: qsTr("Number"),
        tags: qsTr("Tags"), publisher: qsTr("Publisher"), published: qsTr("Published"), language: qsTr("Language"),
        isbn: qsTr("ISBN"), description: qsTr("Description"), shelf: qsTr("Shelf"), rating: qsTr("Rating"),
        finished: qsTr("Finished"), paper: qsTr("On my shelf")
    })

    // A book's fields as the library has them now; null when it isn't there.
    function stored(id) {
        const book = backend.book(id)
        const reading = backend.reading(id)
        if (reading.shelf === undefined)
            return null
        const values = { shelf: reading.shelf, rating: reading.rating, finished: reading.finished, paper: reading.paper }
        for (const key of ["title", "authors", "series", "tags", "publisher", "published", "language", "isbn", "description"])
            values[key] = book[key] || ""
        values.seriesIndex = book.seriesIndex > 0 ? String(book.seriesIndex) : ""
        return values
    }
    // What the controls say, and putting some fields into them.
    function typed() {
        const values = { shelf: shelves[shelf.currentIndex], rating: stars.value, paper: paper.checked }
        for (const key in texts)
            values[key] = texts[key].text
        return values
    }
    function put(values) {
        for (const key in values) {
            if (key === "shelf")
                shelf.currentIndex = Math.max(0, shelves.indexOf(values[key]))
            else if (key === "rating")
                stars.value = values[key]
            else if (key === "paper")
                paper.checked = values[key]
            else
                texts[key].text = values[key]
        }
    }
    function show(id) {
        const values = stored(id)
        if (!values)
            return
        bookId = id
        clashes = []
        gone = false
        noNumber = ""
        put(values)
        before = values
        shown = typed()
        file = backend.fileDetails(id)
        intoFile.checked = settings.writeDetails
        open()
        title.forceActiveFocus()
    }
    // A new book with no file: one on a real shelf, or one to read some day.
    function showNew() {
        bookId = 0
        file = ({})
        clashes = []
        gone = false
        noNumber = ""
        shown = ({})
        for (const field of [title, authors, series, number, tags, publisher, published, language, isbn, description, finished])
            field.text = ""
        shelf.currentIndex = 0
        stars.value = 0
        paper.checked = true
        open()
        title.forceActiveFocus()
    }
    function save() {
        const mine = typed()
        const index = mine.seriesIndex.trim().replace(",", ".")
        noNumber = mine.seriesIndex !== shown.seriesIndex && !/^(\d+(\.\d*)?|\.\d+)?$/.test(index) ? mine.seriesIndex : ""
        if (noNumber !== "") {
            number.forceActiveFocus()
            return
        }
        let details = mine
        if (bookId > 0) {
            const now = stored(bookId)
            gone = !now
            if (gone)
                return
            details = {}
            for (const key in mine)
                if (mine[key] !== shown[key])
                    details[key] = mine[key]
            // Changed on both sides, to different things: said first, and
            // saved over theirs only when asked again.
            const both = Object.keys(details).filter(key => now[key] !== before[key] && now[key] !== mine[key])
            if (both.length > 0) {
                const seen = Object.assign({}, before)
                for (const key of both)
                    seen[key] = now[key]
                before = seen
                clashes = both
                return
            }
        } else if (title.text.trim() === "") {
            title.forceActiveFocus()
            return
        }
        if (details.seriesIndex !== undefined)
            details.seriesIndex = Number(index)
        details.writeToFile = intoFile.visible && intoFile.checked
        if (bookId > 0)
            backend.saveDetails(bookId, details)
        else
            backend.newEntry(details)
        close()
    }
    // The other side's values for the fields changed on both, into the dialog;
    // what was changed only here stays.
    function takeTheirs() {
        const now = stored(bookId)
        gone = !now
        if (gone)
            return
        const theirs = {}
        for (const key of clashes)
            theirs[key] = now[key]
        put(theirs)
        const taken = typed()
        const loaded = Object.assign({}, shown)
        for (const key of clashes)
            loaded[key] = taken[key]
        shown = loaded
        before = Object.assign({}, before, theirs)
        clashes = []
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
            TextField { id: number; objectName: "editNumber"; Layout.fillWidth: win.compact; Layout.preferredWidth: 140; placeholderText: qsTr("Number"); inputMethodHints: Qt.ImhFormattedNumbersOnly; Material.roundedScale: theme.radius; onAccepted: dialog.save() }
        }
        TextField { id: tags; objectName: "editTags"; Layout.fillWidth: true; placeholderText: qsTr("Tags, separated by commas"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
        GridLayout {
            Layout.fillWidth: true
            columns: win.compact ? 1 : 2
            columnSpacing: 12
            rowSpacing: 12
            TextField { id: publisher; objectName: "editPublisher"; Layout.fillWidth: true; placeholderText: qsTr("Publisher"); Material.roundedScale: theme.radius; onAccepted: dialog.save() }
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
                objectName: "editDescription"
                placeholderText: qsTr("Description")
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                Material.roundedScale: theme.radius
            }
        }
        // The library's copy of the book, never the file it was added from;
        // the settings say whether this is ticked to begin with.
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.file.epub === true
            CheckBox {
                id: intoFile
                objectName: "editIntoFile"
                Layout.fillWidth: true
                text: qsTr("Write the details into the book's EPUB too, for other apps and e-readers")
            }
            AppButton {
                objectName: "editRestoreFile"
                visible: dialog.file.original === true
                text: qsTr("Put the File Back")
                onClicked: {
                    backend.restoreFile(dialog.bookId)
                    dialog.close()
                }
            }
        }
        // Why Save did not save: kept in sight with the buttons, however
        // low the window is, so it isn't pressed again unread (#257).
        noted: dialog.noNumber !== "" || dialog.gone || dialog.clashes.length > 0
        notes: [
            Label {
                objectName: "editNoNumber"
                visible: dialog.noNumber !== ""
                Layout.fillWidth: true
                text: qsTr("Number takes a number, such as 3 or 2.5, not “%1”. Nothing was saved.").arg(dialog.noNumber)
                color: theme.colors.orange
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            },
            // What happened to the book while the dialog was open, when it
            // stands in the way of saving.
            Label {
                objectName: "editChanged"
                visible: dialog.gone || dialog.clashes.length > 0
                Layout.fillWidth: true
                text: dialog.gone ? qsTr("This book was removed from the library meanwhile, so its details can't be saved.")
                      : qsTr("Changed elsewhere meanwhile: %1. Save again to keep what you entered.")
                            .arg(dialog.clashes.map(key => dialog.fieldNames[key]).join(", "))
                color: theme.colors.orange
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            },
            AppButton {
                objectName: "editTakeTheirs"
                visible: !dialog.gone && dialog.clashes.length > 0
                text: qsTr("Take the Other Changes")
                onClicked: dialog.takeTheirs()
            }
        ]
        actions: [
            AppButton {
                text: qsTr("Choose Cover")
                // A book that is not there yet has nothing to hang a cover on.
                visible: dialog.bookId > 0
                onClicked: backend.chooseCover(dialog.bookId)
            },
            AppButton { text: qsTr("Cancel"); onClicked: dialog.close() },
            AppButton { objectName: "editSave"; text: qsTr("Save"); primary: true; enabled: !dialog.gone; onClicked: dialog.save() }
        ]
    }
}
