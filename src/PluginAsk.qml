import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A plugin asking the reader for words (omabook.ui.ask): its question, a
// place to type, and nothing else. Enter answers, Shift+Enter is a new
// line, and Escape leaves the question unanswered, which the plugin hears
// of as no answer rather than an empty one.
AppPopup {
    id: dialog
    objectName: "pluginAsk"
    readonly property var question: backend.scripts.question
    // The question on show, so each is answered or left exactly once.
    property int shown: -1

    widest: 560
    closePolicy: Popup.CloseOnEscape

    function accept() {
        if (shown < 0) return
        const id = shown
        shown = -1
        backend.scripts.answer(id, words.text)
    }

    onQuestionChanged: {
        // A form is PluginForm's to show.
        if (question.id === undefined || question.form !== undefined) {
            shown = -1
            close()
        } else if (question.id !== shown) {
            shown = question.id
            words.text = question.text
            open()
        }
    }
    onOpened: {
        words.selectAll()
        words.forceActiveFocus()
    }
    onClosed: {
        if (shown < 0) return
        const id = shown
        shown = -1
        backend.scripts.dismiss(id)
    }
    // The window is there to be asked, for as long as it is.
    Component.onCompleted: backend.scripts.attend(true)
    Component.onDestruction: backend.scripts.attend(false)

    DialogBody {

        Label {
            objectName: "pluginAskTitle"
            Layout.fillWidth: true
            text: dialog.question.title || ""
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
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(240, Math.max(96, words.implicitHeight))
            TextArea {
                id: words
                objectName: "pluginAskWords"
                placeholderText: dialog.question.placeholder || ""
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                Material.roundedScale: theme.radius
                Keys.onReturnPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier) event.accepted = false
                    else dialog.accept()
                }
                Keys.onEnterPressed: dialog.accept()
            }
        }
        actions: [
            AppButton { text: qsTr("Cancel"); focusPolicy: Qt.NoFocus; onClicked: dialog.close() },
            AppButton { objectName: "pluginAskDone"; text: qsTr("Done"); primary: true; focusPolicy: Qt.NoFocus; onClicked: dialog.accept() }
        ]
    }
}
