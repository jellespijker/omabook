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
        details: qsTr("Details looked up")
    })
    // "title (dune → Dune), description"
    function changed(proposal) {
        return proposal.changes.map(change => {
            const name = fieldNames[change.field] || change.field
            const short = change.from.length + change.to.length < 80 && (change.from !== "" || change.to !== "")
            return short ? name + " (" + change.from + " → " + change.to + ")" : name
        }).concat(proposal.adds.map(field => "+ " + (fieldNames[field] || field))).join(", ")
    }

    function show() {
        open()
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
                text: qsTr("Tidy the library")
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
                  : list.count === 0 ? qsTr("Nothing to tidy: no duplicates, and every author is spelled one way.")
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
                        checked: row.modelData.chosen
                        focusPolicy: Qt.NoFocus
                        onToggled: librarian.setChosen(row.index, checked)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label { text: row.modelData.title; font.bold: true; elide: Text.ElideRight; Layout.maximumWidth: 380 }
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
