import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// E-readers and phones (#71), in the settings: the server they talk to (off
// until chosen, and only where it is allowed to listen), KOReader's progress
// sync both ways, and where a book is sent. Each choice takes effect as it is made.
ColumnLayout {
    id: section
    objectName: "deviceSettings"
    spacing: 12

    readonly property var options: devices.options
    readonly property var reaches: [
        { key: "off", name: qsTr("Serve to e-readers and phones: off") },
        { key: "localhost", name: qsTr("Serve to e-readers and phones: on this computer only") },
        { key: "tailnet", name: qsTr("Serve to e-readers and phones: on my tailnet") },
        { key: "network", name: qsTr("Serve to e-readers and phones: on every network this computer is on") }
    ]
    readonly property bool serving: options.serve !== "off"

    // A line to type a setting into, kept when it is left.
    component Field: TextField {
        property string key
        Layout.fillWidth: true
        text: devices.options[key] !== undefined ? String(devices.options[key]) : ""
        selectByMouse: true
        Material.roundedScale: theme.radius
        onEditingFinished: devices.set(key, text.trim())
    }
    // A password, kept in the keyring and never shown again.
    component Password: TextField {
        property string which
        Layout.fillWidth: true
        echoMode: TextInput.Password
        placeholderText: devices.secrets[which] ? qsTr("Kept in the keyring; type another to replace it") : qsTr("Password")
        selectByMouse: true
        Material.roundedScale: theme.radius
        onAccepted: {
            if (text === "")
                return
            devices.setPassword(which, text)
            text = ""
        }
    }
    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
    }

    Label { text: qsTr("E-readers and phones"); font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
    ComboBox {
        objectName: "deviceReach"
        Layout.fillWidth: true
        model: section.reaches
        textRole: "name"
        currentIndex: Math.max(0, section.reaches.findIndex(entry => entry.key === section.options.serve))
        Material.roundedScale: theme.radius
        onActivated: (index) => devices.set("serve", section.reaches[index].key)
    }
    Note {
        objectName: "deviceReachNote"
        visible: section.serving
        color: devices.problem !== "" || section.options.serve === "network" ? theme.colors.orange : theme.colors.muted
        text: devices.problem !== "" ? devices.problem
            : section.options.serve === "network"
              ? qsTr("Careful: anyone on a network this computer is on who has the password can download your books, and they cross it unencrypted. Your tailnet is safer, where the device can join it.")
              : devices.address !== "" ? qsTr("Devices reach Omabook at %1. Books a plugin keeps private are never served.").arg(devices.address)
              : qsTr("Starting…")
    }
    CheckBox {
        objectName: "deviceOpds"
        visible: section.serving
        Layout.fillWidth: true
        text: qsTr("An OPDS catalog, for e-reader and phone apps to browse and download books")
        checked: section.options.opds
        onToggled: devices.set("opds", checked)
    }
    Field {
        objectName: "deviceCollection"
        visible: section.serving && section.options.opds
        key: "opds_collection"
        placeholderText: qsTr("Only this collection, by its name or path; empty for every book")
    }
    RowLayout {
        visible: section.serving && section.options.opds && devices.address !== ""
        Layout.fillWidth: true
        spacing: 8
        Note {
            objectName: "deviceCatalog"
            textFormat: Text.PlainText
            text: qsTr("Catalog: %1/opds, with any name and the password %2. An app that asks for no password: %1/t/%2/opds")
                  .arg(devices.address).arg(devices.token)
        }
        AppButton { objectName: "renewToken"; text: qsTr("New Password"); onClicked: devices.renewToken() }
    }
    CheckBox {
        objectName: "deviceKoreaderServe"
        visible: section.serving
        Layout.fillWidth: true
        text: qsTr("KOReader's progress sync: set Omabook's address as KOReader's custom sync server")
        checked: section.options.koreader_serve
        onToggled: devices.set("koreader_serve", checked)
    }
    RowLayout {
        visible: section.serving && section.options.koreader_serve
        Layout.fillWidth: true
        spacing: 8
        Field { key: "koreader_serve_user"; placeholderText: qsTr("The name KOReader logs in with") }
        Password { which: "serve" }
    }

    Label { text: qsTr("A KOReader sync server elsewhere"); font.bold: true }
    Field { objectName: "koreaderServer"; key: "koreader_server"; placeholderText: qsTr("Its address, such as https://sync.koreader.rocks") }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Field { key: "koreader_user"; placeholderText: qsTr("Your name there") }
        Password { which: "sync" }
    }
    AppButton {
        objectName: "koreaderSyncNow"
        enabled: !devices.busy && section.options.koreader_server !== "" && devices.secrets.sync
        text: qsTr("Sync Reading Places Now")
        onClicked: devices.syncNow()
    }

    Label { text: qsTr("Sending books"); font.bold: true }
    Field { objectName: "deviceFolder"; key: "device_folder"; placeholderText: qsTr("Where the e-reader is mounted, such as /run/media/you/KOBOeReader") }
    Note {
        objectName: "deviceFound"
        visible: section.options.device_folder !== ""
        text: !devices.deviceThere ? qsTr("No e-reader is mounted there now.")
            : devices.deviceKind !== "" ? qsTr("A %1 is there: books go to it in a format it reads, converted with Calibre when needed.").arg(devices.deviceKind)
            : qsTr("An e-reader is there: books go to it as EPUB where they can, converted with Calibre when needed.")
    }
    Field { objectName: "kindleAddress"; key: "kindle_address"; placeholderText: qsTr("A Kindle's Send-to-Kindle address, such as you@kindle.com") }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Field { key: "mail_host"; placeholderText: qsTr("Your mail server, such as smtp.fastmail.com") }
        Field { key: "mail_port"; Layout.fillWidth: false; Layout.preferredWidth: 80; inputMethodHints: Qt.ImhDigitsOnly }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Field { key: "mail_user"; placeholderText: qsTr("Its user name") }
        Password { which: "mail" }
    }
    Field { key: "mail_from"; placeholderText: qsTr("The address you send from, as Amazon knows it") }
    Note { text: qsTr("Amazon takes books only from an address you approved for your Kindle, and EPUB and PDF up to 50 MB.") }
    Note { objectName: "devicesStatus"; visible: text !== ""; text: devices.busy ? qsTr("Working…") : devices.status }
}
