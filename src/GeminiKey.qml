import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Gemini's API key, on the AI page (#145): one key for every capability it
// serves, kept in the keyring and never shown again; given in whichever row
// Gemini is chosen in.
ColumnLayout {
    id: setup
    property var info: ({})
    readonly property string capability: info.capability || ""
    readonly property var desk: aiAgent.capabilities
    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        TextField {
            objectName: setup.capability + "GeminiKey"
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: setup.info.hasKey ? qsTr("Gemini's key is kept in the keyring; paste another to replace it")
                                               : qsTr("Gemini's API key, from Google AI Studio")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: {
                if (text === "")
                    return
                keyProblem.text = setup.desk.setKey(setup.capability, text)
                text = ""
            }
        }
        AppButton {
            objectName: setup.capability + "ForgetGeminiKey"
            visible: !!setup.info.hasKey
            text: qsTr("Forget the Key")
            onClicked: keyProblem.text = setup.desk.setKey(setup.capability, "")
        }
    }
    Label {
        id: keyProblem
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
    }
}
