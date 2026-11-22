import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A server one capability is sent to, on the AI page: its address (none for
// the AI server), its key, kept in the keyring and never shown again, and the
// model. Handwriting has the AI server's vision model, or Gemini's; speech
// and voices have a server only where one is chosen for them.
ColumnLayout {
    id: setup
    property var info: ({})
    readonly property string capability: info.capability || ""
    readonly property var desk: aiAgent.capabilities
    // Gemini, and OpenAI's voices, have their key in a row of their own (ApiKey) and no address.
    readonly property bool keyed: info.chosen === "gemini" || (capability === "text-to-speech" && info.chosen === "openai")
    readonly property bool server: (capability === "embeddings" && !keyed) || (info.chosen === "server" && capability !== "handwriting")
    spacing: 8
    visible: server || keyed || capability === "handwriting"

    TextField {
        id: address
        objectName: setup.capability + "Address"
        Layout.fillWidth: true
        visible: setup.server
        placeholderText: setup.info.addressDefault ? qsTr("Its address; empty for the AI server (%1)").arg(setup.info.addressDefault)
                                                   : qsTr("Its address, such as http://localhost:11434/v1")
        text: setup.info.address || ""
        selectByMouse: true
        enabled: setup.desk.aiOn
        Material.roundedScale: theme.radius
        onEditingFinished: if (text.trim() !== (setup.info.address || "")) setup.desk.setAddress(setup.capability, text)
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: setup.server && (setup.info.address || "") !== ""
        TextField {
            id: key
            objectName: setup.capability + "Key"
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: setup.info.hasKey ? qsTr("Kept in the keyring; paste another to replace it")
                                               : qsTr("Its key, if it takes one")
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
            objectName: setup.capability + "ForgetKey"
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
    Label {
        objectName: setup.capability + "Unencrypted"
        Layout.fillWidth: true
        visible: setup.server && !!setup.info.inTheClear
        wrapMode: Text.Wrap
        text: qsTr("This address starts with http://, so what is sent crosses the network unencrypted. Use https:// for a server beyond this computer and your tailnet.")
    }
    GridLayout {
        Layout.fillWidth: true
        columns: win.compact ? 1 : 2
        columnSpacing: 12
        rowSpacing: 4
        Label {
            text: qsTr("Model")
            Layout.fillWidth: win.compact
            Layout.minimumWidth: win.compact ? 0 : 190
            Layout.preferredWidth: 190
            Layout.maximumWidth: win.compact ? Number.POSITIVE_INFINITY : 190
            wrapMode: Text.Wrap
        }
        TextField {
            id: model
            objectName: setup.capability + "Model"
            Layout.fillWidth: true
            placeholderText: setup.info.modelDefault ? qsTr("The default (%1)").arg(setup.info.modelDefault) : qsTr("The default")
            text: setup.info.modelChoice || ""
            selectByMouse: true
            enabled: setup.desk.aiOn
            Material.roundedScale: theme.radius
            onEditingFinished: if (text.trim() !== (setup.info.modelChoice || "")) setup.desk.setModel(setup.capability, text)
        }
    }
}
