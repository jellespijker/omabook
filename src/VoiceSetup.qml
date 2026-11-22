import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Text to speech's voice on the AI page (#147): which of the engine's voices,
// how fast (where it takes a pace), and a sentence read with them to hear it
// before a book is. Each engine keeps its own voice and pace, so choosing
// another provider brings back what was chosen for it.
ColumnLayout {
    id: setup
    property var info: ({})
    readonly property var desk: aiAgent.capabilities
    readonly property var preview: desk.preview
    readonly property var voices: info.voices || []
    spacing: 8

    GridLayout {
        Layout.fillWidth: true
        columns: win.compact ? 1 : 2
        columnSpacing: 12
        rowSpacing: 4

        Label {
            visible: setup.voices.length > 0
            text: qsTr("Voice")
            Layout.fillWidth: win.compact
            Layout.minimumWidth: win.compact ? 0 : 190
            Layout.preferredWidth: 190
            Layout.maximumWidth: win.compact ? Number.POSITIVE_INFINITY : 190
            wrapMode: Text.Wrap
        }
        ComboBox {
            objectName: "voiceChoice"
            Layout.fillWidth: true
            visible: setup.voices.length > 0
            Accessible.name: qsTr("Voice")
            // The first is whichever voice suits the book's language.
            model: [qsTr("Automatic")].concat(setup.voices)
            currentIndex: Math.max(0, setup.voices.indexOf(setup.info.voice || "") + 1)
            Material.roundedScale: theme.radius
            onActivated: (index) => setup.desk.setVoice(index > 0 ? setup.voices[index - 1] : "")
        }

        Label {
            text: qsTr("Speed")
            Layout.fillWidth: win.compact
            Layout.minimumWidth: win.compact ? 0 : 190
            Layout.preferredWidth: 190
            Layout.maximumWidth: win.compact ? Number.POSITIVE_INFINITY : 190
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            visible: setup.info.paced !== false
            AppButton {
                objectName: "voiceSlower"
                text: "−"
                Accessible.name: qsTr("Slower")
                enabled: (setup.info.rate || 1) > 0.5
                onClicked: setup.desk.setRate((setup.info.rate || 1) - 0.1)
            }
            Label {
                objectName: "voiceRate"
                Layout.minimumWidth: 40
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("%1×").arg((setup.info.rate || 1).toLocaleString(Qt.locale(), 'f', 1))
                Accessible.name: qsTr("Speed")
                Accessible.description: text
            }
            AppButton {
                objectName: "voiceFaster"
                text: "+"
                Accessible.name: qsTr("Faster")
                enabled: (setup.info.rate || 1) < 2
                onClicked: setup.desk.setRate((setup.info.rate || 1) + 0.1)
            }
            Item { Layout.fillWidth: true }
        }
        Label {
            objectName: "voiceUnpaced"
            Layout.fillWidth: true
            visible: setup.info.paced === false
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("Its voices read at their own pace.")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        AppButton {
            objectName: "voicePreview"
            text: setup.preview.playing ? qsTr("Stop") : qsTr("Hear It")
            Accessible.description: qsTr("Reads a sentence with this voice")
            onClicked: setup.preview.playing ? setup.preview.stop()
                                             : setup.preview.play(qsTr("This is how your books will be read aloud."), Qt.uiLanguage || Qt.locale().name)
        }
        Label {
            objectName: "voicePreviewWhy"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: theme.colors.orange
            text: setup.preview.why
        }
    }
}
