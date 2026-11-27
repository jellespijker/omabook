import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Phones and e-readers (#71), in their panel under serving (PhonesDialog):
// KOReader's progress sync with a sync server elsewhere, and where a book is
// sent: the e-reader mounted over USB, or a Kindle's address by mail.
ColumnLayout {
    id: section
    objectName: "deviceSettings"
    spacing: 12

    readonly property var options: devices.options

    // A line to type a setting into, kept when it is left.
    component Field: TextField {
        property string key
        Layout.fillWidth: true
        text: devices.options[key] !== undefined ? String(devices.options[key]) : ""
        selectByMouse: true
        Material.roundedScale: theme.radius
        onEditingFinished: {
            devices.set(key, text.trim())
            // What was kept, which a port that can't be one doesn't change.
            text = Qt.binding(() => devices.options[key] !== undefined ? String(devices.options[key]) : "")
        }
    }
    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
    }

    Label { text: qsTr("A KOReader sync server elsewhere"); font.bold: true; Layout.topMargin: 8 }
    Field { objectName: "koreaderServer"; key: "koreader_server"; placeholderText: qsTr("Its address, such as https://sync.koreader.rocks") }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Field { key: "koreader_user"; placeholderText: qsTr("Your name there") }
        DevicePassword { which: "sync" }
    }
    AppButton {
        objectName: "koreaderSyncNow"
        enabled: !devices.busy && section.options.koreader_server !== "" && devices.secrets.sync
        text: qsTr("Sync Reading Places Now")
        onClicked: devices.syncNow()
    }

    Label { text: qsTr("Sending books"); font.bold: true; Layout.topMargin: 8 }
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
        DevicePassword { which: "mail" }
    }
    Field { key: "mail_from"; placeholderText: qsTr("The address you send from, as Amazon knows it") }
    Note { text: qsTr("Amazon takes books only from an address you approved for your Kindle, and EPUB and PDF up to 50 MB.") }
    Note { objectName: "devicesStatus"; visible: text !== ""; text: devices.busy ? qsTr("Working…") : devices.status }
}
