import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Which theme the colors come from: Automatic (Omarchy's, else Aether's,
// else Tokyo Night), Omarchy's or Aether's followed as they change, or one
// of Omarchy's own themes. Moving through the list shows each on the whole
// window at once; Enter or Use Theme keeps it, and Escape puts back the one
// there was. The choice is "theme" in style.json, under the colors set there.
AppPopup {
    id: picker
    objectName: "themePicker"
    widest: 520
    // Beside the shelf where the window has room, and over it otherwise,
    // with nothing dimmed: what is behind is the preview.
    readonly property bool beside: win.width >= 2 * widest
    anchors.centerIn: beside ? undefined : Overlay.overlay
    x: beside ? (win.rightToLeft ? win.dialogEdge : Overlay.overlay.width - width - win.dialogEdge) : 0
    y: beside ? win.dialogEdge : 0
    Overlay.modal: Item {}

    readonly property var choice: theme.themes
    readonly property var options: choice ? choice.options : []
    // Set once a theme is kept, so closing doesn't put the old one back.
    property bool kept: false
    property string problem: ""

    // How an option is named, the same here and in the settings.
    function describe(option) {
        if (!option)
            return ""
        const source = option.source || {}
        if (option.choice === "auto")
            return source.kind === "omarchy" ? qsTr("Automatic: Omarchy — %1").arg(source.name)
                 : source.kind === "aether" ? qsTr("Automatic: Aether")
                 : qsTr("Automatic: %1").arg(source.name)
        if (option.choice === "omarchy")
            return qsTr("Omarchy — %1").arg(source.name)
        if (option.choice === "aether")
            return qsTr("Aether")
        return option.name
    }
    // What the row says under its name.
    function detail(option) {
        if (option.choice === "auto")
            return qsTr("Omarchy's, else Aether's, else Tokyo Night")
        if (option.choice === "omarchy")
            return qsTr("Follows Omarchy's theme as it changes")
        if (option.choice === "aether")
            return qsTr("Follows Aether's palette as it changes")
        const kind = option.kind === "user" ? qsTr("Your own") : option.kind === "file" ? qsTr("A file of your own") : ""
        const shade = option.light ? qsTr("Light") : qsTr("Dark")
        return kind === "" ? shade : kind + " · " + shade
    }
    // The option the kept choice is, for the settings to name.
    function current() {
        if (!choice)
            return null
        const found = options.find(option => option.choice === choice.choice)
        return found ? found : { choice: choice.choice, name: choice.source.name, source: choice.source }
    }

    function keep(index) {
        const option = options[index]
        if (!option)
            return
        problem = choice.choose(option.choice)
        if (problem !== "")
            return
        kept = true
        close()
    }

    onOpened: {
        kept = false
        problem = ""
        list.currentIndex = Math.max(0, options.findIndex(option => option.choice === choice.choice))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        list.forceActiveFocus()
    }
    // At once, not when the closing has been drawn.
    onAboutToHide: if (choice && !kept) choice.endPreview()

    DialogBody {
        Label {
            text: qsTr("Theme")
            font.pixelSize: 18
            font.bold: true
        }
        Label {
            objectName: "themeNow"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: picker.choice && picker.choice.source.note
                  ? picker.choice.source.note
                  : qsTr("Your own colors in style.json stay over the theme. This changes Omabook only, not the desktop.")
        }
        ListView {
            id: list
            objectName: "themeList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Math.max(132, win.height - 300))
            clip: true
            model: picker.options
            keyNavigationWraps: false
            boundsBehavior: Flickable.StopAtBounds
            // The list keeps up with the keyboard rather than gliding after it.
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar {}
            // Each theme is shown as the keyboard reaches it.
            onCurrentIndexChanged: {
                if (picker.opened && picker.options[currentIndex])
                    picker.choice.preview(picker.options[currentIndex].choice)
            }
            Keys.onReturnPressed: picker.keep(currentIndex)
            Keys.onEnterPressed: picker.keep(currentIndex)

            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                readonly property bool inUse: picker.choice && modelData.choice === picker.choice.choice
                objectName: "theme-" + modelData.choice
                width: list.width
                height: parts.implicitHeight + 14
                radius: Math.min(4, theme.radius)
                color: list.currentIndex === index ? theme.colors.selection : "transparent"

                RowLayout {
                    id: parts
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 12

                    // The theme in small: its background, and on it the
                    // text color, the accent and the hues.
                    Rectangle {
                        objectName: "themeSwatch"
                        implicitWidth: 92
                        implicitHeight: 26
                        radius: Math.min(4, theme.radius)
                        color: row.modelData.swatch[0]
                        border.color: theme.colors.selection
                        Row {
                            anchors.centerIn: parent
                            spacing: 3
                            Repeater {
                                model: row.modelData.swatch.slice(1)
                                Rectangle {
                                    required property int index
                                    required property string modelData
                                    width: index < 2 ? 12 : 8
                                    height: 14
                                    radius: Math.min(2, theme.radius)
                                    color: modelData
                                }
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: picker.describe(row.modelData)
                            font.bold: row.inUse
                            color: row.inUse ? theme.accent : theme.foreground
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: picker.detail(row.modelData)
                            color: theme.colors.muted
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                    // Only while the row is wholly in view: a curve-drawn
                    // glyph isn't cut by the list's clip.
                    Glyph {
                        visible: row.inUse && row.y >= list.contentY - 1 && row.y + row.height <= list.contentY + list.height + 1
                        name: "check"
                        color: theme.accent
                    }
                }
                TapHandler {
                    onTapped: list.currentIndex = row.index
                    onDoubleTapped: picker.keep(row.index)
                }
            }
        }
        Label {
            objectName: "themeProblem"
            Layout.fillWidth: true
            visible: picker.problem !== ""
            text: picker.problem
            color: theme.colors.red
            wrapMode: Text.Wrap
        }
        actions: [
            AppButton {
                objectName: "themeCancel"
                text: qsTr("Cancel")
                onClicked: picker.close()
            },
            AppButton {
                objectName: "themeUse"
                text: qsTr("Use Theme")
                primary: true
                onClicked: picker.keep(list.currentIndex)
            }
        ]
    }
}
