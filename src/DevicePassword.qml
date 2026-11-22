import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A password for phones and e-readers (#71), kept in the keyring and never
// shown again: "serve" (what KOReader logs in to Omabook with), "sync"
// (Omabook's at a sync server) or "mail".
TextField {
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
