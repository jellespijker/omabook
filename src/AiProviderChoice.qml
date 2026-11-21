import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The provider of one capability on the AI page: chosen from those that can
// serve it, by their ids (AiCapabilityRows::offered), worded here; named
// where there is only one, for now (Gemini and more voices come: #145,
// #147, each an id more).
GridLayout {
    id: choice
    property var info: ({})
    readonly property var offered: info.offered || []
    readonly property string capability: info.capability || ""

    function nameOf(id) {
        switch (id) {
        case "omarchy-default": return qsTr("Omarchy's default agent")
        case "claude": return qsTr("Claude Code, with your subscription")
        case "agy": return qsTr("Antigravity, with your subscription")
        case "anthropic": return qsTr("Claude, with an API key")
        case "openai": return qsTr("A server (OpenAI-compatible)")
        case "command": return qsTr("A command")
        case "answers": return qsTr("The same as for answers")
        case "ai-server": return qsTr("The AI server, with a model that reads pictures")
        case "whisper.cpp": return qsTr("whisper.cpp, on this computer only")
        case "auto":
            return choice.capability === "speech-to-text" ? qsTr("Automatic: voxtype, whisper.cpp or a server, the first that is there")
                                                          : qsTr("Automatic: Piper with a voice, else speech-dispatcher")
        case "voxtype": return qsTr("voxtype, on this computer only")
        case "piper": return qsTr("Piper")
        case "speech-dispatcher": return qsTr("speech-dispatcher")
        case "server":
            return choice.capability === "speech-to-text" ? qsTr("A transcription server first, then this computer")
                                                          : qsTr("A server (OpenAI-compatible)")
        }
        return id
    }

    columns: win.compact ? 1 : 2
    columnSpacing: 12
    rowSpacing: 4

    Label {
        text: qsTr("Provider")
        Layout.fillWidth: win.compact
        Layout.minimumWidth: win.compact ? 0 : 190
        Layout.preferredWidth: 190
        Layout.maximumWidth: win.compact ? Number.POSITIVE_INFINITY : 190
        wrapMode: Text.Wrap
    }
    ComboBox {
        objectName: choice.capability + "Provider"
        Layout.fillWidth: true
        visible: choice.offered.length > 1
        enabled: aiAgent.capabilities.aiOn || choice.capability === "text-to-speech"
        model: choice.offered.map(id => choice.nameOf(id))
        currentIndex: Math.max(0, choice.offered.indexOf(choice.info.chosen))
        Material.roundedScale: theme.radius
        onActivated: (index) => aiAgent.capabilities.choose(choice.capability, choice.offered[index])
    }
    Label {
        objectName: choice.capability + "OnlyProvider"
        Layout.fillWidth: true
        visible: choice.offered.length === 1
        wrapMode: Text.Wrap
        text: choice.nameOf(choice.offered[0] || "")
    }
}
