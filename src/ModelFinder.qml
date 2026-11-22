import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// "Find a model…" for one capability (#175): a search of Ollama's library and
// Hugging Face, opened by the person, each model found with its size, the
// memory it needs and whether that fits where it would run, its licence and
// whether it runs on someone else's computer; installed with one press and
// then chosen for the capability. The AI page's rows show it, and so does the
// place a missing model is reported (ModelOffer).
ColumnLayout {
    id: finder

    property string capability: ""
    // What the objectNames start with.
    property string name: capability + "Find"
    property bool open: false
    readonly property QtObject windowSettings: settings
    readonly property string stage: search.state
    readonly property bool speech: capability === "speech-to-text" || capability === "text-to-speech"
    // Installed and chosen: the row then tests it once (#170).
    signal chosen(string model)

    spacing: 8

    ModelSearch {
        id: search
        settings: finder.windowSettings
        capability: finder.capability
        onChosen: (model) => finder.chosen(model)
    }

    function fitText(found) {
        switch (found.fit) {
        case "gpu": return qsTr("fits the graphics card's free memory")
        case "memory": return qsTr("fits the free memory, on the processor: slower")
        case "too-big": return qsTr("too big for the memory free here")
        }
        if (found.hosted) return qsTr("runs on Ollama's computers")
        return search.where !== "" ? qsTr("whether it fits %1 can't be told from here").arg(search.where) : ""
    }
    function details(found) {
        const parts = [found.source === "ollama" ? qsTr("Ollama") : qsTr("Hugging Face")]
        if (found.size) parts.push(qsTr("%1 to download").arg(found.size))
        if (found.needed) parts.push(qsTr("needs %1").arg(found.needed))
        parts.push(fitText(found))
        parts.push(found.licence ? qsTr("licence: %1").arg(found.licence) : qsTr("licence not said"))
        if (found.hosted) parts.push(qsTr("someone else's computer"))
        return parts.filter(part => !!part).join(" · ")
    }
    readonly property string said: {
        switch (stage) {
        case "off": return qsTr("Off, as the AI is: nothing is searched.")
        case "searching": return qsTr("Searching…")
        case "found":
            return search.results.length === 0 ? qsTr("Nothing found that can do this. Try other words.") : ""
        case "installing":
            return speech ? qsTr("Downloading %1… %2%").arg(search.model).arg(Math.round(search.progress * 100))
                          : qsTr("Installing onto %1… %2%").arg(search.where).arg(Math.round(search.progress * 100))
        case "chosen": return qsTr("%1 is installed and chosen for this.").arg(search.model)
        case "failed": return search.problem
        }
        return ""
    }

    AppButton {
        objectName: finder.name + "Open"
        visible: !finder.open
        enabled: aiAgent.capabilities.aiOn
        text: qsTr("Find a Model…")
        Accessible.description: qsTr("Searches Ollama's library and Hugging Face for a model that can do this.")
        onClicked: finder.open = true
    }
    Label {
        Layout.fillWidth: true
        visible: finder.open
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: theme.colors.muted
        text: finder.capability === "speech-to-text" ? qsTr("Searches Hugging Face for whisper.cpp's models. Only the words you search for go there, never a book, a note or a recording.")
              : finder.capability === "text-to-speech" ? qsTr("Searches Hugging Face for Piper's voices. Only the words you search for go there, never a book or a note.")
              : qsTr("Searches Ollama's library and Hugging Face. Only the words you search for and the kind of model go there, never a book or a note.")
    }
    RowLayout {
        Layout.fillWidth: true
        visible: finder.open
        spacing: 8
        TextField {
            id: terms
            objectName: finder.name + "Terms"
            Layout.fillWidth: true
            placeholderText: finder.speech ? qsTr("A language or a size, such as en or base") : qsTr("A name or a family, such as qwen")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: search.search(text)
        }
        AppButton {
            objectName: finder.name + "Search"
            primary: true
            text: qsTr("Search")
            enabled: finder.stage !== "searching" && finder.stage !== "installing"
            onClicked: search.search(terms.text)
        }
        AppButton {
            objectName: finder.name + "Close"
            text: finder.stage === "searching" || finder.stage === "installing" ? qsTr("Cancel") : qsTr("Close")
            onClicked: {
                if (finder.stage === "searching" || finder.stage === "installing")
                    search.cancel()
                else
                    finder.open = false
            }
        }
    }
    Label {
        objectName: finder.name + "Says"
        Layout.fillWidth: true
        visible: finder.open && text !== ""
        wrapMode: Text.Wrap
        color: finder.stage === "failed" ? theme.colors.orange : theme.foreground
        text: finder.said
    }
    ProgressBar {
        Layout.fillWidth: true
        visible: finder.open && finder.stage === "installing"
        value: search.progress
    }
    Repeater {
        model: finder.open ? search.results : []
        delegate: RowLayout {
            id: result
            required property var modelData
            required property int index
            Layout.fillWidth: true
            spacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: result.modelData.name
                    font.bold: true
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    text: finder.details(result.modelData)
                    color: result.modelData.fit === "too-big" || result.modelData.hosted ? theme.colors.orange : theme.colors.muted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
            AppButton {
                objectName: finder.name + "Install" + result.index
                visible: search.installable
                enabled: finder.stage === "found" || finder.stage === "failed" || finder.stage === "chosen"
                text: qsTr("Install")
                Accessible.description: result.modelData.size ? qsTr("Downloads %1 and chooses it for this.").arg(result.modelData.size)
                                                              : qsTr("Downloads it and chooses it for this.")
                onClicked: search.install(result.index)
            }
        }
    }
}
