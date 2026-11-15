import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A plugin asking the reader to fill in a form (omabook.ui.form). The plugin
// says what is asked; how it looks is Omabook's: the theme's colors and
// corners, Omabook's own words for its buttons, and the same layout under
// fingers as under a mouse. Tab goes from field to field, Enter in a line of
// words sends the form, and Escape leaves it, which the plugin hears of as
// no answer at all. A field that must be filled in keeps the form from being
// sent until it is, and says so.
AppPopup {
    id: dialog
    objectName: "pluginForm"
    readonly property var question: backend.scripts.question
    readonly property var form: question.form || ({})
    readonly property var fields: form.fields || []
    // The form on show, so each is answered or left exactly once.
    property int shown: -1
    // What is filled in so far, by field.
    property var values: ({})
    readonly property string missing: shown < 0 ? "" : backend.scripts.formMissing(form, values)

    widest: 600
    closePolicy: Popup.CloseOnEscape

    function set(id, value) {
        const next = Object.assign({}, values)
        next[id] = value
        values = next
    }
    function accept() {
        if (shown < 0 || missing !== "") return
        const id = shown
        if (backend.scripts.fill(id, values))
            shown = -1
    }

    onQuestionChanged: {
        if (question.form === undefined) {
            if (shown >= 0) {
                shown = -1
                close()
            }
        } else if (question.id !== shown) {
            shown = question.id
            values = backend.scripts.formValues(question.form, {})
            open()
        }
    }
    // The first field takes the keyboard; with none, the button that sends
    // it does, so Enter is yes.
    onOpened: Qt.callLater(() => { if (!column.focusFirst()) done.forceActiveFocus() })
    onClosed: {
        if (shown < 0) return
        const id = shown
        shown = -1
        backend.scripts.dismiss(id)
    }
    Component.onCompleted: backend.scripts.attend(true)
    Component.onDestruction: backend.scripts.attend(false)

    DialogBody {
        Label {
            objectName: "pluginFormTitle"
            Layout.fillWidth: true
            text: dialog.form.title || ""
            textFormat: Text.PlainText
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            // "Asked by Storyteller"
            text: qsTr("Asked by %1").arg(dialog.question.plugin || "")
            textFormat: Text.PlainText
            color: theme.colors.muted
            font.pixelSize: 12
        }
        Label {
            visible: text !== ""
            Layout.fillWidth: true
            text: dialog.form.intro || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }

        ColumnLayout {
            id: column
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 14
            function focusFirst() {
                for (let i = 0; i < repeater.count; ++i) {
                    const item = repeater.itemAt(i)
                    if (item && item.visible && item.takeFocus()) return true
                }
                return false
            }

            Repeater {
                id: repeater
                model: dialog.shown >= 0 ? dialog.fields : []
                delegate: ColumnLayout {
                    id: entry
                    required property var modelData
                    required property int index
                    readonly property var field: modelData
                    readonly property var value: dialog.values[field.id]
                    readonly property bool labelled: field.type !== "toggle"
                    objectName: "pluginFormField_" + field.id
                    Layout.fillWidth: true
                    spacing: 4
                    visible: backend.scripts.fieldShown(field, dialog.values)
                    function takeFocus() {
                        if (!editor.item) return false
                        if (typeof editor.item.takeFocus === "function") editor.item.takeFocus()
                        else editor.item.forceActiveFocus()
                        return true
                    }

                    Label {
                        visible: entry.labelled
                        Layout.fillWidth: true
                        text: entry.field.label + (entry.field.required ? " *" : "")
                        textFormat: Text.PlainText
                        font.bold: true
                        wrapMode: Text.Wrap
                    }
                    Loader {
                        id: editor
                        Layout.fillWidth: true
                        sourceComponent: entry.field.type === "textarea" ? manyLines
                                         : entry.field.type === "choice" ? ((entry.field.choices || []).length <= 6 ? radios : combo)
                                         : entry.field.type === "multi" ? ticks
                                         : entry.field.type === "number" ? count
                                         : entry.field.type === "toggle" ? toggle
                                         : oneLine
                        // What each kind of editor needs to know of its field.
                        property var field: entry.field
                        property var value: entry.value
                    }
                    Label {
                        visible: text !== ""
                        Layout.fillWidth: true
                        text: entry.field.help || ""
                        textFormat: Text.PlainText
                        color: theme.colors.muted
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
        Label {
            objectName: "pluginFormMissing"
            visible: dialog.missing !== ""
            Layout.fillWidth: true
            // "Fill in “What should it be?” first."
            text: qsTr("Fill in “%1” first.").arg(dialog.missing)
            textFormat: Text.PlainText
            color: theme.colors.orange
            wrapMode: Text.Wrap
        }

        actions: [
            AppButton { objectName: "pluginFormCancel"; text: qsTr("Cancel"); focusPolicy: Qt.NoFocus; onClicked: dialog.close() },
            AppButton {
                id: done
                objectName: "pluginFormDone"
                text: dialog.form.accept || qsTr("Done")
                primary: true
                enabled: dialog.missing === ""
                focusPolicy: Qt.NoFocus
                onClicked: dialog.accept()
            }
        ]
    }

    // The editors, one per kind of field. Each says what it was changed to
    // through dialog.set, and shows what dialog.values holds.
    Component {
        id: oneLine
        TextField {
            readonly property var field: parent.field
            objectName: "pluginFormText_" + field.id
            text: parent.value || ""
            placeholderText: field.placeholder || ""
            selectByMouse: true
            Material.roundedScale: theme.radius
            onTextEdited: dialog.set(field.id, text)
            onAccepted: dialog.accept()
        }
    }
    Component {
        id: manyLines
        ScrollView {
            id: scroller
            readonly property var field: parent.field
            readonly property var start: parent.value
            implicitHeight: Math.min(200, Math.max(96, area.implicitHeight))
            function takeFocus() { area.forceActiveFocus() }
            TextArea {
                id: area
                objectName: "pluginFormText_" + scroller.field.id
                text: scroller.start || ""
                placeholderText: scroller.field.placeholder || ""
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                Material.roundedScale: theme.radius
                onTextChanged: if (activeFocus) dialog.set(scroller.field.id, text)
            }
        }
    }
    Component {
        id: radios
        ColumnLayout {
            id: group
            readonly property var field: parent.field
            readonly property var value: parent.value
            spacing: 0
            function takeFocus() {
                for (let i = 0; i < options.count; ++i)
                    if (options.itemAt(i).checked) { options.itemAt(i).forceActiveFocus(); return }
            }
            ButtonGroup { id: exclusive }
            Repeater {
                id: options
                model: group.field.choices
                delegate: RadioButton {
                    id: option
                    required property var modelData
                    objectName: "pluginFormChoice_" + group.field.id + "_" + modelData.value
                    Layout.fillWidth: true
                    ButtonGroup.group: exclusive
                    checked: group.value === modelData.value
                    implicitHeight: Math.max(win.touch ? 48 : 36, implicitContentHeight + topPadding + bottomPadding)
                    onToggled: if (checked) dialog.set(group.field.id, modelData.value)
                    contentItem: Column {
                        leftPadding: option.indicator && !option.mirrored ? option.indicator.width + option.spacing : 0
                        rightPadding: option.indicator && option.mirrored ? option.indicator.width + option.spacing : 0
                        spacing: 2
                        Label {
                            width: option.availableWidth - parent.leftPadding - parent.rightPadding
                            text: option.modelData.label
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        Label {
                            visible: text !== ""
                            width: option.availableWidth - parent.leftPadding - parent.rightPadding
                            text: option.modelData.help || ""
                            textFormat: Text.PlainText
                            color: theme.colors.muted
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
    }
    Component {
        id: combo
        ComboBox {
            readonly property var field: parent.field
            objectName: "pluginFormCombo_" + field.id
            model: field.choices
            textRole: "label"
            valueRole: "value"
            currentIndex: Math.max(0, (field.choices || []).findIndex(choice => choice.value === parent.value))
            implicitHeight: win.touch ? 48 : 40
            Material.roundedScale: theme.radius
            onActivated: dialog.set(field.id, currentValue)
        }
    }
    Component {
        id: ticks
        Flow {
            id: flow
            readonly property var field: parent.field
            readonly property var value: parent.value || []
            spacing: 4
            function takeFocus() { if (boxes.count > 0) boxes.itemAt(0).forceActiveFocus() }
            Repeater {
                id: boxes
                model: flow.field.choices
                delegate: CheckBox {
                    required property var modelData
                    objectName: "pluginFormTick_" + flow.field.id + "_" + modelData.value
                    text: modelData.label
                    checked: flow.value.indexOf(modelData.value) >= 0
                    implicitHeight: win.touch ? 48 : 36
                    ToolTip.visible: hovered && (modelData.help || "") !== ""
                    ToolTip.text: modelData.help || ""
                    ToolTip.delay: 600
                    onToggled: {
                        const now = flow.value.filter(value => value !== modelData.value)
                        if (checked) now.push(modelData.value)
                        dialog.set(flow.field.id, now)
                    }
                }
            }
        }
    }
    Component {
        id: count
        SpinBox {
            readonly property var field: parent.field
            objectName: "pluginFormNumber_" + field.id
            from: field.min !== undefined ? Math.ceil(field.min) : -1000000
            to: field.max !== undefined ? Math.floor(field.max) : 1000000
            stepSize: Math.max(1, Math.round(field.step || 1))
            value: Number(parent.value) || 0
            editable: true
            implicitHeight: win.touch ? 48 : 40
            Material.roundedScale: theme.radius
            onValueModified: dialog.set(field.id, value)
        }
    }
    Component {
        id: toggle
        Switch {
            readonly property var field: parent.field
            objectName: "pluginFormToggle_" + field.id
            text: field.label
            checked: parent.value === true
            implicitHeight: win.touch ? 48 : 36
            onToggled: dialog.set(field.id, checked)
        }
    }
}
