import QtQuick
import QtQuick.Controls

// While a plugin records from the microphone (#79), a mark at the top of the
// window says so, whatever else is open: who records, for how long, how
// loud, and a button that stops it. A plugin can't record without it: the
// recording is shown here from its first moment to its last. So too while a
// field is dictated into (#148), by Omabook, a plugin's view or voxtype's own
// dictation: who listens, and Stop, which writes out what was said.
Rectangle {
    id: mark
    objectName: "recordingMark"
    // AudioDevices: backend.audio.
    property var devices: null
    // Dictation: backend.dictation.
    property var dictation: null
    readonly property bool dictating: !!dictation && dictation.listening
    readonly property bool shown: (!!devices && devices.recording) || dictating

    visible: shown
    // Above the dialogs too: a recording is shown whatever is open.
    parent: Overlay.overlay
    z: 1000
    anchors.top: parent ? parent.top : undefined
    anchors.topMargin: 8
    anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
    width: Math.min(row.implicitWidth + 24, (parent ? parent.width : 400) - 16)
    height: row.implicitHeight + 12
    radius: height / 2
    color: theme.background
    border.width: 1.5
    border.color: theme.colors.red

    function clock(milliseconds) {
        const seconds = Math.floor(milliseconds / 1000)
        const minutes = Math.floor(seconds / 60)
        const hours = Math.floor(minutes / 60)
        const two = n => (n < 10 ? "0" : "") + n
        return hours > 0 ? hours + ":" + two(minutes % 60) + ":" + two(seconds % 60) : minutes + ":" + two(seconds % 60)
    }

    Accessible.role: Accessible.AlertMessage
    Accessible.name: status.text

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 10

        // A dot that is filled while it records and a ring while it is
        // paused: the shape says it as well as the colour.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 12
            height: 12
            radius: 6
            color: mark.dictating || (mark.shown && !mark.devices.paused) ? theme.colors.red : "transparent"
            border.width: 2
            border.color: theme.colors.red
        }
        Label {
            id: status
            objectName: "recordingStatus"
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: !mark.shown ? ""
                  : mark.dictating && mark.dictation.byVoxtype ? qsTr("voxtype is listening for %1").arg(mark.dictation.who || "Omabook")
                  : mark.dictating ? qsTr("%1 is listening").arg(mark.dictation.who || "Omabook") + " · " + mark.clock(mark.dictation.elapsed)
                  : (mark.devices.paused ? qsTr("%1 paused recording") : qsTr("%1 is recording")).arg(mark.devices.recorder)
                    + " · " + mark.clock(mark.devices.elapsed)
            color: theme.foreground
        }
        // How loud it is, so it is plain that it hears.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            height: 6
            radius: 3
            color: theme.colors.selection
            Rectangle {
                width: parent.width * Math.min(1, Math.sqrt(mark.dictating ? mark.dictation.level : mark.shown ? mark.devices.level : 0))
                height: parent.height
                radius: parent.radius
                color: theme.colors.red
            }
        }
        AppButton {
            objectName: "stopRecording"
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Stop")
            // It takes nothing from the field being dictated into, which keeps the keyboard.
            focusPolicy: Qt.NoFocus
            onClicked: mark.dictating ? mark.dictation.stop() : mark.devices.stopRecording()
        }
    }
}
