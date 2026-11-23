import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One book of a library diff (#185): its title and authors, each detail
// that differs before and after, and its annotations counted. A book changed
// since the snapshot can be put back as the snapshot has it: what that
// changes is shown first, from the library now to the snapshot, its details
// and its annotations each to choose, and nothing is put back before Put Back.
// A moment snapshot is taken first, to undo it with.
ColumnLayout {
    id: change
    required property var modelData
    required property SnapshotWords words
    // The snapshot it is put back from, for a book changed since.
    property var snapshot: ({})
    property bool changed: false
    readonly property var book: modelData
    readonly property var asked: snapshotDesk.putBack.book && snapshotDesk.putBack.book.id === book.id ? snapshotDesk.putBack.book : null
    readonly property string notesBack: asked ? words.putBack(asked.annotations) : ""
    readonly property bool detailsBack: !!asked && asked.details.length > 0
    // Annotations that would come back or change back; those made since stay either way.
    readonly property bool notesComing: !!asked && Object.values(asked.annotations).some(count => count.added > 0 || count.changed > 0)
    readonly property bool anythingBack: detailsBack || notesComing
    spacing: 2

    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.bold: true
        text: change.book.authors.length > 0 ? qsTr("%1, by %2").arg(change.book.title).arg(change.book.authors.join(" & "))
                                             : change.book.title
    }
    Repeater {
        model: change.book.details
        Label {
            required property var modelData
            Layout.fillWidth: true
            Layout.leftMargin: 12
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: qsTr("%1: %2 → %3").arg(change.words.field(modelData.field)).arg(change.words.value(modelData.before))
                                     .arg(change.words.value(modelData.after))
        }
    }
    Label {
        visible: change.changed && text !== ""
        Layout.fillWidth: true
        Layout.leftMargin: 12
        wrapMode: Text.Wrap
        color: theme.colors.muted
        text: change.words.counts(change.book.annotations)
    }
    AppButton {
        objectName: "snapshotPutBack"
        visible: change.changed && !change.asked
        Layout.leftMargin: 12
        enabled: snapshotDesk.doing === ""
        text: qsTr("Put Back…")
        onClicked: snapshotDesk.preview(change.snapshot.id, change.book.id)
    }

    // What putting it back changes, and the asking.
    Rectangle {
        objectName: "snapshotPutBackAsk"
        visible: !!change.asked
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.topMargin: 4
        implicitHeight: asking.implicitHeight + 20
        radius: theme.radius
        color: "transparent"
        border.color: theme.colors.selection
        ColumnLayout {
            id: asking
            anchors.fill: parent
            anchors.margins: 10
            spacing: 4
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: change.anythingBack ? qsTr("Put back as the snapshot of %1 has it:").arg(change.words.named(change.snapshot))
                                          : qsTr("Nothing to put back: the book is as the snapshot has it.")
            }
            Repeater {
                model: change.asked ? change.asked.details : []
                Label {
                    required property var modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: qsTr("%1: %2 → %3").arg(change.words.field(modelData.field)).arg(change.words.value(modelData.before))
                                             .arg(change.words.value(modelData.after))
                }
            }
            Label {
                visible: change.notesBack !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: change.notesBack
            }
            CheckBox {
                id: details
                objectName: "snapshotPutBackDetails"
                visible: change.detailsBack
                Layout.fillWidth: true
                checked: true
                text: qsTr("Its details")
            }
            CheckBox {
                id: annotations
                objectName: "snapshotPutBackAnnotations"
                visible: change.notesComing
                Layout.fillWidth: true
                checked: true
                text: qsTr("Its annotations")
            }
            Label {
                visible: change.anythingBack
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.muted
                text: qsTr("A moment snapshot is taken first, to undo it with.")
            }
            DialogActions {
                AppButton {
                    objectName: "snapshotPutBackCancel"
                    text: qsTr("Cancel")
                    onClicked: snapshotDesk.forget()
                }
                AppButton {
                    objectName: "snapshotPutBackYes"
                    visible: change.anythingBack
                    enabled: snapshotDesk.doing === "" && ((change.detailsBack && details.checked)
                                                           || (change.notesComing && annotations.checked))
                    primary: true
                    text: qsTr("Put Back")
                    onClicked: snapshotDesk.restoreBook(change.snapshot.id, change.book.id,
                                                        change.detailsBack && details.checked,
                                                        change.notesComing && annotations.checked)
                }
            }
        }
    }
}
