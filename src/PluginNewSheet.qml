import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Starting a plugin of one's own: a name and one of the templates. It is
// made in the plugin folder, which is then shown, since a new plugin is
// something to edit.
AppPopup {
    id: sheet
    objectName: "pluginNew"
    property var host
    property int chosen: 0
    property string problem: ""
    // The plugin that was made, for the dialog to select.
    signal made(string id)

    widest: 520
    // What each template is, in a line.
    readonly property var templateNotes: ({
        metadata: qsTr("A source of book details, asked in every look-up"),
        command: qsTr("A command with a key of its own"),
        hook: qsTr("Something done whenever a book is added, opened or closed"),
        catalog: qsTr("A format to write the library out in"),
        format: qsTr("A book format Omabook doesn't read by itself"),
        panel: qsTr("A panel beside the shelf or the page")
    })
    readonly property var templates: host ? host.manager.templates : []
    readonly property string folderName: host ? host.manager.folderName(field.text) : ""

    // The template that is chosen stays in sight where the list scrolls.
    onChosenChanged: {
        const row = choices.children[chosen]
        if (!row)
            return
        if (row.y < templateList.contentY)
            templateList.contentY = row.y
        else if (row.y + row.height > templateList.contentY + templateList.height)
            templateList.contentY = row.y + row.height - templateList.height
    }

    function show() {
        field.text = ""
        problem = ""
        chosen = Math.max(0, templates.indexOf("hook"))
        open()
        field.forceActiveFocus()
    }
    function create() {
        if (folderName === "")
            return
        const answer = host.manager.create(folderName, templates[chosen])
        if (answer.error) {
            problem = answer.error
            return
        }
        close()
        made(answer.id)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            Layout.fillWidth: true
            text: qsTr("New plugin")
            font.pixelSize: 18
            font.bold: true
        }
        TextField {
            id: field
            objectName: "pluginNewName"
            Layout.fillWidth: true
            placeholderText: qsTr("Its name")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onTextEdited: sheet.problem = ""
            Keys.onDownPressed: sheet.chosen = Math.min(sheet.templates.length - 1, sheet.chosen + 1)
            Keys.onUpPressed: sheet.chosen = Math.max(0, sheet.chosen - 1)
            onAccepted: sheet.create()
        }
        // The templates scroll where the window is too low for all of them.
        Flickable {
            id: templateList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: choices.implicitHeight
            Layout.minimumHeight: 72
            contentHeight: choices.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: choices
                width: templateList.width
                spacing: 2
                Repeater {
                    model: sheet.templates
                    delegate: Rectangle {
                        id: choice
                        required property int index
                        required property string modelData
                        objectName: "pluginTemplate_" + modelData
                        Layout.fillWidth: true
                        implicitHeight: Math.max(win.touch ? 44 : 34, line.implicitHeight + 12)
                        radius: Math.min(4, theme.radius)
                        color: sheet.chosen === index ? Qt.alpha(theme.foreground, 0.13) : "transparent"

                        RowLayout {
                            id: line
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 10
                            PluginMark { condition: sheet.chosen === choice.index ? "on" : "off" }
                            Label {
                                Layout.fillWidth: true
                                text: sheet.templateNotes[choice.modelData] || choice.modelData
                                wrapMode: Text.Wrap
                            }
                        }
                        TapHandler { onTapped: sheet.chosen = choice.index }
                    }
                }
            }
        }
        Label {
            objectName: "pluginNewNote"
            Layout.fillWidth: true
            // Where it will live, or why it can't be made.
            text: sheet.problem !== "" ? sheet.problem
                  : sheet.folderName !== "" ? qsTr("It starts as a working example in %1, for you or an agent to edit.").arg(sheet.host.manager.folder + "/" + sheet.folderName)
                  : field.text.trim() !== "" ? qsTr("That name has no letters or digits to make a folder name of.")
                  : qsTr("It starts as a working example, for you or an agent to edit.")
            textFormat: Text.PlainText
            color: sheet.problem !== "" ? theme.colors.orange : sheet.host ? sheet.host.quiet : theme.foreground
            font.pixelSize: 12
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }
        DialogActions {
            AppButton { text: qsTr("Cancel"); onClicked: sheet.close() }
            AppButton {
                objectName: "pluginNewCreate"
                text: qsTr("Create")
                primary: true
                enabled: sheet.folderName !== ""
                onClicked: sheet.create()
            }
        }
    }
}
