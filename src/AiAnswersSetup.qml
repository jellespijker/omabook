import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What answers are set up with, on the AI page: the command, or the server
// (which one, where it is, its key, kept in the keyring and never shown
// again), the model, and which agent answers and why.
ColumnLayout {
    id: setup
    spacing: 8
    readonly property bool server: settings.aiProvider === "openai"

    // The servers "A server" can be: Omarchy's local models only where
    // Omarchy can run them.
    readonly property var servers: (aiAgent.omarchyLocal ? [{ key: "omarchy-local", name: qsTr("Omarchy's local model") }] : []).concat([
        { key: "ollama", name: qsTr("Ollama") },
        { key: "openai", name: qsTr("OpenAI") },
        { key: "custom", name: qsTr("Another server: OpenRouter, Groq, LM Studio, llama.cpp, ...") }
    ])

    TextField {
        objectName: "aiCommand"
        Layout.fillWidth: true
        visible: settings.aiProvider === "command"
        placeholderText: qsTr("Command that reads a prompt and writes the answer")
        text: settings.aiCommand
        selectByMouse: true
        Material.roundedScale: theme.radius
        onEditingFinished: settings.aiCommand = text
    }
    ComboBox {
        objectName: "aiServer"
        Layout.fillWidth: true
        visible: setup.server
        model: setup.servers
        textRole: "name"
        currentIndex: Math.max(0, setup.servers.findIndex(entry => entry.key === aiAgent.serverPreset))
        Material.roundedScale: theme.radius
        onActivated: (index) => aiAgent.setServer(setup.servers[index].key, "")
    }
    TextField {
        objectName: "aiServerUrl"
        Layout.fillWidth: true
        visible: setup.server && aiAgent.serverPreset !== "omarchy-local"
        placeholderText: qsTr("The server's address, such as https://openrouter.ai/api/v1")
        text: aiAgent.serverUrl
        selectByMouse: true
        Material.roundedScale: theme.radius
        onEditingFinished: aiAgent.setServer(aiAgent.serverPreset, text)
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: setup.server && aiAgent.serverPreset !== "omarchy-local"
        TextField {
            objectName: "aiServerKey"
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: aiAgent.serverHasKey ? qsTr("Kept in the keyring; paste another to replace it")
                                                  : qsTr("Its key, if it takes one")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: {
                if (text === "")
                    return
                serverKeyProblem.text = aiAgent.setServerKey(text)
                text = ""
            }
        }
        AppButton {
            objectName: "forgetServerKey"
            visible: aiAgent.serverHasKey
            text: qsTr("Forget the Key")
            onClicked: serverKeyProblem.text = aiAgent.setServerKey("")
        }
    }
    Label {
        id: serverKeyProblem
        Layout.fillWidth: true
        visible: setup.server && text !== ""
        wrapMode: Text.Wrap
    }
    Label {
        objectName: "aiServerUnencrypted"
        Layout.fillWidth: true
        visible: setup.server && aiAgent.serverInTheClear
        wrapMode: Text.Wrap
        text: qsTr("This address starts with http://, so your questions and the book's text cross the network unencrypted. Use https:// for a server beyond this computer and your tailnet.")
    }
    Label {
        objectName: "aiServerModels"
        Layout.fillWidth: true
        visible: setup.server && aiAgent.modelsProblem !== ""
        wrapMode: Text.Wrap
        color: theme.colors.muted
        text: aiAgent.modelsProblem
    }
    // The models are those of the AI in use; quick work has its own row.
    ModelPicker {
        Layout.fillWidth: true
        visible: aiAgent.available
        tier: "main"
        label: qsTr("Model")
    }
    // Which agent answers and why: Omarchy's default agent is followed.
    Label {
        objectName: "aiAgent"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: text !== ""
        text: aiAgent.source === "omarchy" ? qsTr("Omarchy's default: %1").arg(aiAgent.name)
            : aiAgent.source === "fallback" ? (aiAgent.omarchyDefaultRuns
                ? qsTr("Omarchy's default, %1, isn't installed, so %2 answers.").arg(aiAgent.omarchyDefaultName).arg(aiAgent.name)
                : qsTr("Omabook can't run Omarchy's default, %1, with its tools off, so %2 answers.").arg(aiAgent.omarchyDefaultName).arg(aiAgent.name))
            : aiAgent.source === "setting" ? qsTr("Chosen here: %1").arg(aiAgent.name)
            : aiAgent.source === "automatic" ? qsTr("Found on this computer: %1").arg(aiAgent.name)
            : ""
    }
    Label {
        objectName: "aiState"
        Layout.fillWidth: true
        visible: assistant.available
        wrapMode: Text.Wrap
        color: theme.colors.muted
        text: qsTr("Questions go to %1, with the part of the book you ask about.").arg(assistant.providerName)
    }
}
