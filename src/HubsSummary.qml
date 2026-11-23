import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The hubs this computer is linked to (#189), in a line for the settings: by
// their names, without asking them how they stand.
Label {
    readonly property var hosts: hubsDesk.hubs.map(hub => hub.host)
    Layout.fillWidth: true
    wrapMode: Text.Wrap
    textFormat: Text.PlainText
    text: hosts.length === 0 ? qsTr("Linked to no hub.")
                             : qsTr("Linked to %Ln hubs: %1", "", hosts.length).arg(hosts.join(", "))
}
