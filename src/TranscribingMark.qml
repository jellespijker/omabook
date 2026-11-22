import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// While a recording of the library is written out (#151), a mark at the top
// of the window says which, how far it is, and has a button that stops it;
// what was written out is kept, and starting it again goes on from there.
// Its key (library.transcribe) writes out the selected book, or stops it.
// The key stays in the window, where the help finds it, while the mark is
// shown over everything; what holds them takes no room.
Item {
    id: holder
    objectName: "transcribingHolder"
    // TranscriptDesk: backend.transcripts.
    property var desk: null
    // The book selected on the shelf, which the key writes out.
    property real selected: 0
    // How far down the window it is: below a recording's or dictation's mark when there is one.
    property real top: 8
    readonly property bool shown: !!desk && desk.busy
    // The selected book is a recording, which the key writes out.
    readonly property bool recording: {
        if (!desk || selected <= 0)
            return false
        desk.revision
        return desk.state(selected).recording === true
    }

    width: 0
    height: 0

    Key {
        command: "library.transcribe"
        when: !win.inReader && holder.recording
        onActivated: holder.desk.toggle(holder.selected)
    }

    Rectangle {
        id: mark
        objectName: "transcribingMark"
        readonly property var desk: holder.desk
        readonly property bool shown: holder.shown

        visible: shown
        // Above the dialogs too, as a recording's mark is.
        parent: Overlay.overlay
        z: 999
        anchors.top: parent ? parent.top : undefined
        anchors.topMargin: holder.top
        anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
        width: Math.min(row.implicitWidth + 28, (parent ? parent.width : 400) - 16)
        height: row.implicitHeight + 12
        radius: height / 2
        color: theme.background
        border.width: 1.5
        border.color: theme.accent

        Accessible.role: Accessible.AlertMessage
        Accessible.name: status.text

        RowLayout {
            id: row
            anchors.centerIn: parent
            width: Math.min(implicitWidth, mark.width - 28)
            spacing: 10

            Label {
                id: status
                objectName: "transcribingStatus"
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth
                elide: Text.ElideRight
                // "Writing out “Lecture 3”…", or why it waits; how far it is beside it.
                text: !mark.shown ? ""
                      : mark.desk.why !== "" ? qsTr("Writing out “%1”: %2").arg(mark.desk.title).arg(mark.desk.why)
                      : qsTr("Writing out “%1”…").arg(mark.desk.title)
                color: theme.foreground
            }
            // The pieces written out, which a long title never hides: "4 of 12".
            Label {
                objectName: "transcribingCount"
                visible: mark.shown && mark.desk.total > 0
                text: visible ? qsTr("%1 of %2").arg(mark.desk.done.toLocaleString(Qt.locale(), 'f', 0))
                                                 .arg(mark.desk.total.toLocaleString(Qt.locale(), 'f', 0)) : ""
                color: theme.colors.muted
            }
            // How far it is, so it is plain that it goes on.
            Rectangle {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 6
                radius: 3
                color: theme.colors.selection
                Rectangle {
                    width: parent.width * (mark.shown && mark.desk.total > 0 ? mark.desk.done / mark.desk.total : 0)
                    height: parent.height
                    radius: parent.radius
                    color: theme.accent
                }
            }
            AppButton {
                objectName: "stopTranscribing"
                text: qsTr("Stop")
                onClicked: mark.desk.stop()
            }
        }
    }
}
