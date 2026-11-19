import QtQuick
import QtQuick.Window

// While a library in an older format is brought up to date, before the
// window opens it (#54): a small window that says so, rather than nothing at
// all for as long as that takes on a large library.
Window {
    width: 460
    height: 140
    visible: true
    title: "Omabook"
    color: theme.background

    Text {
        objectName: "upgradeMessage"
        anchors.centerIn: parent
        width: parent.width - 48
        visible: upgrade.running
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("Bringing your library up to date…")
        color: theme.foreground
        font.pixelSize: 16
    }
}
