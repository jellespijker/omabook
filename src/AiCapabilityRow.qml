import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One row of the AI page: a capability, how it stands (its provider and
// model, or why it can't be used), and, open, what can be chosen for it. The
// registry says why by a code (ADR 0005); the words are here, in the
// person's language, with what to do about it.
ColumnLayout {
    id: row

    // The row as AiCapabilityRows has it (aicapabilityrows.h).
    property var info: ({})
    property bool open: false
    readonly property alias head: head
    readonly property string capability: info.capability || ""
    readonly property var desk: aiAgent.capabilities
    signal toggled()
    signal step(int by)

    spacing: 8

    readonly property var titles: ({
        "answers": qsTr("Answers"),
        "quick": qsTr("Quick work"),
        "handwriting": qsTr("Handwriting"),
        "speech-to-text": qsTr("Speech to text"),
        "text-to-speech": qsTr("Text to speech"),
        "embeddings": qsTr("Meaning")
    })
    readonly property var purposes: ({
        "answers": qsTr("What you read from the AI: the companion, catch-ups, questions, and what plugins write."),
        "quick": qsTr("What Omabook reads: books into the knowledge graph, look-ups and series. A small, fast model makes it cheaper, and often faster."),
        "handwriting": qsTr("Reads your handwritten notes into words, with a model that reads pictures."),
        "speech-to-text": qsTr("Turns what you record into words."),
        "text-to-speech": qsTr("Reads books aloud."),
        "embeddings": qsTr("Indexes books by what they mean, so a passage is found by its sense, not only its words.")
    })
    // What goes to the provider, for the warning when it may be hosted.
    readonly property var sent: ({
        "answers": qsTr("Careful: your questions, with the part of the book they are about, then go to someone else's computer, and they can read them."),
        "quick": qsTr("Careful: the books Omabook reads for the knowledge graph then go to someone else's computer, and they can read them."),
        "handwriting": qsTr("Careful: what you write by hand then goes to someone else's computer wherever you choose it, such as for one notebook, and they can read it."),
        "speech-to-text": qsTr("Careful: what you record then goes to someone else's computer wherever you choose it, and they can listen to it."),
        "text-to-speech": qsTr("Careful: the text read aloud then goes to someone else's computer, and they can read it."),
        "embeddings": qsTr("Careful: a book's text then goes to someone else's computer to be indexed, and you are told so first.")
    })

    // Why it can't be used, in a few words, for the row itself.
    function whyText(info) {
        switch (info.why) {
        case "ai-off": return qsTr("Off, as the AI is")
        case "no-server": return qsTr("Nothing is set up for it")
        case "hosted-not-allowed": return qsTr("Only a hosted service is set up, and hosted isn't allowed for it")
        case "hosted-not-chosen": return qsTr("Hosted, and not chosen for this")
        case "no-model": return qsTr("The server has no model for it")
        case "not-installed": return qsTr("Not installed")
        case "no-key": return qsTr("A key is needed")
        case "unreachable": return qsTr("It doesn't answer")
        }
        return ""
    }
    // What to do about it.
    function toDo(info) {
        switch (info.why) {
        case "ai-off": return qsTr("Turn on Use AI at the top of this page.")
        case "hosted-not-allowed": return qsTr("Allow a hosted service for it, or choose one of your own.")
        case "hosted-not-chosen": return qsTr("Only where you choose it, such as for one notebook.")
        case "no-model": return qsTr("Choose a model the server has, or add one to it (such as with ollama pull).")
        case "no-key": return qsTr("Give its key below; it is kept in the keyring, never in a file.")
        case "unreachable": return qsTr("Start it, or check its address below.")
        case "not-installed":
            return info.capability === "text-to-speech" ? qsTr("Install Piper with a voice, or speech-dispatcher, or choose a server below.")
                                                        : qsTr("Install it, or choose another below.")
        case "no-server":
            switch (info.capability) {
            case "handwriting": return qsTr("Choose a server for answers that reads pictures, such as Ollama with a vision model.")
            case "speech-to-text": return qsTr("Install whisper.cpp with a model, or give a transcription server's address below.")
            case "text-to-speech": return qsTr("Give the server's address below, or choose a voice on this computer.")
            case "embeddings": return qsTr("Run Ollama with nomic-embed-text (ollama pull nomic-embed-text), or give a server's address below.")
            }
            return qsTr("Start Omarchy's local models or Ollama, or choose who answers below.")
        }
        return ""
    }
    // How it stands, in one line.
    readonly property string summary: {
        if (!info.available)
            return whyText(info)
        const parts = [info.provider]
        if (info.model) parts.push(info.model)
        // voxtype gives only the words (#146).
        if (info.timed === false) parts.push(qsTr("no word timestamps"))
        parts.push(info.hosted ? qsTr("someone else's computer") : qsTr("your own computers"))
        return parts.filter(part => !!part).join(" · ")
    }

    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: theme.colors.selection }

    ItemDelegate {
        id: head
        objectName: row.capability + "Row"
        Layout.fillWidth: true
        implicitHeight: Math.max(52, heading.implicitHeight + 16)
        padding: 0
        Keys.onReturnPressed: row.toggled()
        Keys.onEnterPressed: row.toggled()
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Down || event.key === Qt.Key_J) { row.step(1); event.accepted = true }
            else if (event.key === Qt.Key_Up || event.key === Qt.Key_K) { row.step(-1); event.accepted = true }
        }
        onClicked: row.toggled()
        Accessible.name: (row.titles[row.capability] || "") + ": " + row.summary
        background: Rectangle {
            radius: theme.radius
            color: head.visualFocus || head.hovered ? theme.colors.selection : "transparent"
            border.width: head.visualFocus ? 1 : 0
            border.color: theme.foreground
        }
        contentItem: RowLayout {
            spacing: 10
            // Ready, ready on someone else's computer, or not.
            Rectangle {
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                implicitWidth: 10
                implicitHeight: 10
                radius: 5
                color: !row.info.available ? theme.colors.muted : row.info.hosted ? theme.colors.orange : theme.accent
            }
            ColumnLayout {
                id: heading
                Layout.fillWidth: true
                Layout.rightMargin: 8
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: row.titles[row.capability] || row.capability
                    font.bold: true
                    wrapMode: Text.Wrap
                }
                Label {
                    objectName: row.capability + "Summary"
                    Layout.fillWidth: true
                    text: row.summary
                    color: row.info.available ? theme.colors.muted : theme.colors.orange
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    // What the row opens to.
    ColumnLayout {
        visible: row.open
        Layout.fillWidth: true
        Layout.leftMargin: 8
        Layout.rightMargin: 8
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: row.purposes[row.capability] || ""
        }
        // Why not, and what to do: the registry's own words name the provider.
        Label {
            objectName: row.capability + "Why"
            Layout.fillWidth: true
            visible: !row.info.available && text !== ""
            wrapMode: Text.Wrap
            text: [row.info.message, row.toDo(row.info)].filter(part => !!part).join(" ")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: allow.visible || install.visible
            AppButton {
                id: allow
                objectName: row.capability + "Allow"
                visible: row.info.why === "hosted-not-allowed"
                text: qsTr("Allow Hosted")
                onClicked: row.desk.allowHosted(row.capability, true)
            }
            // Omarchy can install a voice (#156).
            AppButton {
                id: install
                objectName: row.capability + "Install"
                visible: row.capability === "text-to-speech" && !row.info.available && reader.readAloud.installable
                text: qsTr("Install a voice…")
                onClicked: reader.readAloud.offerInstall()
            }
            Item { Layout.fillWidth: true }
        }

        // What writes out recordings, and voxtype to install on Omarchy (#146).
        // Each part of one capability only is made in its row alone, so that
        // its names are found once.
        Loader {
            Layout.fillWidth: true
            active: row.capability === "speech-to-text"
            visible: active
            sourceComponent: ColumnLayout {
                spacing: 8
                Label {
                    objectName: "speechHeardBy"
                    Layout.fillWidth: true
                    visible: text !== ""
                    wrapMode: Text.Wrap
                    color: theme.colors.muted
                    text: backend.speech.installing ? qsTr("Installing voxtype in the terminal that opened. Once it is closed, Omabook looks for it again.")
                          : backend.speech.heardBy === "" ? ""
                          : backend.speech.timed ? qsTr("Recordings are written out by %1, each word with its moment.").arg(backend.speech.heardBy)
                          : qsTr("Recordings are written out by %1, which doesn't give each word its moment; whisper.cpp or a transcription server does.").arg(backend.speech.heardBy)
                }
                AppButton {
                    objectName: "speechInstall"
                    visible: backend.speech.installable && !backend.speech.installing
                    text: qsTr("Install voxtype…")
                    onClicked: backend.speech.install()
                }
            }
        }

        // The provider: a choice where there is more than one.
        AiProviderChoice {
            Layout.fillWidth: true
            info: row.info
        }

        // Its set-up: the AI's own for answers and quick work, else a server's.
        Loader {
            Layout.fillWidth: true
            active: row.capability === "answers"
            visible: active
            sourceComponent: AiAnswersSetup {}
        }
        Loader {
            Layout.fillWidth: true
            active: row.capability === "quick"
            visible: active && aiAgent.available
            sourceComponent: ModelPicker {
                tier: "quick"
                label: qsTr("Model")
            }
        }
        AiServerSetup {
            Layout.fillWidth: true
            visible: row.capability !== "answers" && row.capability !== "quick"
            info: row.info
        }
        Loader {
            Layout.fillWidth: true
            active: row.capability === "embeddings"
            visible: active
            sourceComponent: CheckBox {
                objectName: "meaningAll"
                text: qsTr("Index every book by its meaning, not only those I ask for")
                checked: settings.meaningAll
                onToggled: settings.meaningAll = checked
            }
        }

        // Whether it may go to someone else's computer: for this one alone.
        CheckBox {
            objectName: row.capability + "Hosted"
            Layout.fillWidth: true
            text: qsTr("May use a hosted service")
            checked: !!row.info.hostedAllowed
            onToggled: row.desk.allowHosted(row.capability, checked)
        }
        Label {
            objectName: row.capability + "HostedNote"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: row.info.hostedAllowed ? theme.colors.orange : theme.colors.muted
            text: row.info.hostedAllowed ? (row.sent[row.capability] || "")
                                         : qsTr("A hosted service is someone else's computer. Without it, only this computer and your tailnet, or an address you marked as your own.")
        }
    }
}
