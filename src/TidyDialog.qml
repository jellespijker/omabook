import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The library looked over as a whole: every change that would tidy it, to
// tick or untick, and nothing done until asked.
AppPopup {
    id: dialog
    objectName: "tidy"
    widest: 760
    tallest: 680
    closePolicy: Popup.CloseOnEscape

    readonly property var fieldNames: ({
        title: qsTr("title"), authors: qsTr("authors"), series: qsTr("series"), publisher: qsTr("publisher"),
        published: qsTr("date"), language: qsTr("language"), isbn: qsTr("ISBN"), description: qsTr("description"),
        tags: qsTr("tags"), cover: qsTr("cover")
    })
    readonly property var kinds: ({
        duplicate: qsTr("The same book twice"), author: qsTr("Author's name"), series: qsTr("Series"),
        details: qsTr("Details looked up"), collection: qsTr("Collection"), undecided: qsTr("Yours to decide")
    })
    // What the tidy is: the whole of it, or only "collections", the AI's
    // proposal of a collection for each book that is in none.
    property string pass: ""
    // The books in no collection there are, counted when it opens.
    property int uncollected: 0
    // "title (dune → Dune), description"
    function changed(proposal) {
        return proposal.changes.map(change => {
            const name = fieldNames[change.field] || change.field
            const short = change.from.length + change.to.length < 80 && (change.from !== "" || change.to !== "")
            return short ? name + " (" + change.from + " → " + change.to + ")" : name
        }).concat(proposal.adds.map(field => "+ " + (fieldNames[field] || field))).join(", ")
    }

    function show(what) {
        pass = what || ""
        open()
        uncollected = librarian.uncollected()
        if (pass === "collections")
            librarian.sortIntoCollections()
        else
            librarian.tidy(false)
        list.forceActiveFocus()
    }
    onClosed: librarian.stop()

    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: dialog.pass === "collections" ? qsTr("Sort into collections") : qsTr("Tidy the library")
                font.pixelSize: 18
                font.bold: true
            }
            BusyIndicator {
                visible: librarian.busy
                running: librarian.busy
                implicitWidth: 28
                implicitHeight: 28
            }
        }
        Label {
            objectName: "tidyState"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: librarian.busy ? librarian.status
                  : list.count === 0 && dialog.pass === "collections"
                    ? (librarian.hasAi ? qsTr("Every book is in a collection, or none of them fits the collections there are.") : librarian.aiProblem)
                  : list.count === 0 ? qsTr("Nothing to tidy: no duplicates, and every author is spelled one way.")
                  : dialog.pass === "collections"
                    ? qsTr("Untick what you'd rather leave. Nothing changes until you make the changes; a new collection is made then.")
                  : qsTr("Untick what you'd rather leave. A book folded into another goes to the trash; its formats move over.")
        }
        ListView {
            id: list
            objectName: "tidyList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: librarian.proposals
            ScrollBar.vertical: ScrollBar {}
            Keys.onSpacePressed: if (currentIndex >= 0) librarian.setChosen(currentIndex, !librarian.proposals[currentIndex].chosen)

            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                width: list.width
                height: body.implicitHeight + 16
                radius: Math.min(4, theme.radius)
                color: list.currentIndex === index && list.activeFocus ? theme.colors.selection : "transparent"

                RowLayout {
                    id: body
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 12

                    CheckBox {
                        objectName: "tidyChosen"
                        // Nothing to apply to a book left to the person.
                        enabled: row.modelData.kind !== "undecided"
                        checked: row.modelData.chosen
                        focusPolicy: Qt.NoFocus
                        onToggled: librarian.setChosen(row.index, checked)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        // The title, who it is by and what kind of change this
                        // is in one line; the title on a line of its own where
                        // the window is too narrow for the three.
                        GridLayout {
                            Layout.fillWidth: true
                            columns: win.compact ? 2 : 3
                            columnSpacing: 8
                            rowSpacing: 2
                            Label {
                                text: row.modelData.title
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.columnSpan: win.compact ? 2 : 1
                                Layout.fillWidth: win.compact
                                Layout.maximumWidth: win.compact ? Number.POSITIVE_INFINITY : 380
                            }
                            Label { text: row.modelData.authors; color: theme.colors.muted; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label { text: dialog.kinds[row.modelData.kind] || row.modelData.kind; color: theme.accent; font.pixelSize: 12 }
                        }
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            color: theme.colors.orange
                            text: row.modelData.kind === "duplicate"
                                  ? qsTr("Another copy (%1) folds into this one (%2).").arg(row.modelData.otherFormats).arg(row.modelData.formats)
                                  : row.modelData.kind === "collection" && row.modelData.isNew ? qsTr("Put in %1, a new collection").arg(row.modelData.collection)
                                  : row.modelData.kind === "collection" ? qsTr("Put in %1").arg(row.modelData.collection)
                                  : row.modelData.kind === "undecided" ? qsTr("Not clear where it belongs; put it in a collection yourself with %1.").arg((custom.keyText["library.collection_book"] || "").split("  ")[0])
                                  : dialog.changed(row.modelData)
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: text !== ""
                            text: row.modelData.why
                            textFormat: Text.PlainText
                            color: theme.colors.muted
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }
                }
                TapHandler { onTapped: list.currentIndex = row.index }
            }
        }
        DialogActions {
            AppButton {
                objectName: "tidyLookUps"
                visible: librarian.hasAi && librarian.incomplete() > 0
                enabled: !librarian.busy
                text: qsTr("Also look up %1 with missing details").arg(librarian.incomplete())
                onClicked: librarian.tidy(true)
            }
            AppButton {
                objectName: "tidyCollections"
                visible: librarian.hasAi && dialog.uncollected > 0 && dialog.pass !== "collections"
                enabled: !librarian.busy
                text: qsTr("Also sort %Ln books into collections", "", dialog.uncollected)
                // Once asked, the button has done its part.
                onClicked: {
                    dialog.uncollected = 0
                    librarian.sortIntoCollections()
                }
            }
            AppButton { text: qsTr("Close"); onClicked: dialog.close() }
            AppButton {
                objectName: "tidyApply"
                primary: true
                enabled: !librarian.busy && librarian.chosen > 0
                text: qsTr("Make %1 changes").arg(librarian.chosen)
                onClicked: {
                    librarian.applyChosen()
                    dialog.close()
                }
            }
        }
    }
}
