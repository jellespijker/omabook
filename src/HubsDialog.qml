import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The hubs this computer is linked to (#189, ADR 0012), from the settings,
// the library's menu and its key: each hub with how it stands (reachable,
// its Omabook, its libraries, the computers linked to it, what is wrong with
// its snapshots), linking another, unlinking, and the hub's computers, each
// of the others to take off it; and syncing the library with it (#188), or
// seeing first what a sync would do; and, where a sync was refused for the
// hub's library not being paired with this computer's (#214), pairing them
// after asking (#217). All of it through the hub's session, as
// `omabook hub` asks it. Automatic sync is switched on per hub (#191), and
// allowed with the window closed too. A hub serves phones and e-readers
// itself, set up on a page of its own (#193). The keys are the panel's "hubs"
// (panelkeys.cpp).
AppPopup {
    id: dialog
    objectName: "hubs"
    widest: 680

    // What is shown: "list", "computers" or "phones" (of `chosen`, a host), or "link".
    property string page: "list"
    property string chosen: ""
    // The hub in the list the keys act on.
    property int at: 0
    // The hub whose unlinking, or the computer whose forgetting, waits for a yes.
    property string unlinking: ""
    property string forgetting: ""
    // Whether pairing the hub's library with this computer's waits for a yes (#217).
    property bool pairing: false
    readonly property var unpaired: hubsDesk.unpaired.host !== undefined && page === "list" ? hubsDesk.unpaired : null
    readonly property var current: hubsDesk.hubs.find(hub => hub.host === chosen) || null
    readonly property var picked: at >= 0 && at < hubsDesk.hubs.length ? hubsDesk.hubs[at] : null
    readonly property bool idle: hubsDesk.doing === ""

    function show(which, host) {
        unlinking = ""
        forgetting = ""
        pairing = false
        hubsDesk.clear()
        if (host !== undefined)
            chosen = host
        page = which
        if (which === "link")
            linking.begin()
        else if (which === "phones")
            phones.begin()
        if (which !== "link")
            keyboard.forceActiveFocus()
    }
    function back() {
        if (unlinking !== "" || forgetting !== "" || pairing) {
            unlinking = ""
            forgetting = ""
            pairing = false
        } else if (page === "link" && linking.step > 0) {
            linking.step = 0
        } else if (page !== "list") {
            show("list")
        } else {
            close()
        }
        if (page !== "link")
            keyboard.forceActiveFocus()
    }
    function pick(index) {
        at = Math.max(0, Math.min(hubsDesk.hubs.length - 1, index))
        list.positionViewAtIndex(at, ListView.Contain)
    }
    // The answer to a question the dialog asked: unlink, forget, or pair.
    function sure() {
        if (unlinking !== "")
            hubsDesk.unlink(unlinking)
        else if (forgetting !== "")
            hubsDesk.forget(chosen, forgetting)
        else if (pairing)
            hubsDesk.pair()
        unlinking = ""
        forgetting = ""
        pairing = false
        keyboard.forceActiveFocus()
    }

    closePolicy: Popup.CloseOnPressOutside
    onAboutToShow: {
        page = "list"
        at = 0
        unlinking = ""
        forgetting = ""
        pairing = false
        hubsDesk.clear()
        hubsDesk.refresh()
    }
    onOpened: keyboard.forceActiveFocus()

    DialogBody {
        id: keyboard
        focus: true
        // Every key of the panel. A key a text field took never gets here.
        Keys.onPressed: (event) => {
            const plain = event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier
            const asking = dialog.unlinking !== "" || dialog.forgetting !== "" || (dialog.pairing && dialog.unpaired !== null)
            const entered = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
            let taken = true
            if (event.key === Qt.Key_Escape) dialog.back()
            else if (dialog.page === "link" || dialog.page === "phones") taken = false
            else if (asking && entered) dialog.sure()
            else if (dialog.page === "computers") taken = computers.key(event)
            else if (event.key === Qt.Key_Down || (event.key === Qt.Key_J && plain)) dialog.pick(dialog.at + 1)
            else if (event.key === Qt.Key_Up || (event.key === Qt.Key_K && plain)) dialog.pick(dialog.at - 1)
            else if (entered && dialog.picked) dialog.show("computers", dialog.picked.host)
            else if (event.key === Qt.Key_N && plain && dialog.idle) dialog.show("link")
            else if (event.key === Qt.Key_R && plain && dialog.idle) hubsDesk.refresh()
            else if (event.key === Qt.Key_A && plain && dialog.idle && dialog.unpaired) dialog.pairing = true
            else if (event.key === Qt.Key_C && plain && dialog.idle && dialog.picked && dialog.picked.work
                     && dialog.picked.work.running)
                hubsDesk.cancelWork(dialog.picked.host)
            else if ((event.key === Qt.Key_S || event.key === Qt.Key_P) && plain && dialog.idle && dialog.picked
                     && dialog.picked.state === "reachable")
                hubsDesk.syncNow(dialog.picked.host, event.key === Qt.Key_P)
            else if (event.key === Qt.Key_O && plain && dialog.picked && hubsDesk.automatic && dialog.picked.state !== "unlinked")
                hubsDesk.setAutomatic(dialog.picked.host, dialog.picked.auto !== true)
            else if ((event.key === Qt.Key_Delete || (event.key === Qt.Key_D && plain)) && dialog.picked && dialog.idle)
                dialog.unlinking = dialog.picked.host
            else taken = false
            event.accepted = taken
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            AppButton {
                objectName: "hubsBack"
                visible: dialog.page !== "list"
                text: qsTr("Back")
                onClicked: dialog.back()
            }
            Label {
                Layout.fillWidth: true
                text: dialog.page === "link" ? qsTr("Link a hub")
                    : dialog.page === "computers" ? qsTr("The computers linked to %1").arg(dialog.chosen)
                    : dialog.page === "phones" ? qsTr("Phones and e-readers through %1").arg(dialog.chosen)
                    : qsTr("Hubs")
                font.pixelSize: 16
                font.bold: true
                wrapMode: Text.Wrap
            }
        }
        Thinking {
            objectName: "hubsWorking"
            active: !dialog.idle
            room: dialog.availableWidth
            text: hubsDesk.doing === "link" ? qsTr("Linking this computer to %1…").arg(hubsDesk.doingHost)
                : hubsDesk.doing === "unlink" ? qsTr("Taking this computer's key off %1…").arg(hubsDesk.doingHost)
                : hubsDesk.doing === "forget" ? qsTr("Taking the computer's key off %1…").arg(hubsDesk.doingHost)
                : hubsDesk.doing === "sync" ? qsTr("Syncing with %1…").arg(hubsDesk.doingHost)
                : hubsDesk.doing === "preview" ? qsTr("Seeing what a sync with %1 would do…").arg(hubsDesk.doingHost)
                : hubsDesk.doing === "cancel" ? qsTr("Stopping the work under way on %1…").arg(hubsDesk.doingHost)
                : hubsDesk.doing === "auto" ? qsTr("Syncing with %1 by itself…").arg(hubsDesk.doingHost)
                : hubsDesk.doing === "background" ? qsTr("Asking systemd for the background service…")
                : hubsDesk.doing === "devices" ? qsTr("Asking %1 to serve phones and e-readers as chosen…").arg(hubsDesk.doingHost)
                : qsTr("Asking the hubs…")
        }
        Label {
            objectName: "hubsStatus"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: hubsDesk.failed ? theme.colors.red : theme.foreground
            text: hubsDesk.status
        }
        // Where the hub refused a sync for its library not being paired with this computer's: pairing them, asked first.
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.unpaired !== null
            spacing: 6
            Label {
                objectName: "hubPairQuestion"
                visible: dialog.pairing
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: theme.colors.orange
                text: dialog.unpaired === null ? ""
                    : dialog.unpaired.dryRun
                      ? qsTr("See what a sync would do with the hub's library %1 paired with this computer's library %2? Nothing changes yet, and they stay unpaired until a sync pairs them.").arg(dialog.unpaired.there).arg(dialog.unpaired.here)
                      : qsTr("Pair the hub's library %1 with this computer's library %2, and sync them? Only if the two are one library: from then on each sync brings books, details and the change log both ways.").arg(dialog.unpaired.there).arg(dialog.unpaired.here)
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                AppButton {
                    objectName: "hubPair"
                    visible: !dialog.pairing
                    enabled: dialog.idle
                    text: qsTr("Pair…")
                    onClicked: dialog.pairing = true
                }
                AppButton {
                    objectName: "hubPairYes"
                    visible: dialog.pairing
                    enabled: dialog.idle
                    text: dialog.unpaired !== null && dialog.unpaired.dryRun ? qsTr("Preview Paired") : qsTr("Pair and Sync")
                    onClicked: dialog.sure()
                }
                AppButton {
                    objectName: "hubPairNo"
                    visible: dialog.pairing
                    text: qsTr("Keep Apart")
                    onClicked: dialog.back()
                }
            }
        }

        // The list of hubs.
        Label {
            objectName: "hubsNone"
            Layout.fillWidth: true
            visible: dialog.page === "list" && hubsDesk.hubs.length === 0
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("This computer is linked to no hub. A hub is an Omabook without a window that is always on: it keeps your libraries for your computers to sync with, and backs them up. Link one to see how it stands here.")
        }
        ListView {
            id: list
            objectName: "hubsList"
            visible: dialog.page === "list" && hubsDesk.hubs.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 420)
            clip: true
            spacing: 4
            model: hubsDesk.hubs
            interactive: contentHeight > height
            delegate: HubRow {
                width: list.width
                current: index === dialog.at
                unlinking: dialog.unlinking === modelData.host
                busy: !dialog.idle
                onPicked: {
                    dialog.at = index
                    keyboard.forceActiveFocus()
                }
                onComputersWanted: dialog.show("computers", modelData.host)
                onPhonesWanted: dialog.show("phones", modelData.host)
                onSyncWanted: (dryRun) => hubsDesk.syncNow(modelData.host, dryRun)
                onUnlinkWanted: dialog.unlinking = modelData.host
                onUnlinkLeft: dialog.unlinking = ""
                onUnlinkSure: dialog.sure()
                onCancelWanted: hubsDesk.cancelWork(modelData.host)
            }
        }

        // With the window closed too: the background service, once any hub syncs by itself.
        CheckBox {
            objectName: "hubsBackground"
            Layout.fillWidth: true
            visible: dialog.page === "list" && hubsDesk.automatic && hubsDesk.hubs.some(hub => hub.auto === true)
            enabled: dialog.idle
            text: qsTr("Also while the window is closed, from login on")
            checked: hubsDesk.background
            onToggled: hubsDesk.setBackground(checked)
        }

        HubComputersPage {
            id: computers
            Layout.fillWidth: true
            visible: dialog.page === "computers"
            hub: dialog.current
            forgetting: dialog.forgetting
            busy: !dialog.idle
            onForgetWanted: (name) => dialog.forgetting = name
            onForgetLeft: dialog.forgetting = ""
            onForgetSure: dialog.sure()
        }

        HubPhonesPage {
            id: phones
            Layout.fillWidth: true
            visible: dialog.page === "phones"
            hub: dialog.current
            busy: !dialog.idle
        }

        HubLinkPage {
            id: linking
            Layout.fillWidth: true
            visible: dialog.page === "link"
            busy: !dialog.idle
            // Back to the list, with how the linking went still said.
            onLinked: {
                dialog.page = "list"
                keyboard.forceActiveFocus()
            }
        }

        actions: [
            AppButton {
                objectName: "hubsRefresh"
                visible: dialog.page !== "link" && hubsDesk.hubs.length > 0
                enabled: dialog.idle
                text: qsTr("Ask Again")
                onClicked: hubsDesk.refresh()
            },
            AppButton {
                objectName: "hubsLink"
                visible: dialog.page === "list"
                enabled: dialog.idle
                text: qsTr("Link a Hub…")
                onClicked: dialog.show("link")
            },
            AppButton {
                objectName: "hubsDone"
                text: qsTr("Done")
                primary: true
                onClicked: dialog.close()
            }
        ]
    }
}
