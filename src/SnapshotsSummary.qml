import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The snapshots in a line (#185), for the settings: not set up, the newest
// one, or the first warning, in red.
Label {
    Layout.fillWidth: true
    wrapMode: Text.Wrap
    readonly property var newest: snapshotDesk.snapshots.length > 0 ? snapshotDesk.snapshots[0] : null
    color: snapshotDesk.warnings.length > 0 || snapshotDesk.passwordMissing !== "" ? theme.colors.red : theme.foreground
    SnapshotWords { id: words }
    text: snapshotDesk.passwordMissing !== "" ? snapshotDesk.passwordMissing
        : !snapshotDesk.ready ? qsTr("Not set up: nothing to go back to should the library be lost, or a mistake carried to every computer.")
        : snapshotDesk.warnings.length > 0 ? snapshotDesk.warnings[0]
        : newest ? qsTr("The newest snapshot: %1.").arg(words.named(newest))
        : snapshotDesk.listed ? qsTr("No snapshot of this library yet.") : ""
}
