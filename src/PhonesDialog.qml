import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// Phones and e-readers (#177), from the settings and the library's menu:
// whether the library is served and by whom, where it may be reached (off
// until chosen), serving on with the window closed, connecting a phone by a
// QR code, KOReader's progress sync, what is sent, and sending a book to an
// e-reader. Each choice takes effect as it is made.
AppPopup {
    id: dialog
    objectName: "phones"
    widest: 640

    readonly property var options: devices.options
    readonly property bool serving: options.serve !== "off"
    readonly property var reaches: [
        { key: "off", name: qsTr("Serve to e-readers and phones: off") },
        { key: "localhost", name: qsTr("Serve to e-readers and phones: on this computer only") },
        { key: "network", name: qsTr("Serve to e-readers and phones: on my home network, and every other network this computer is on") },
        { key: "tailnet", name: qsTr("Serve to e-readers and phones: on my tailnet") }
    ]
    // One collection only was chosen, before its name is typed.
    property bool oneCollection: false

    onAboutToShow: {
        oneCollection = options.opds_collection !== ""
        devices.refresh()
    }
    // Who serves, and how many devices asked lately, kept up to date while it is open.
    Timer { interval: 5000; repeat: true; running: dialog.opened; onTriggered: devices.refresh() }

    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
    }
    component Field: TextField {
        property string key
        Layout.fillWidth: true
        text: devices.options[key] !== undefined ? String(devices.options[key]) : ""
        selectByMouse: true
        Material.roundedScale: theme.radius
        onEditingFinished: devices.set(key, text.trim())
    }

    DialogBody {
        PlainCopy { id: copier }
        Label { text: qsTr("Phones and e-readers"); font.pixelSize: 16; font.bold: true }
        PhonesStatus { objectName: "phonesStatus" }
        ComboBox {
            objectName: "deviceReach"
            Layout.fillWidth: true
            model: dialog.reaches
            textRole: "name"
            currentIndex: Math.max(0, dialog.reaches.findIndex(entry => entry.key === dialog.options.serve))
            Material.roundedScale: theme.radius
            onActivated: (index) => devices.set("serve", dialog.reaches[index].key)
        }
        Note {
            objectName: "deviceReachNote"
            visible: dialog.serving
            color: dialog.options.serve === "network" ? theme.colors.orange : theme.colors.muted
            text: dialog.options.serve === "network"
                  ? qsTr("Careful: anyone on a network this computer is on who has the password can download your books, and they cross it unencrypted. Your tailnet is safer, where the device can join it.")
                  : qsTr("Books a plugin keeps private are never served.")
        }
        Switch {
            objectName: "phonesBackground"
            visible: dialog.serving
            Layout.fillWidth: true
            // Tall enough for a fingertip.
            Layout.minimumHeight: 40
            text: qsTr("Serve with the window closed too")
            checked: dialog.options.serve_background
            onToggled: devices.set("serve_background", checked)
        }
        Note {
            visible: dialog.serving
            text: qsTr("A background service of your own (systemd, no administrator needed) serves from when you log in, and the window leaves serving to it.")
        }

        CheckBox {
            objectName: "deviceOpds"
            visible: dialog.serving
            Layout.fillWidth: true
            text: qsTr("An OPDS catalog")
            checked: dialog.options.opds
            onToggled: devices.set("opds", checked)
        }
        Note { visible: dialog.serving; text: qsTr("For e-reader and phone apps to browse and download books.") }
        Label { visible: dialog.serving && dialog.options.opds; text: qsTr("What to send"); font.bold: true }
        RadioButton {
            objectName: "phonesWholeLibrary"
            visible: dialog.serving && dialog.options.opds
            Layout.fillWidth: true
            text: qsTr("The whole library")
            checked: !dialog.oneCollection
            onToggled: {
                dialog.oneCollection = false
                devices.set("opds_collection", "")
            }
        }
        RadioButton {
            objectName: "phonesOneCollection"
            visible: dialog.serving && dialog.options.opds
            Layout.fillWidth: true
            text: qsTr("One collection only")
            checked: dialog.oneCollection
            onToggled: dialog.oneCollection = true
        }
        Field {
            objectName: "deviceCollection"
            visible: dialog.serving && dialog.options.opds && dialog.oneCollection
            key: "opds_collection"
            placeholderText: qsTr("The collection's name or path")
        }
        PhonesConnect {
            Layout.fillWidth: true
            visible: dialog.serving && dialog.options.opds && devices.address !== ""
            address: devices.address
            namedAddress: devices.namedAddress
            token: devices.token
            onRenewWanted: devices.renewToken()
        }

        CheckBox {
            objectName: "deviceKoreaderServe"
            visible: dialog.serving
            Layout.fillWidth: true
            Layout.topMargin: 8
            text: qsTr("KOReader's progress sync")
            checked: dialog.options.koreader_serve
            onToggled: devices.set("koreader_serve", checked)
        }
        RowLayout {
            visible: dialog.serving && dialog.options.koreader_serve
            Layout.fillWidth: true
            spacing: 8
            Field { objectName: "koreaderServeUser"; key: "koreader_serve_user"; placeholderText: qsTr("The name KOReader logs in with") }
            DevicePassword { which: "serve" }
        }
        RowLayout {
            visible: dialog.serving && dialog.options.koreader_serve && devices.address !== ""
            Layout.fillWidth: true
            spacing: 16
            QrView {
                objectName: "phonesSyncCode"
                Layout.preferredWidth: 144
                Layout.preferredHeight: 144
                text: devices.namedAddress !== "" ? devices.namedAddress : devices.address
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8
                Note {
                    objectName: "phonesSyncHow"
                    color: theme.foreground
                    textFormat: Text.PlainText
                    text: qsTr("In KOReader: Tools, Progress sync, Custom sync server, then %1. Log in with the name and the password above.")
                          .arg(devices.namedAddress !== "" ? devices.namedAddress : devices.address)
                }
                AppButton {
                    text: qsTr("Copy")
                    onClicked: copier.copyText(devices.namedAddress !== "" ? devices.namedAddress : devices.address)
                }
            }
        }

        DeviceSettings { Layout.fillWidth: true }

        actions: [
            AppButton {
                objectName: "phonesDone"
                text: qsTr("Done")
                primary: true
                onClicked: dialog.close()
            }
        ]
    }
}
