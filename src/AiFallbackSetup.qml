import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What a row uses when its first server can't be reached (#174): this
// computer, another server (its address, key and model), or nothing, and its
// own Test, as though the first were away. Quick work has none: it waits for
// its server and catches up. While the fallback is in use, the row says so.
ColumnLayout {
    id: fallback
    property var info: ({})
    readonly property string capability: info.capability || ""
    readonly property var desk: aiAgent.capabilities
    readonly property var choice: info.fallbackChoice || ({})
    readonly property bool waits: choice.rule === "waits"
    readonly property bool server: choice.to === "server"
    readonly property var ways: ["none", "this-computer", "server"]
    spacing: 8

    function nameOf(way) {
        switch (way) {
        case "none": return qsTr("Nothing: wait for it to answer again")
        case "this-computer":
            switch (fallback.capability) {
            case "speech-to-text": return qsTr("This computer: voxtype or whisper.cpp")
            case "text-to-speech": return qsTr("This computer: Piper or speech-dispatcher")
            }
            return qsTr("This computer: Ollama at localhost")
        }
        return qsTr("Another server (OpenAI-compatible)")
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("When it can't be reached")
        font.bold: true
        wrapMode: Text.Wrap
    }
    // In use now: the first doesn't answer.
    Label {
        objectName: fallback.capability + "FallbackInUse"
        Layout.fillWidth: true
        visible: !!fallback.info.fallback
        wrapMode: Text.Wrap
        color: theme.colors.orange
        text: !visible ? ""
              : fallback.choice.to === "this-computer"
                ? qsTr("On this computer now: %1 can't be reached. Omabook goes back to it by itself once it answers.").arg(fallback.info.first)
                : qsTr("On %1 now: %2 can't be reached. Omabook goes back to it by itself once it answers.").arg(fallback.info.provider).arg(fallback.info.first)
    }
    Label {
        objectName: fallback.capability + "FallbackRule"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        text: fallback.waits ? qsTr("Quick work waits for its server rather than doing poorer work slowly elsewhere: the knowledge graph catches up once the server is back, and nothing is lost.")
              : fallback.capability === "answers" ? qsTr("Answers go elsewhere only if you choose it here.")
              : fallback.capability === "embeddings" ? qsTr("The fallback is asked for the same model, so a book indexed on one server is searched on either.")
              : qsTr("Only when it can't be reached at all, not when it answers with an error.")
    }
    ComboBox {
        objectName: fallback.capability + "FallbackTo"
        Layout.fillWidth: true
        visible: !fallback.waits
        enabled: fallback.desk.aiOn
        model: fallback.ways.map(way => fallback.nameOf(way))
        currentIndex: Math.max(0, fallback.ways.indexOf(fallback.choice.to))
        Material.roundedScale: theme.radius
        onActivated: (index) => fallback.desk.setFallback(fallback.capability, {to: fallback.ways[index]})
    }
    TextField {
        objectName: fallback.capability + "FallbackAddress"
        Layout.fillWidth: true
        visible: fallback.server
        placeholderText: qsTr("Its address, such as http://localhost:11434/v1")
        text: fallback.choice.url || ""
        selectByMouse: true
        enabled: fallback.desk.aiOn
        Material.roundedScale: theme.radius
        onEditingFinished: if (text.trim() !== (fallback.choice.url || "")) fallback.desk.setFallback(fallback.capability, {url: text})
    }
    TextField {
        id: key
        objectName: fallback.capability + "FallbackKey"
        Layout.fillWidth: true
        visible: fallback.server && (fallback.choice.url || "") !== ""
        echoMode: TextInput.Password
        placeholderText: fallback.choice.hasKey ? qsTr("Kept in the keyring; paste another to replace it") : qsTr("Its key, if it takes one")
        selectByMouse: true
        Material.roundedScale: theme.radius
        onAccepted: {
            if (text === "")
                return
            keyProblem.text = fallback.desk.setFallbackKey(fallback.capability, text)
            text = ""
        }
    }
    Label {
        id: keyProblem
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
    }
    Label {
        Layout.fillWidth: true
        visible: fallback.server && !!fallback.choice.inTheClear
        wrapMode: Text.Wrap
        text: qsTr("This address starts with http://, so what is sent crosses the network unencrypted. Use https:// for a server beyond this computer and your tailnet.")
    }
    // The model there, for all but embeddings, which keep to the first's.
    TextField {
        objectName: fallback.capability + "FallbackModel"
        Layout.fillWidth: true
        visible: !fallback.waits && fallback.choice.to !== "none" && fallback.capability !== "embeddings"
                 && !(fallback.choice.to === "this-computer" && (fallback.capability === "speech-to-text" || fallback.capability === "text-to-speech"))
        placeholderText: qsTr("Its model; empty for the same as the first's")
        text: fallback.choice.model || ""
        selectByMouse: true
        enabled: fallback.desk.aiOn
        Material.roundedScale: theme.radius
        onEditingFinished: if (text.trim() !== (fallback.choice.model || "")) fallback.desk.setFallback(fallback.capability, {model: text})
    }
    // Someone else's computer needs its own allowance, whatever the first is.
    CheckBox {
        objectName: fallback.capability + "FallbackHosted"
        Layout.fillWidth: true
        visible: fallback.server
        text: qsTr("It may be a hosted service")
        checked: !!fallback.choice.hosted
        onToggled: fallback.desk.setFallback(fallback.capability, {hosted: checked})
    }
    AiCapabilityTest {
        Layout.fillWidth: true
        visible: !fallback.waits && fallback.choice.to !== "none"
        info: fallback.info
        fallback: true
    }
}
