import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The snapshots' first page (#185): setting them up, or where they are kept
// and the newest in each place, whether that is three copies on two kinds of
// medium with one away from home, and the list of them with a filter. Up and
// Down move along the list; Return compares the one there with now.
ColumnLayout {
    id: overview
    required property SnapshotWords words
    signal compared(var snapshot)
    signal whole(var snapshot)
    spacing: 8

    // The snapshots the filter lets through: words of when or why, and a kind.
    property string kindShown: ""
    readonly property var shown: {
        const typed = filter.text.trim().toLowerCase()
        return snapshotDesk.snapshots.filter(snapshot => (kindShown === "" || snapshot.kind === kindShown)
            && (typed === "" || words.named(snapshot).toLowerCase().includes(typed) || snapshot.id.startsWith(typed)))
    }
    readonly property var newest: snapshotDesk.snapshots.length > 0 ? snapshotDesk.snapshots[0] : null

    // A place they are kept, by its name, and how it stands there.
    component Place: GridLayout {
        id: place
        property string name
        default property alias detail: holder.data
        Layout.fillWidth: true
        columns: win.compact ? 1 : 2
        columnSpacing: 16
        rowSpacing: 2
        Label {
            Layout.preferredWidth: win.compact ? -1 : 160
            Layout.fillWidth: win.compact
            Layout.alignment: Qt.AlignTop
            text: place.name
            font.bold: true
            elide: Text.ElideRight
        }
        ColumnLayout {
            id: holder
            Layout.fillWidth: true
            spacing: 2
        }
    }
    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
    }

    // Not set up yet: where to keep them, and the recovery key.
    Note {
        visible: !snapshotDesk.ready
        color: theme.foreground
        text: qsTr("Snapshots aren't set up on this computer yet. Each keeps the library as it was at one moment, encrypted and read-only, to go back to; each stores only what changed since the last.")
    }
    TextField {
        id: repository
        objectName: "snapshotRepository"
        visible: !snapshotDesk.ready
        Layout.fillWidth: true
        placeholderText: snapshotDesk.repository
        selectByMouse: true
        Material.roundedScale: theme.radius
    }
    Note {
        visible: !snapshotDesk.ready
        text: qsTr("A folder, or anything restic takes (sftp:host:/path, s3:…, rclone:…); the folder shown when left empty. A recovery key is made and shown once: without it nothing can be restored.")
    }
    AppButton {
        objectName: "snapshotSetUp"
        visible: !snapshotDesk.ready
        enabled: snapshotDesk.doing === ""
        primary: true
        text: qsTr("Set Up Snapshots")
        onClicked: snapshotDesk.setUp(repository.text)
    }

    // Where they are kept, and the newest in each place.
    Label { visible: snapshotDesk.ready; text: qsTr("Where they are kept"); font.bold: true }
    ColumnLayout {
        objectName: "snapshotPlaces"
        visible: snapshotDesk.ready
        Layout.fillWidth: true
        spacing: 6
        Place {
            name: qsTr("This computer")
            Label {
                objectName: "snapshotNewest"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: overview.newest ? qsTr("Newest: %1").arg(overview.words.named(overview.newest))
                    : snapshotDesk.listed ? qsTr("None yet: Snapshot Now takes the first.") : ""
            }
        }
        Repeater {
            model: snapshotDesk.destinations
            delegate: Place {
                id: place
                required property var modelData
                name: modelData.name
                SnapshotDestinationState {
                    Layout.fillWidth: true
                    destination: place.modelData
                    words: overview.words
                }
            }
        }
    }
    Note {
        objectName: "snapshotBackedUp"
        visible: snapshotDesk.ready
        color: snapshotDesk.backedUp ? theme.colors.muted : theme.colors.orange
        text: snapshotDesk.backedUp ? qsTr("Backed up: three copies, on two kinds of medium, one away from home.")
                                    : qsTr("Not backed up yet as three copies on two kinds of medium, one away from home: add a second disk and a place away from home under Destinations.")
    }

    // The list, with its filter.
    GridLayout {
        visible: snapshotDesk.ready
        Layout.fillWidth: true
        Layout.topMargin: 8
        columns: win.compact ? 1 : 2
        columnSpacing: 8
        TextField {
            id: filter
            objectName: "snapshotFilter"
            Layout.fillWidth: true
            placeholderText: qsTr("Filter by date, reason or id")
            selectByMouse: true
            Material.roundedScale: theme.radius
            Keys.onDownPressed: list.forceActiveFocus()
        }
        ComboBox {
            objectName: "snapshotKinds"
            Layout.fillWidth: win.compact
            model: [qsTr("All snapshots"), qsTr("Nightly ones"), qsTr("Moment snapshots")]
            Material.roundedScale: theme.radius
            onActivated: (index) => overview.kindShown = ["", "nightly", "moment"][index]
        }
    }
    Note {
        objectName: "snapshotsNone"
        visible: snapshotDesk.ready && snapshotDesk.listed && overview.shown.length === 0
        text: snapshotDesk.snapshots.length === 0 ? qsTr("No snapshots of this library yet.") : qsTr("No snapshot fits the filter.")
    }
    ListView {
        id: list
        objectName: "snapshotList"
        visible: snapshotDesk.ready && overview.shown.length > 0
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 300)
        clip: true
        model: overview.shown
        keyNavigationEnabled: true
        Keys.onReturnPressed: if (currentIndex >= 0) overview.compared(overview.shown[currentIndex])
        Keys.onEnterPressed: if (currentIndex >= 0) overview.compared(overview.shown[currentIndex])
        delegate: SnapshotRow {
            width: list.width
            current: ListView.isCurrentItem && list.activeFocus
            words: overview.words
            onCompared: (snapshot) => overview.compared(snapshot)
            onWhole: (snapshot) => overview.whole(snapshot)
            onPicked: list.currentIndex = index
        }
    }
}
