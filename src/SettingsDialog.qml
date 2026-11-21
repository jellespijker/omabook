import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The few choices there are: which library, how the page is set, which AI.
// Each takes effect as it is made.
AppPopup {
    id: dialog
    objectName: "settings"
    widest: 560
    onAboutToShow: backend.fileManager.checkOpensEbooks()

    readonly property var providers: [
        { key: "auto", name: qsTr("Automatic") },
        { key: "claude", name: qsTr("Claude Code, with your subscription") },
        { key: "agy", name: qsTr("Antigravity, with your subscription") },
        { key: "anthropic", name: qsTr("Claude, with an API key") },
        { key: "openai", name: qsTr("A server (OpenAI-compatible)") },
        { key: "command", name: qsTr("A command") },
        { key: "off", name: qsTr("Off") }
    ]

    // The servers "A server" can be: Omarchy's local models only where
    // Omarchy can run them.
    readonly property var servers: (aiAgent.omarchyLocal ? [{ key: "omarchy-local", name: qsTr("Omarchy's local model") }] : []).concat([
        { key: "ollama", name: qsTr("Ollama") },
        { key: "openai", name: qsTr("OpenAI") },
        { key: "custom", name: qsTr("Another server: OpenRouter, Groq, LM Studio, llama.cpp, ...") }
    ])

    // In a short window the settings scroll rather than run off its edge.
    contentItem: ScrollView {
        id: scroller
        clip: true
        contentWidth: availableWidth
    ColumnLayout {
        width: scroller.availableWidth
        spacing: 12
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label { text: qsTr("Library"); font.pixelSize: 16; font.bold: true }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ComboBox {
                id: libraries
                objectName: "libraries"
                Layout.fillWidth: true
                model: settings.libraries
                currentIndex: Math.max(0, settings.libraries.indexOf(backend.libraryPath))
                Material.roundedScale: theme.radius
                onActivated: (index) => backend.useLibrary(settings.libraries[index])
            }
            AppButton { text: qsTr("Other Folder"); onClicked: backend.chooseLibrary() }
        }

        TextField {
            objectName: "syncTarget"
            Layout.fillWidth: true
            placeholderText: qsTr("Sync with: another computer's name, or name:folder")
            text: settings.syncTarget
            selectByMouse: true
            Material.roundedScale: theme.radius
            onEditingFinished: settings.syncTarget = text
        }

        // Which theme the colors come from, and what Automatic came to.
        Label { visible: !!theme.themes; text: qsTr("Theme"); font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
        RowLayout {
            visible: !!theme.themes
            Layout.fillWidth: true
            spacing: 8
            Label {
                objectName: "themeInUse"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: theme.themes ? themePicker.describe(themePicker.current()) : ""
            }
            AppButton {
                objectName: "chooseTheme"
                text: qsTr("Choose Theme")
                onClicked: {
                    dialog.close()
                    win.showThemes()
                }
            }
        }

        Label { text: qsTr("Reading"); font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("How every book is set, unless it has a way of its own: give it one beside the page, with %1.")
                  .arg((custom.keyText["reader.format"] || "Aa").split("  ")[0])
        }
        // The same controls as the reader's panel, for the defaults.
        FormatControls {
            objectName: "settingsFormat"
            prefix: "settingsFormat"
            Layout.fillWidth: true
        }

        Label { text: qsTr("AI"); font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
        ComboBox {
            id: provider
            objectName: "aiProvider"
            Layout.fillWidth: true
            model: dialog.providers
            textRole: "name"
            currentIndex: Math.max(0, dialog.providers.findIndex(entry => entry.key === settings.aiProvider))
            Material.roundedScale: theme.radius
            onActivated: (index) => settings.aiProvider = dialog.providers[index].key
        }
        TextField {
            Layout.fillWidth: true
            visible: settings.aiProvider === "command"
            placeholderText: qsTr("Command that reads a prompt and writes the answer")
            text: settings.aiCommand
            selectByMouse: true
            Material.roundedScale: theme.radius
            onEditingFinished: settings.aiCommand = text
        }
        // A server: which one, where it is, and its key, which is kept in
        // the keyring and never shown again.
        ComboBox {
            objectName: "aiServer"
            Layout.fillWidth: true
            visible: settings.aiProvider === "openai"
            model: dialog.servers
            textRole: "name"
            currentIndex: Math.max(0, dialog.servers.findIndex(entry => entry.key === aiAgent.serverPreset))
            Material.roundedScale: theme.radius
            onActivated: (index) => aiAgent.setServer(dialog.servers[index].key, "")
        }
        TextField {
            objectName: "aiServerUrl"
            Layout.fillWidth: true
            visible: settings.aiProvider === "openai" && aiAgent.serverPreset !== "omarchy-local"
            placeholderText: qsTr("The server's address, such as https://openrouter.ai/api/v1")
            text: aiAgent.serverUrl
            selectByMouse: true
            Material.roundedScale: theme.radius
            onEditingFinished: aiAgent.setServer(aiAgent.serverPreset, text)
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: settings.aiProvider === "openai" && aiAgent.serverPreset !== "omarchy-local"
            TextField {
                id: serverKey
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
            visible: settings.aiProvider === "openai" && text !== ""
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "aiServerUnencrypted"
            Layout.fillWidth: true
            visible: settings.aiProvider === "openai" && aiAgent.serverInTheClear
            wrapMode: Text.Wrap
            text: qsTr("This address starts with http://, so your questions and the book's text cross the network unencrypted. Use https:// for a server beyond this computer and your tailnet.")
        }
        Label {
            objectName: "aiServerModels"
            Layout.fillWidth: true
            visible: settings.aiProvider === "openai" && aiAgent.modelsProblem !== ""
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: aiAgent.modelsProblem
        }
        // Handwriting and speech go only to one's own servers unless this is
        // on, and then only where a plugin asks for it (#84).
        CheckBox {
            objectName: "recognitionHosted"
            Layout.fillWidth: true
            text: qsTr("Recognise handwriting and speech on hosted servers too, where I choose it")
            checked: settings.recognitionHosted
            onToggled: settings.recognitionHosted = checked
        }
        Label {
            objectName: "recognitionHostedNote"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: settings.recognitionHosted ? theme.colors.orange : theme.colors.muted
            text: settings.recognitionHosted
                  ? qsTr("Careful: what you write by hand and record then goes to someone else's computer wherever you choose it, such as for one notebook, and they can read it.")
                  : qsTr("What you write by hand and record is recognised only on this computer and your tailnet.")
        }
        // A book's text is indexed by its meaning on one's own servers only,
        // unless this is on (#72); and only the books asked for, unless every one.
        CheckBox {
            objectName: "meaningAll"
            Layout.fillWidth: true
            text: qsTr("Index every book by its meaning, not only those I ask for")
            checked: settings.meaningAll
            onToggled: settings.meaningAll = checked
        }
        CheckBox {
            objectName: "meaningHosted"
            Layout.fillWidth: true
            text: qsTr("Index books by their meaning on hosted servers too")
            checked: settings.meaningHosted
            onToggled: settings.meaningHosted = checked
        }
        Label {
            objectName: "meaningHostedNote"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: settings.meaningHosted ? theme.colors.orange : theme.colors.muted
            text: settings.meaningHosted
                  ? qsTr("Careful: a book's text then goes to someone else's computer to be indexed, if that is where the embeddings are made, and you are told so first.")
                  : qsTr("A book's text is indexed by its meaning only on this computer and your tailnet (Ollama's nomic-embed-text unless config.json says another).")
        }
        // The model picker: one model for what you read, a smaller one for
        // what Omabook reads. The lists are those of the AI in use.
        ModelPicker {
            Layout.fillWidth: true
            visible: aiAgent.available
            tier: "main"
            label: qsTr("For answers and writing")
        }
        ModelPicker {
            Layout.fillWidth: true
            visible: aiAgent.available
            tier: "quick"
            label: qsTr("For reading and parsing")
        }
        Label {
            objectName: "aiModelsNote"
            Layout.fillWidth: true
            visible: aiAgent.available
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("Answers, catch-ups and suggestions come from the first. The second reads books into the knowledge graph, judges look-ups and finds series: a small, fast model makes building the graph much cheaper, and often faster. Pick from the list or type any name the AI takes.")
        }
        // Which agent answers and why: automatic follows Omarchy's default agent.
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
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: assistant.available ? qsTr("Questions go to %1, with the part of the book you ask about.").arg(assistant.providerName)
                                      : assistant.problem
        }
        // The knowledge graph of what is read: who and what is in a book,
        // grown by the AI, which is why it says what it costs.
        ComboBox {
            id: graphMode
            objectName: "graphMode"
            Layout.fillWidth: true
            readonly property var modes: [
                { key: "auto", name: qsTr("Knowledge graph: grows as you read (uses AI calls)") },
                { key: "ask", name: qsTr("Knowledge graph: only when you ask for it") },
                { key: "off", name: qsTr("Knowledge graph: off") }
            ]
            model: modes
            textRole: "name"
            currentIndex: Math.max(0, modes.findIndex(entry => entry.key === settings.graph))
            Material.roundedScale: theme.radius
            onActivated: (index) => settings.graph = modes[index].key
        }
        // Off, each collection at the top of the library is a tenant of the
        // graph, its books' knowledge apart from the others' (ADR 0003).
        CheckBox {
            objectName: "graphOneTenant"
            Layout.fillWidth: true
            text: qsTr("Knowledge graph: one for the whole library, not one per top-level collection")
            checked: settings.graphOneTenant
            onToggled: settings.graphOneTenant = checked
        }
        Label {
            objectName: "graphOneTenantNote"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: settings.graphOneTenant
                  ? qsTr("Every book is in one graph, and the AI may link any book with any other.")
                  : qsTr("Each top-level collection has a graph of its own, and so do the books in none (Unfiled): nothing of one is shown or told to the AI in another. The graph window switches between them, with %1.")
                    .arg((custom.keyText["graph.tenant"] || "C").split("  ")[0])
        }
        // What is done about plugins that came from a GitHub repository.
        ComboBox {
            id: pluginUpdates
            objectName: "pluginUpdates"
            Layout.fillWidth: true
            readonly property var modes: [
                { key: "auto", name: qsTr("Plugin updates: install by themselves") },
                { key: "tell", name: qsTr("Plugin updates: tell me") },
                { key: "never", name: qsTr("Plugin updates: never check") }
            ]
            model: modes
            textRole: "name"
            currentIndex: Math.max(0, modes.findIndex(entry => entry.key === settings.pluginUpdates))
            Material.roundedScale: theme.radius
            onActivated: (index) => settings.pluginUpdates = modes[index].key
        }
        // Folio, the file manager, shows each library in its sidebar as a
        // folder of its books, and offers "Add to Library".
        CheckBox {
            objectName: "folioShown"
            Layout.fillWidth: true
            text: qsTr("Show my libraries in the file manager")
            checked: settings.folio
            onToggled: settings.folio = checked
        }
        // Which app opens an ebook is the person's to choose; Omabook only
        // becomes it when asked to here.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                objectName: "opensEbooks"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: backend.fileManager.opensEbooks ? qsTr("Ebooks open in Omabook.")
                                                      : qsTr("Ebooks open in another app.")
            }
            AppButton {
                objectName: "openEbooksWithOmabook"
                visible: !backend.fileManager.opensEbooks
                text: qsTr("Open Ebooks with Omabook")
                onClicked: backend.fileManager.openEbooksWithOmabook()
            }
        }

        DialogActions {
            AppButton {
                objectName: "openPlugins"
                text: qsTr("Plugins")
                onClicked: {
                    dialog.close()
                    win.showPlugins()
                }
            }
            AppButton {
                objectName: "openReadingSites"
                text: qsTr("Reading Sites")
                onClicked: {
                    dialog.close()
                    win.showReadingSites()
                }
            }
            AppButton {
                text: qsTr("Done")
                primary: true
                onClicked: dialog.close()
            }
        }
    }
    }
}
