import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The whole library as a snapshot holds it, its books' files with it,
// restored into a new folder (#185, #183): a library of its own, never over
// the one in use, which stays as it is. Once restored it can be opened.
ColumnLayout {
    id: page
    required property SnapshotWords words
    property var snapshot: ({})
    signal opened()
    spacing: 8

    // Beside the library in use, named for the day the snapshot was taken.
    readonly property string suggested: snapshot.time
                                        ? "%1 %2".arg(backend.libraryPath).arg(new Date(snapshot.time).toISOString().slice(0, 10))
                                        : ""
    readonly property bool done: snapshotDesk.restoredInto !== "" && snapshotDesk.restoredInto === folder.text.trim()
    onSuggestedChanged: folder.text = suggested
    onVisibleChanged: if (visible) folder.text = suggested

    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("The library as the snapshot of %1 holds it, its books' files with it, restored into a new folder: a library of its own. The library in use stays as it is.")
                  .arg(page.words.named(page.snapshot))
    }
    Label { text: qsTr("Into the folder") }
    TextField {
        id: folder
        objectName: "snapshotWholeFolder"
        Layout.fillWidth: true
        selectByMouse: true
        Material.roundedScale: theme.radius
        onAccepted: if (restore.enabled) restore.clicked()
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        text: qsTr("It must be new, or empty, and not inside the library in use.")
    }
    DialogActions {
        AppButton {
            objectName: "snapshotOpenRestored"
            visible: page.done
            text: qsTr("Open It")
            onClicked: {
                backend.useLibrary(snapshotDesk.restoredInto)
                page.opened()
            }
        }
        AppButton {
            id: restore
            objectName: "snapshotWholeRestore"
            visible: !page.done
            enabled: snapshotDesk.doing === "" && folder.text.trim() !== ""
            primary: true
            text: qsTr("Restore")
            onClicked: snapshotDesk.restoreWhole(page.snapshot.id, folder.text)
        }
    }
}
