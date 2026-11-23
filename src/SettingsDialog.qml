import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The few choices there are: which library, how the page is set, and the
// way to the AI's page.
// Each takes effect as it is made.
AppPopup {
    id: dialog
    objectName: "settings"
    widest: 560
    onAboutToShow: {
        backend.fileManager.checkOpensEbooks()
        if (snapshotDesk.ready)
            snapshotDesk.refresh()
    }

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

        // The AI has a page of its own: a provider per kind of work (#144).
        Label { text: qsTr("AI"); font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                objectName: "aiSummary"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.muted
                text: settings.aiProvider === "off" ? qsTr("Off: no model is asked anything, and nothing is sent.")
                    : assistant.available ? qsTr("Answers come from %1. Handwriting, speech, voices and the meaning index each have a provider of their own.").arg(assistant.providerName)
                    : assistant.problem
            }
            AppButton {
                objectName: "openAi"
                text: qsTr("AI Settings")
                onClicked: {
                    dialog.close()
                    win.showAiSettings()
                }
            }
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
        // What is edited here goes into the book's own EPUB too (#74): the
        // library's copy, never the file it was added from.
        CheckBox {
            objectName: "writeDetails"
            Layout.fillWidth: true
            text: qsTr("Write a book's details into its EPUB whenever they are edited")
            checked: settings.writeDetails
            onToggled: settings.writeDetails = checked
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

        // Phones and e-readers have a panel of their own (#177): here, whether they are served.
        Label { text: qsTr("Phones and e-readers"); font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            PhonesStatus { objectName: "phonesSummary" }
            AppButton {
                objectName: "openPhones"
                text: qsTr("Phones and E-readers")
                onClicked: {
                    dialog.close()
                    win.showPhones()
                }
            }
        }

        // Snapshots have a panel of their own (#185): here, how they stand.
        Label { text: qsTr("Snapshots"); font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            SnapshotsSummary { objectName: "snapshotsSummary" }
            AppButton {
                objectName: "openSnapshots"
                text: qsTr("Snapshots")
                onClicked: {
                    dialog.close()
                    win.showSnapshots()
                }
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
