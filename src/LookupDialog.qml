import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What the catalogs and the AI say a book is, side by side: pick one to take
// its details and cover.
AppPopup {
    id: dialog
    objectName: "lookup"
    widest: 680
    tallest: 640

    readonly property var fieldNames: ({
        title: qsTr("title"), authors: qsTr("authors"), series: qsTr("series"), publisher: qsTr("publisher"),
        published: qsTr("date"), language: qsTr("language"), isbn: qsTr("ISBN"), description: qsTr("description"),
        tags: qsTr("tags"), cover: qsTr("cover")
    })
    // "cover, description, series"
    function added(candidate) { return candidate.adds.map(field => fieldNames[field] || field).join(", ") }
    // "title (dune → Dune), description": short values are shown, long ones named.
    function changed(candidate) {
        return candidate.changes.map(change => {
            const name = fieldNames[change.field] || change.field
            const short = change.to !== "" && change.from.length + change.to.length < 70
            return short ? name + " (" + change.from + " → " + change.to + ")" : name
        }).join(", ")
    }

    function show(id) {
        open()
        backend.lookUp(id)
        list.forceActiveFocus()
    }
    function apply(index) {
        backend.applyCandidate(index)
        close()
    }
    onClosed: backend.stopLookUp()

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
                text: qsTr("Look up details")
                font.pixelSize: 18
                font.bold: true
            }
            BusyIndicator {
                visible: backend.searching
                running: backend.searching
                implicitWidth: 28
                implicitHeight: 28
            }
        }
        // Every source that is asked, and how it answered.
        Flow {
            objectName: "lookupSources"
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: backend.lookUpSources
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    width: chipText.implicitWidth + 16
                    height: chipText.implicitHeight + 8
                    radius: Math.min(4, theme.radius)
                    color: "transparent"
                    border.color: chip.modelData.problem !== "" ? theme.colors.orange
                                  : chip.modelData.found > 0 ? theme.accent : theme.colors.selection
                    Label {
                        id: chipText
                        anchors.centerIn: parent
                        font.pixelSize: 12
                        color: chip.modelData.asking || chip.modelData.found === 0 ? theme.colors.muted : theme.foreground
                        text: chip.modelData.asking ? qsTr("%1 …").arg(chip.modelData.name)
                              : chip.modelData.problem !== "" ? qsTr("%1: failed").arg(chip.modelData.name)
                              : chip.modelData.found === 0 ? qsTr("%1: nothing").arg(chip.modelData.name)
                              : qsTr("%1: %2").arg(chip.modelData.name).arg(chip.modelData.found)
                    }
                }
            }
        }
        Label {
            objectName: "lookupState"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: backend.candidates.length > 0 ? qsTr("Pick the one that is this book. What it brings replaces the book's own; the rest stays.")
                  : backend.searching ? qsTr("Asking…")
                  : qsTr("Nothing was found for this book.")
        }
        ListView {
            id: list
            objectName: "lookupList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: backend.candidates
            ScrollBar.vertical: ScrollBar {}
            Keys.onReturnPressed: if (currentIndex >= 0 && count > 0) dialog.apply(currentIndex)
            Keys.onEnterPressed: if (currentIndex >= 0 && count > 0) dialog.apply(currentIndex)

            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                width: list.width
                height: Math.max(96, details.implicitHeight + 16)
                radius: Math.min(4, theme.radius)
                color: list.currentIndex === index ? theme.colors.selection : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 12

                    Rectangle {
                        Layout.preferredWidth: 54
                        Layout.preferredHeight: 80
                        Layout.alignment: Qt.AlignTop
                        color: theme.colors.selection
                        Image {
                            anchors.fill: parent
                            source: row.modelData.cover
                            asynchronous: true
                            fillMode: Image.PreserveAspectCrop
                            sourceSize.width: 108
                        }
                    }
                    ColumnLayout {
                        id: details
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 2
                        Label { Layout.fillWidth: true; text: row.modelData.title; font.bold: true; elide: Text.ElideRight }
                        Label { Layout.fillWidth: true; visible: text !== ""; text: row.modelData.authors; elide: Text.ElideRight }
                        Label {
                            Layout.fillWidth: true
                            visible: text !== ""
                            text: [row.modelData.series, row.modelData.publisher, row.modelData.published].filter(part => !!part).join(" · ")
                            color: theme.colors.muted
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: text !== ""
                            text: row.modelData.description
                            textFormat: Text.PlainText
                            color: theme.colors.muted
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        // The AI's reason for its choice.
                        Label {
                            objectName: "candidateWhy"
                            Layout.fillWidth: true
                            visible: text !== ""
                            text: row.modelData.why
                            textFormat: Text.PlainText
                            color: theme.accent
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                        // What taking it would do to the book.
                        Label {
                            objectName: "candidateAdds"
                            Layout.fillWidth: true
                            visible: row.modelData.adds.length > 0
                            text: qsTr("Adds: %1").arg(dialog.added(row.modelData))
                            textFormat: Text.PlainText
                            color: theme.colors.green
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                        Label {
                            objectName: "candidateChanges"
                            Layout.fillWidth: true
                            visible: row.modelData.changes.length > 0
                            text: qsTr("Changes: %1").arg(dialog.changed(row.modelData))
                            textFormat: Text.PlainText
                            color: theme.colors.orange
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: row.modelData.adds.length === 0 && row.modelData.changes.length === 0
                            text: qsTr("Nothing the book doesn't already have.")
                            color: theme.colors.muted
                            font.pixelSize: 12
                        }
                    }
                    Label {
                        Layout.alignment: Qt.AlignTop
                        text: row.modelData.source
                        color: theme.accent
                        font.pixelSize: 12
                    }
                }
                TapHandler {
                    onTapped: list.currentIndex = row.index
                    onDoubleTapped: dialog.apply(row.index)
                }
            }
        }
        // A catalog that couldn't be reached is said, not hidden.
        Label {
            objectName: "lookupProblem"
            Layout.fillWidth: true
            visible: text !== ""
            text: backend.searchProblem
            color: theme.colors.orange
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        DialogActions {
            AppButton { text: qsTr("Cancel"); onClicked: dialog.close() }
            AppButton {
                text: qsTr("Use These Details")
                primary: true
                enabled: list.count > 0 && list.currentIndex >= 0
                onClicked: dialog.apply(list.currentIndex)
            }
        }
    }
}
