import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Whether the library is served to phones and e-readers (#177): by whom
// (this window, the background service, or omabook serve in a terminal),
// where, on which port, and how many devices asked lately; or why not.
Label {
    readonly property var options: devices.options
    readonly property string where: options.serve === "localhost" ? qsTr("on this computer only")
                                    : options.serve === "tailnet" ? qsTr("on your tailnet")
                                    : qsTr("on your home network, and every other network this computer is on")
    readonly property string lately: qsTr("%n devices asked it for something in the last day.", "", devices.devicesLately)
    Layout.fillWidth: true
    wrapMode: Text.Wrap
    color: devices.problem !== "" && devices.servedBy === "" ? theme.colors.orange : theme.foreground
    text: options.serve === "off" ? qsTr("Not served: phones and e-readers can't reach the library.")
        : devices.servedBy === "window" ? qsTr("This window serves the library %1, on port %2.").arg(where).arg(options.port) + " " + lately
        : devices.servedBy === "service" ? qsTr("The background service serves the library %1, on port %2, the window open or closed.").arg(where).arg(options.port) + " " + lately
        : devices.servedBy === "command" ? qsTr("omabook serve, in a terminal, serves the library %1, on port %2.").arg(where).arg(options.port) + " " + lately
        : devices.problem !== "" ? devices.problem
        : qsTr("Starting…")
}
