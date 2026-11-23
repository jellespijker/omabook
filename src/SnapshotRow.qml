import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One snapshot in the list (#185): when it was taken and why, its id, and
// what can be done with it: see what changed since, or restore the whole
// library as it holds it into a new folder. Beside it where there is room,
// under it where there isn't.
Rectangle {
    id: row
    required property var modelData
    required property int index
    required property SnapshotWords words
    property bool current: false
    signal compared(var snapshot)
    signal whole(var snapshot)
    signal picked()

    height: parts.implicitHeight + 12
    radius: Math.min(4, theme.radius)
    color: current ? theme.colors.selection : "transparent"

    GridLayout {
        id: parts
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 8
        anchors.rightMargin: 6
        columns: win.compact ? 1 : 2
        columnSpacing: 8
        rowSpacing: 4
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Label {
                Layout.fillWidth: true
                text: row.words.when(row.modelData.time)
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: "%1 · %2".arg(row.words.why(row.modelData)).arg(row.modelData.id.slice(0, 8))
                color: theme.colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
        Row {
            spacing: 6
            z: 1
            AppButton {
                objectName: "snapshotCompare"
                text: qsTr("What Changed")
                enabled: snapshotDesk.doing === ""
                onClicked: row.compared(row.modelData)
            }
            AppButton {
                objectName: "snapshotRestoreWhole"
                text: qsTr("Restore…")
                enabled: snapshotDesk.doing === ""
                onClicked: row.whole(row.modelData)
            }
        }
    }
    TapHandler { onTapped: row.picked() }
}
