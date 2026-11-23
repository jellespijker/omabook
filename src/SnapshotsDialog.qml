import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The library's snapshots (#185, ADR 0014), from the settings and the
// library's menu: what is wrong with them first, in red, with what to do;
// where they are kept and the newest in each place; Snapshot Now; the list,
// filtered, each to compare with now or another, or to restore as a whole
// into a new folder; the diff, and a book put back from it after asking; and
// the destinations. Snapshots not set up yet are set up here, the recovery
// key shown once.
AppPopup {
    id: dialog
    objectName: "snapshots"
    widest: 720

    // What is shown: "list", "diff" (of `chosen`), "whole" (`chosen` restored
    // into a new folder) or "destinations".
    property string page: "list"
    property var chosen: ({})

    function compare(snapshot, to) {
        chosen = snapshot
        page = "diff"
        snapshotDesk.compare(snapshot.id, to || "")
    }
    function restoreWhole(snapshot) {
        chosen = snapshot
        page = "whole"
    }
    function back() {
        snapshotDesk.forget()
        page = "list"
    }

    onAboutToShow: {
        page = "list"
        snapshotDesk.forget()
        snapshotDesk.refresh()
    }
    onClosed: snapshotDesk.forget()

    SnapshotWords { id: snapshotWords }

    DialogBody {
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            AppButton {
                objectName: "snapshotsBack"
                visible: dialog.page !== "list"
                text: qsTr("Back")
                onClicked: dialog.back()
            }
            Label {
                Layout.fillWidth: true
                text: dialog.page === "diff" ? qsTr("What changed")
                    : dialog.page === "whole" ? qsTr("Restore the whole library")
                    : dialog.page === "destinations" ? qsTr("Where else they are kept")
                    : qsTr("Snapshots")
                font.pixelSize: 16
                font.bold: true
                wrapMode: Text.Wrap
            }
        }
        // What is wrong, each with what to do about it (#184).
        Repeater {
            model: snapshotDesk.warnings
            Label {
                objectName: "snapshotWarning"
                required property string modelData
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.red
                font.bold: true
                text: modelData
            }
        }
        Thinking {
            objectName: "snapshotsWorking"
            active: snapshotDesk.doing !== ""
            room: dialog.availableWidth
            text: snapshotDesk.doing === "take" ? qsTr("Taking a snapshot…")
                : snapshotDesk.doing === "setup" ? qsTr("Setting up snapshots…")
                : snapshotDesk.doing === "diff" ? qsTr("Comparing…")
                : snapshotDesk.doing === "preview" ? qsTr("Looking at the book in the snapshot…")
                : snapshotDesk.doing === "restore" ? qsTr("Taking a moment snapshot, then putting it back…")
                : snapshotDesk.doing === "whole" ? qsTr("Restoring the library…")
                : snapshotDesk.doing === "add" ? qsTr("Making the destination's repository…")
                : snapshotDesk.doing === "test" ? qsTr("Reaching the destination…")
                : qsTr("Listing the snapshots…")
        }
        Label {
            objectName: "snapshotsStatus"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: snapshotDesk.failed ? theme.colors.red : theme.foreground
            text: snapshotDesk.status
        }
        // A book just put back, and the way to undo it.
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.page === "diff" && snapshotDesk.restored.undo !== undefined && !snapshotDesk.failed
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.muted
                text: qsTr("Undo puts it back as it was just before, from that moment snapshot.")
            }
            AppButton {
                objectName: "snapshotUndo"
                enabled: snapshotDesk.doing === ""
                text: qsTr("Undo")
                onClicked: snapshotDesk.undoRestore()
            }
        }
        SnapshotKey { Layout.fillWidth: true }

        SnapshotOverview {
            Layout.fillWidth: true
            visible: dialog.page === "list"
            words: snapshotWords
            onCompared: (snapshot) => dialog.compare(snapshot, "")
            onWhole: (snapshot) => dialog.restoreWhole(snapshot)
        }
        SnapshotDiff {
            Layout.fillWidth: true
            visible: dialog.page === "diff"
            words: snapshotWords
            from: dialog.chosen
        }
        SnapshotWhole {
            Layout.fillWidth: true
            visible: dialog.page === "whole"
            words: snapshotWords
            snapshot: dialog.chosen
            onOpened: dialog.close()
        }
        SnapshotDestinationsPage {
            Layout.fillWidth: true
            visible: dialog.page === "destinations"
            words: snapshotWords
        }

        actions: [
            AppButton {
                objectName: "openDestinations"
                visible: dialog.page === "list" && snapshotDesk.ready
                text: qsTr("Destinations")
                onClicked: {
                    snapshotDesk.forget()
                    dialog.page = "destinations"
                }
            },
            AppButton {
                objectName: "snapshotNow"
                visible: dialog.page === "list" && snapshotDesk.ready
                enabled: snapshotDesk.doing === ""
                text: qsTr("Snapshot Now")
                onClicked: snapshotDesk.takeNow()
            },
            AppButton {
                objectName: "snapshotsDone"
                text: qsTr("Done")
                primary: true
                onClicked: dialog.close()
            }
        ]
    }
}
