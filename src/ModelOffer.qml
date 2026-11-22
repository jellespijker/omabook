import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A capability's model missing on its server, and what to do about it (#172):
// one already on the server that fits, offered; else a small, well-known one
// to download onto Ollama, its size said first; the download's progress; and
// the model chosen once it is there. Find's pane and the AI page's rows show
// it, each with a ModelHelp of its own.
ColumnLayout {
    id: offer

    // The ModelHelp (modelhelp.h) whose state is shown.
    property QtObject help: null
    // What the objectNames start with: "findModel", "embeddingsModel", ...
    property string name: "model"
    readonly property string stage: help ? help.state : ""
    // A model found by a search was installed and chosen (#175).
    signal found()

    visible: stage !== "" && stage !== "none" && stage !== "there"
    spacing: 8

    function said() {
        if (!help) return ""
        const wanted = help.wanted
        switch (stage) {
        case "looking": return qsTr("Looking at what %1 has…").arg(help.server)
        case "offer":
            if (help.unfit) return qsTr("%1 on %2 can't do this; %3, which is on it too, can.").arg(wanted).arg(help.server).arg(help.model)
            return wanted === "" ? qsTr("No model is chosen for this on %1; %2, which is on it, can do this.").arg(help.server).arg(help.model)
                                 : qsTr("%1 isn't on %2; %3, which is on it, can do this.").arg(wanted).arg(help.server).arg(help.model)
        case "suggest":
            if (help.unfit) return qsTr("%1 on %2 can't do this, nor can anything else on it. Omabook can download %3 onto it (%4), a small model that can.").arg(wanted).arg(help.server).arg(help.model).arg(help.size)
            return wanted === help.model ? qsTr("%1 isn't on %2 yet. Omabook can download it onto the server (%3).").arg(wanted).arg(help.server).arg(help.size)
                                         : qsTr("Nothing on %1 can do this. Omabook can download %2 onto it (%3), a small model that can.").arg(help.server).arg(help.model).arg(help.size)
        case "missing":
            return qsTr("Nothing on %1 can do this. Add a model that can to the server, or choose another way on the AI page.").arg(help.server)
        case "downloading": return qsTr("Downloading %1 onto %2… %3%").arg(help.model).arg(help.server).arg(Math.round(help.progress * 100))
        case "chosen": return qsTr("%1 on %2 is chosen for this now.").arg(help.model).arg(help.server)
        case "failed": return help.problem
        }
        return ""
    }

    Label {
        objectName: offer.name + "Says"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: offer.stage === "failed" || offer.stage === "missing" ? theme.colors.orange : theme.colors.muted
        text: offer.said()
    }
    ProgressBar {
        Layout.fillWidth: true
        visible: offer.stage === "downloading"
        value: offer.help ? offer.help.progress : 0
    }
    Flow {
        Layout.fillWidth: true
        spacing: 8
        // By the stage, not the buttons' own visible, which follows this one's.
        visible: ["offer", "suggest", "downloading", "looking", "failed"].indexOf(offer.stage) >= 0
        AppButton {
            objectName: offer.name + "Use"
            visible: offer.stage === "offer"
            primary: true
            text: qsTr("Use %1").arg(offer.help ? offer.help.model : "")
            onClicked: offer.help.use("")
        }
        // The others on the server that can do it too.
        Repeater {
            model: offer.stage === "offer" && offer.help ? offer.help.fitting.slice(1, 4) : []
            AppButton {
                required property string modelData
                text: qsTr("Use %1").arg(modelData)
                onClicked: offer.help.use(modelData)
            }
        }
        AppButton {
            objectName: offer.name + "Download"
            visible: offer.stage === "suggest"
            primary: true
            text: qsTr("Download %1").arg(offer.help ? offer.help.model : "")
            onClicked: offer.help.download()
        }
        AppButton {
            objectName: offer.name + "Cancel"
            visible: offer.stage === "downloading" || offer.stage === "looking"
            text: qsTr("Cancel")
            onClicked: offer.help.cancel()
        }
        AppButton {
            objectName: offer.name + "Again"
            visible: offer.stage === "failed"
            text: qsTr("Try Again")
            onClicked: offer.help.size !== "" ? offer.help.download() : offer.help.look()
        }
    }
    // Beyond the server and the kept suggestion: a search of Ollama's library and Hugging Face (#175).
    ModelFinder {
        Layout.fillWidth: true
        visible: ["suggest", "missing", "offer"].indexOf(offer.stage) >= 0
        capability: offer.help ? offer.help.capability : ""
        name: offer.name + "Find"
        onChosen: offer.found()
    }
}
