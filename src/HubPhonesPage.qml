import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// Phones and e-readers through the hub (#193), in the Hubs panel: the hub
// serves one of its libraries itself, the OPDS catalog and KOReader's
// progress sync, always on, with no computer awake. Chosen here and kept on
// the hub (where it may be reached, off until chosen; its port; what it
// serves; KOReader's name and password), each change sent with Apply; then
// how a phone connects, by the hub's address, as the Phones and e-readers
// panel shows this computer's (PhonesConnect).
ColumnLayout {
    id: page
    objectName: "hubPhonesPage"
    spacing: 8

    // The hub, as HubDesk shows it; its "devices" as the hub answers.
    property var hub: null
    property bool busy: false
    readonly property var devices: hub && hub.devices ? hub.devices : ({})
    readonly property var options: devices.options || ({})
    readonly property var libraries: devices.libraries || []
    readonly property string address: devices.address || ""
    readonly property string named: devices.named || ""
    readonly property string reachable: named !== "" ? named : address
    readonly property var reaches: [
        { key: "off", name: qsTr("Serve to e-readers and phones: off") },
        { key: "localhost", name: qsTr("Serve to e-readers and phones: on the hub only") },
        { key: "tailnet", name: qsTr("Serve to e-readers and phones: on my tailnet") },
        { key: "network", name: qsTr("Serve to e-readers and phones: on my home network, and every other network the hub is on") }
    ]

    // What is chosen here, before Apply sends it.
    property string reach: "off"
    property string library: ""
    property bool opds: true
    property bool koreader: false

    // The choices as the hub has them.
    function begin() {
        reach = options.serve || "off"
        library = options.library || ""
        opds = options.opds !== false
        koreader = options.koreader_serve === true
        port.text = options.port !== undefined ? String(options.port) : ""
        user.text = options.koreader_serve_user || ""
        password.text = ""
    }
    function apply() {
        hubsDesk.setDevices(hub.host, { serve: reach, port: port.text.trim(), opds: opds, koreader_serve: koreader,
                                        koreader_serve_user: user.text.trim(), library: library }, password.text, false)
        password.text = ""
    }

    PlainCopy { id: copier }
    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
    }
    component Line: TextField {
        Layout.fillWidth: true
        selectByMouse: true
        Material.roundedScale: theme.radius
    }

    Label {
        objectName: "hubPhonesStatus"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: page.devices.problem ? theme.colors.orange : theme.foreground
        text: page.devices.problem ? (page.devices.said || "")
            : page.address === "" ? qsTr("Not served: phones and e-readers can't reach the hub's books.")
            : qsTr("The hub serves the library %1 on port %2, with no computer awake.").arg(page.options.library).arg(page.options.port)
              + " " + qsTr("%n devices asked it for something in the last day.", "", page.devices.devices || 0)
    }
    ComboBox {
        objectName: "hubPhonesReach"
        Layout.fillWidth: true
        model: page.reaches
        textRole: "name"
        currentIndex: Math.max(0, page.reaches.findIndex(entry => entry.key === page.reach))
        Material.roundedScale: theme.radius
        onActivated: (index) => page.reach = page.reaches[index].key
    }
    Note {
        visible: page.reach !== "off"
        color: page.reach === "network" ? theme.colors.orange : theme.colors.muted
        text: page.reach === "network"
              ? qsTr("Careful: anyone on a network the hub is on who has the password can download your books, and they cross it unencrypted. Your tailnet is safer, where the device can join it.")
              : qsTr("Books a plugin keeps private are never served.")
    }
    RowLayout {
        visible: page.reach !== "off"
        Layout.fillWidth: true
        spacing: 8
        Label { text: qsTr("Port:") }
        Line {
            id: port
            objectName: "hubPhonesPort"
            Layout.fillWidth: false
            Layout.preferredWidth: 96
            inputMethodHints: Qt.ImhDigitsOnly
        }
    }
    ComboBox {
        objectName: "hubPhonesLibrary"
        visible: page.reach !== "off" && page.libraries.length > 1
        Layout.fillWidth: true
        model: page.libraries
        currentIndex: Math.max(0, page.libraries.indexOf(page.library))
        Material.roundedScale: theme.radius
        onActivated: (index) => page.library = page.libraries[index]
    }
    CheckBox {
        objectName: "hubPhonesOpds"
        visible: page.reach !== "off"
        Layout.fillWidth: true
        text: qsTr("An OPDS catalog")
        checked: page.opds
        onToggled: page.opds = checked
    }
    CheckBox {
        objectName: "hubPhonesKoreader"
        visible: page.reach !== "off"
        Layout.fillWidth: true
        text: qsTr("KOReader's progress sync")
        checked: page.koreader
        onToggled: page.koreader = checked
    }
    GridLayout {
        visible: page.reach !== "off" && page.koreader
        Layout.fillWidth: true
        columns: win.compact ? 1 : 2
        columnSpacing: 8
        Line { id: user; objectName: "hubPhonesUser"; placeholderText: qsTr("The name KOReader logs in with") }
        Line {
            id: password
            objectName: "hubPhonesPassword"
            echoMode: TextInput.Password
            placeholderText: page.devices.password ? qsTr("Kept on the hub; type another to replace it") : qsTr("Password")
        }
    }
    AppButton {
        objectName: "hubPhonesApply"
        enabled: !page.busy
        primary: true
        text: qsTr("Apply")
        onClicked: page.apply()
    }

    PhonesConnect {
        Layout.fillWidth: true
        visible: page.address !== "" && page.options.opds === true
        names: "hubPhones"
        address: page.address
        namedAddress: page.named
        token: page.devices.token || ""
        namedCaption: qsTr("Catalog, by the hub's name on your tailnet:")
        onRenewWanted: hubsDesk.setDevices(page.hub.host, {}, "", true)
    }
    RowLayout {
        visible: page.address !== "" && page.options.koreader_serve === true
        Layout.fillWidth: true
        Layout.topMargin: 8
        spacing: 16
        QrView {
            objectName: "hubPhonesSyncCode"
            Layout.preferredWidth: 144
            Layout.preferredHeight: 144
            text: page.reachable
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            Note {
                objectName: "hubPhonesSyncHow"
                color: theme.foreground
                textFormat: Text.PlainText
                text: qsTr("In KOReader: Tools, Progress sync, Custom sync server, then %1. Log in with the name and the password above.").arg(page.reachable)
            }
            AppButton {
                text: qsTr("Copy")
                onClicked: copier.copyText(page.reachable)
            }
        }
    }
}
