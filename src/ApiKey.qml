import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A hosted provider's API key, on the AI page: Gemini's (#145), one key for
// every capability it serves, or OpenAI's for its voices (#147), the one it
// answers with too. Kept in the keyring and never shown again; given in
// whichever row the provider is chosen in.
ColumnLayout {
    id: setup
    property var info: ({})
    // "gemini" or "openai".
    property string provider: "gemini"
    readonly property string capability: info.capability || ""
    readonly property string tag: provider === "openai" ? "OpenAiKey" : "GeminiKey"
    readonly property var desk: aiAgent.capabilities
    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        TextField {
            objectName: setup.capability + setup.tag
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: setup.provider === "openai"
                             ? (setup.info.hasKey ? qsTr("OpenAI's key is kept in the keyring; paste another to replace it")
                                                  : qsTr("OpenAI's API key, from its platform's API keys page"))
                             : (setup.info.hasKey ? qsTr("Gemini's key is kept in the keyring; paste another to replace it")
                                                  : qsTr("Gemini's API key, from Google AI Studio"))
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
            objectName: setup.capability + "Forget" + setup.tag
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
