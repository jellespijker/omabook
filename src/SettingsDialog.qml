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

    readonly property var providers: [
        { key: "auto", name: qsTr("Automatic") },
        { key: "claude", name: qsTr("Claude Code, with your subscription") },
        { key: "agy", name: qsTr("Antigravity, with your subscription") },
        { key: "anthropic", name: qsTr("Claude, with an API key") },
        { key: "command", name: qsTr("A command") },
        { key: "off", name: qsTr("Off") }
    ]

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
        // Folio, the file manager, shows Omabook's libraries and what is
        // being read in its sidebar, and offers "Add to Library".
        CheckBox {
            objectName: "folioShown"
            Layout.fillWidth: true
            text: qsTr("Show my libraries and what I'm reading in the file manager")
            checked: settings.folio
            onToggled: settings.folio = checked
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
