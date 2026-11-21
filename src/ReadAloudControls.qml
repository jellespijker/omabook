import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Reading aloud in the Aa panel (#73): what reads (or why nothing can, and
// what to install), how fast, and in which voice, for every book; and the
// button that starts it, as its key does.
ColumnLayout {
    id: controls
    objectName: "readAloudControls"
    readonly property var aloud: reader.readAloud
    // The words at the top of the page, where it starts from.
    property var placeText: function() { return "" }
    spacing: 6
    Component.onCompleted: aloud.refresh()

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 8
        text: qsTr("Reading aloud")
        font.bold: true
        elide: Text.ElideRight
    }
    Label {
        objectName: "readAloudEngine"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
        text: controls.aloud.engine !== "" ? qsTr("Read by %1. Nothing is sent anywhere unless you chose a server for it.").arg(controls.aloud.engine)
                                           : controls.aloud.why
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Label {
            Layout.fillWidth: true
            text: qsTr("Speed")
            elide: Text.ElideRight
        }
        AppButton {
            objectName: "readAloudSlower"
            text: "−"
            Accessible.name: qsTr("Slower")
            enabled: controls.aloud.rate > 0.5
            onClicked: controls.aloud.rate = controls.aloud.rate - 0.1
        }
        Label {
            objectName: "readAloudRate"
            Layout.minimumWidth: 40
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("%1×").arg(controls.aloud.rate.toLocaleString(Qt.locale(), 'f', 1))
        }
        AppButton {
            objectName: "readAloudFaster"
            text: "+"
            Accessible.name: qsTr("Faster")
            enabled: controls.aloud.rate < 2
            onClicked: controls.aloud.rate = controls.aloud.rate + 0.1
        }
    }
    ComboBox {
        id: voices
        objectName: "readAloudVoice"
        Layout.fillWidth: true
        visible: controls.aloud.voices.length > 0
        Accessible.name: qsTr("Voice")
        // The first is whichever voice suits the book's language.
        model: [qsTr("Voice: automatic")].concat(controls.aloud.voices)
        currentIndex: Math.max(0, controls.aloud.voices.indexOf(controls.aloud.voice) + 1)
        onActivated: (index) => controls.aloud.voice = index > 0 ? controls.aloud.voices[index - 1] : ""
    }
    AppButton {
        objectName: "readAloudStart"
        Layout.fillWidth: true
        runs: "reader.read_aloud"
        enabled: controls.aloud.engine !== ""
        text: controls.aloud.speaking ? qsTr("Pause") : controls.aloud.active ? qsTr("Go on") : qsTr("Read aloud")
        onClicked: controls.aloud.toggle(controls.placeText())
    }
}
