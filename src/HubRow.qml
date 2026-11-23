import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One linked hub in the Hubs panel (#189): where it is and what it calls this
// computer, how it stands (reachable with its Omabook, or why not), its
// libraries and computers, and what is wrong with its snapshots, in red, each
// saying what to do; and its background work (#192): the book whose graph it
// builds and how far it got, with Cancel. Its buttons beside it where there is
// room, under it where there isn't; unlinking asks first.
Rectangle {
    id: row
    required property var modelData
    required property int index
    property bool current: false
    property bool unlinking: false
    property bool busy: false
    signal picked()
    signal computersWanted()
    signal syncWanted(bool dryRun)
    signal unlinkWanted()
    signal unlinkLeft()
    signal unlinkSure()
    signal cancelWanted()

    readonly property var hub: modelData
    readonly property var libraries: hub.libraries || []
    readonly property var computers: hub.computers || []
    readonly property var warnings: hub.warnings || []
    // The job under way on the hub, when there is one.
    readonly property var working: hub.work && hub.work.running ? hub.work.running : null
    // The last sync with this hub, or its preview, when there was one.
    readonly property var synced: hubsDesk.synced.host === hub.host ? hubsDesk.synced : null

    height: parts.implicitHeight + 16
    radius: Math.min(4, theme.radius)
    color: current ? theme.colors.selection : "transparent"

    component Detail: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
    }

    GridLayout {
        id: parts
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 10
        anchors.rightMargin: 6
        columns: win.compact ? 1 : 2
        columnSpacing: 10
        rowSpacing: 6
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                objectName: "hubName"
                Layout.fillWidth: true
                text: row.hub.host
                font.bold: true
                elide: Text.ElideRight
            }
            Label {
                objectName: "hubState"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: row.hub.state === "reachable" ? theme.foreground
                     : row.hub.state === "" ? theme.colors.muted : theme.colors.orange
                text: row.hub.state === "reachable" ? qsTr("Reachable, Omabook %1. This computer is %2 there.").arg(row.hub.version).arg(row.hub.computer)
                    : row.hub.state === "unlinked" ? qsTr("The hub has taken this computer's key away: unlink it here, then link it again.")
                    : row.hub.state === "unreachable" ? row.hub.why
                    : qsTr("Not asked yet.")
            }
            Detail {
                objectName: "hubLibraries"
                visible: row.hub.state === "reachable"
                textFormat: Text.PlainText
                text: row.libraries.length === 0 ? qsTr("It keeps no library yet.")
                    : row.libraries.map(library => qsTr("%1: %Ln books", "", library.books).arg(library.name)).join(" · ")
            }
            Detail {
                objectName: "hubComputers"
                visible: row.hub.state === "reachable" && row.computers.length > 0
                textFormat: Text.PlainText
                text: qsTr("%Ln computers linked: %1", "", row.computers.length).arg(row.computers.map(computer => computer.name).join(", "))
            }
            Detail {
                objectName: "hubSnapshotsFine"
                visible: row.hub.state === "reachable" && row.hub.snapshots !== undefined && row.warnings.length === 0
                text: row.hub.snapshots ? qsTr("Snapshots: all is well.")
                                        : qsTr("Snapshots aren't set up on this hub, so nothing is kept to go back to.")
                color: row.hub.snapshots ? theme.colors.muted : theme.colors.orange
            }
            Detail {
                objectName: "hubWork"
                visible: row.hub.state === "reachable" && row.hub.work !== undefined
                text: row.working ? qsTr("Building the graph of %1: %2 of %3 requests.").arg(row.working.title).arg(row.working.done).arg(row.working.total)
                    : row.hub.work && row.hub.work.unavailable ? qsTr("No background work: the hub has no model for quick work. %1").arg(row.hub.work.unavailable.message)
                    : row.hub.work && row.hub.work.waiting > 0 ? qsTr("%Ln books wait for their graph.", "", row.hub.work.waiting)
                    : qsTr("No background work under way.")
            }
            Repeater {
                model: row.hub.state === "reachable" ? row.warnings : []
                Label {
                    objectName: "hubWarning"
                    required property string modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    color: theme.colors.red
                    font.bold: true
                    text: modelData
                }
            }
            Label {
                objectName: "hubSynced"
                visible: row.synced !== null
                Layout.fillWidth: true
                Layout.topMargin: 4
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: row.synced === null ? ""
                    : (row.synced.dryRun ? qsTr("A sync now would do this, and nothing has changed yet:") : qsTr("Synced:"))
                      + "\n" + row.synced.lines.join("\n")
            }
            Label {
                objectName: "hubUnlinkQuestion"
                visible: row.unlinking
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: theme.colors.orange
                text: qsTr("Unlink this computer from %1? Its key there is taken away; the hub keeps its libraries, and this computer keeps its own.").arg(row.hub.host)
            }
        }
        // Syncing on one line, the hub's computers and unlinking on the next: two buttons fit a narrow window.
        Column {
            Layout.alignment: Qt.AlignTop
            spacing: 6
            z: 1
            Row {
                spacing: 6
                AppButton {
                    objectName: "hubSyncNow"
                    visible: !row.unlinking && row.hub.state === "reachable"
                    enabled: !row.busy
                    text: qsTr("Sync Now")
                    onClicked: row.syncWanted(false)
                }
                AppButton {
                    objectName: "hubSyncPreview"
                    visible: !row.unlinking && row.hub.state === "reachable"
                    enabled: !row.busy
                    text: qsTr("Preview")
                    onClicked: row.syncWanted(true)
                }
            }
            Row {
                spacing: 6
                AppButton {
                    objectName: "hubCancelWork"
                    visible: !row.unlinking && row.hub.state === "reachable" && row.working !== null
                    enabled: !row.busy
                    text: qsTr("Cancel Work")
                    onClicked: row.cancelWanted()
                }
                AppButton {
                    objectName: "hubShowComputers"
                    visible: !row.unlinking && row.hub.state === "reachable"
                    text: qsTr("Computers")
                    onClicked: row.computersWanted()
                }
                AppButton {
                    objectName: "hubUnlink"
                    visible: !row.unlinking
                    enabled: !row.busy
                    text: qsTr("Unlink…")
                    onClicked: row.unlinkWanted()
                }
                AppButton {
                    objectName: "hubUnlinkYes"
                    visible: row.unlinking
                    enabled: !row.busy
                    text: qsTr("Unlink")
                    onClicked: row.unlinkSure()
                }
                AppButton {
                    objectName: "hubUnlinkNo"
                    visible: row.unlinking
                    text: qsTr("Keep")
                    onClicked: row.unlinkLeft()
                }
            }
        }
    }
    TapHandler { onTapped: row.picked() }
}
