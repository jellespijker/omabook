import QtQuick
import QtQuick.Controls

// The one sign that something is being worked on and hasn't answered yet:
// the AI thinking, a catalog being asked, books being read in. Three dots
// that pulse in turn, what is happening in words, and once it has taken a
// few seconds, how long it has been, so a slow answer is told from a hang.
Row {
    id: thinking
    // Whether the work is under way; nothing shows, or moves, when it isn't.
    property bool active: false
    // What is happening: "Thinking…", "Asking the catalogs…".
    property string text: qsTr("Thinking…")
    property color color: theme.accent
    property int seconds: 0
    // The most room there is for it; the words elide past that.
    property real room: 100000

    visible: active
    spacing: 8
    onActiveChanged: seconds = 0

    Timer {
        interval: 1000
        repeat: true
        running: thinking.active
        onTriggered: thinking.seconds += 1
    }
    Row {
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        Repeater {
            model: 3
            delegate: Rectangle {
                id: dot
                required property int index
                width: 6
                height: 6
                radius: 3
                color: thinking.color
                opacity: 0.25
                // Each dot a beat after the one before; stopped with the work,
                // so nothing animates behind a closed dialog.
                SequentialAnimation on opacity {
                    running: thinking.active && thinking.visible
                    loops: Animation.Infinite
                    PauseAnimation { duration: dot.index * 180 }
                    NumberAnimation { to: 1; duration: 320; easing.type: Easing.OutQuad }
                    NumberAnimation { to: 0.25; duration: 320; easing.type: Easing.InQuad }
                    PauseAnimation { duration: (2 - dot.index) * 180 + 200 }
                }
            }
        }
    }
    Label {
        objectName: "thinkingText"
        anchors.verticalCenter: parent.verticalCenter
        color: theme.colors.muted
        font.pixelSize: 13
        width: Math.min(implicitWidth, Math.max(40, thinking.room - 30))
        elide: Text.ElideRight
        // "Thinking… 12 s", so a long wait shows it is still going.
        text: thinking.seconds >= 4 ? qsTr("%1 %2 s").arg(thinking.text).arg(thinking.seconds) : thinking.text
    }
}
