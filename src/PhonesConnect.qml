import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// Connecting a phone or an e-reader app to the library's catalog (#177): its
// address, by this computer's name on the tailnet and by its number, the
// user name and the password (the catalog's token), each to copy, and QR
// codes to scan: of the address, and of the address with the token in it
// for an app that takes no password. Then the apps that read it, by name,
// and where each one adds a catalog. This computer's in the Phones and
// e-readers panel, the hub's in the Hubs panel (#193): whose it is is given.
ColumnLayout {
    id: connect
    objectName: "phonesConnect"
    spacing: 8

    // Where devices reach the catalog, by number and by name on the tailnet, and its token.
    required property string address
    required property string namedAddress
    required property string token
    // How the name on the tailnet is said, and what the parts' names begin with.
    property string namedCaption: qsTr("Catalog, by this computer's name on your tailnet:")
    property string names: "phones"
    signal renewWanted()

    // The address a device is best given: by name where there is one.
    readonly property string base: namedAddress !== "" ? namedAddress : address
    readonly property string catalog: base !== "" ? base + "/opds" : ""
    readonly property string tokened: base !== "" && token !== "" ? base + "/t/" + token + "/opds" : ""
    readonly property string userName: "omabook"

    PlainCopy { id: copier }

    // A thing to type into an app, with its Copy button.
    component CopyRow: RowLayout {
        id: row
        property string caption
        property string value
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            textFormat: Text.PlainText
            text: row.caption + " " + row.value
        }
        AppButton {
            text: qsTr("Copy")
            onClicked: copier.copyText(row.value)
        }
    }
    // A QR code with what it holds said under it.
    component Code: ColumnLayout {
        id: code
        property alias text: view.text
        property alias viewName: view.objectName
        property string caption
        spacing: 4
        Layout.preferredWidth: 184
        QrView {
            id: view
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 176
            Layout.preferredHeight: 176
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            color: theme.colors.muted
            text: code.caption
        }
    }
    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
    }

    Label { text: qsTr("Connect a phone or an e-reader app"); font.bold: true; Layout.topMargin: 8 }
    CopyRow {
        objectName: connect.names + "NamedAddress"
        visible: connect.namedAddress !== ""
        caption: connect.namedCaption
        value: connect.namedAddress + "/opds"
    }
    CopyRow {
        objectName: connect.names + "Address"
        caption: connect.namedAddress !== "" ? qsTr("Or by its address:") : qsTr("Catalog:")
        value: connect.address + "/opds"
    }
    CopyRow { objectName: connect.names + "User"; caption: qsTr("User name (any will do):"); value: connect.userName }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        CopyRow { objectName: connect.names + "Token"; caption: qsTr("Password:"); value: connect.token }
        AppButton {
            objectName: connect.names === "phones" ? "renewToken" : connect.names + "RenewToken"
            text: qsTr("New Password")
            onClicked: connect.renewWanted()
        }
    }
    Note { text: qsTr("A new password locks out every app that has the old one, until it is given the new one.") }
    Flow {
        Layout.fillWidth: true
        spacing: 16
        Code {
            viewName: connect.names + "CatalogCode"
            text: connect.catalog
            caption: qsTr("The catalog: the app asks for the user name and the password")
        }
        Code {
            viewName: connect.names + "TokenCode"
            text: connect.tokened
            caption: qsTr("The catalog with the password in it, for an app that takes none")
        }
    }

    Label { text: qsTr("Apps that read it"); font.bold: true; Layout.topMargin: 8 }
    Note {
        objectName: connect.names + "Apps"
        color: theme.foreground
        textFormat: Text.PlainText
        text: [qsTr("KOReader, on Android and on Kobo, PocketBook and Kindle e-readers: in the file browser's Search menu, OPDS catalog, then + to add the address, the user name and the password. It syncs your reading place too (below)."),
               qsTr("Librera, on Android: in Network, OPDS, add a catalog with the address, the user name and the password."),
               qsTr("Moon+ Reader, on Android: in Net Library, add a catalog with the address, the user name and the password."),
               qsTr("Any other OPDS reader: the address with the password in it, if it asks for none.")].join("\n\n")
    }
}
