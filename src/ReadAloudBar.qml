import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The control of a book being read aloud (#73), over the foot of the page
// while it is: a sentence back, pause or go on, a sentence on, how fast, and
// stop. Each button is the pointer's and the finger's way to its key (`runs`).
Rectangle {
    id: bar
    objectName: "readAloudBar"
    readonly property var aloud: reader.readAloud
    // The width of the page it lies over.
    property real room: 0
    // How fast, between the others where there is room for it; else in the Aa panel only.
    readonly property bool roomy: room >= 600
    // Put away while the bar of the reading keys is up, which has its own Read aloud.
    property bool covered: false
    visible: aloud.active && reader.open && !covered
    width: row.implicitWidth + 16
    height: row.implicitHeight + 12
    radius: theme.radius
    color: theme.background
    border.color: theme.colors.selection

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 6
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        AppButton {
            objectName: "aloudBack"
            runs: "reader.sentence_back"
            // Back is against the reading direction.
            text: win.rightToLeft ? "›" : "‹"
            Accessible.name: qsTr("A sentence back")
            focusPolicy: Qt.NoFocus
            onClicked: bar.aloud.back()
        }
        AppButton {
            objectName: "aloudPause"
            primary: true
            runs: "reader.read_aloud"
            Layout.minimumWidth: 88
            text: bar.aloud.speaking ? qsTr("Pause") : qsTr("Go on")
            focusPolicy: Qt.NoFocus
            onClicked: bar.aloud.toggle()
        }
        AppButton {
            objectName: "aloudNext"
            runs: "reader.sentence_next"
            text: win.rightToLeft ? "‹" : "›"
            Accessible.name: qsTr("A sentence on")
            focusPolicy: Qt.NoFocus
            onClicked: bar.aloud.next()
        }
        AppButton {
            objectName: "aloudSlower"
            visible: bar.roomy
            text: "−"
            Accessible.name: qsTr("Slower")
            enabled: bar.aloud.rate > 0.5
            focusPolicy: Qt.NoFocus
            onClicked: bar.aloud.rate = bar.aloud.rate - 0.1
        }
        Label {
            objectName: "aloudRate"
            visible: bar.roomy
            text: qsTr("%1×").arg(bar.aloud.rate.toLocaleString(Qt.locale(), 'f', 1))
            color: theme.colors.muted
            Accessible.name: qsTr("Speed")
            Accessible.description: text
        }
        AppButton {
            objectName: "aloudFaster"
            visible: bar.roomy
            text: "+"
            Accessible.name: qsTr("Faster")
            enabled: bar.aloud.rate < 2
            focusPolicy: Qt.NoFocus
            onClicked: bar.aloud.rate = bar.aloud.rate + 0.1
        }
        AppButton {
            objectName: "aloudStop"
            runs: "reader.read_aloud_stop"
            text: "✕"
            Accessible.name: qsTr("Stop reading aloud")
            focusPolicy: Qt.NoFocus
            onClicked: bar.aloud.stop()
        }
    }
}
