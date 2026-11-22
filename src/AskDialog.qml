import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One line to the AI and its answer under it. Used twice: to ask about the
// library as a whole, and to ask for a change to how Omabook looks or works.
//
// Where it may be asked by voice (`voice`, #149), the line's microphone, or
// Ctrl+M held in it, hears the question, asks it, and has the answer read
// aloud as well as shown; asking again, the button or Shift+R stops it.
AppPopup {
    id: dialog
    property string heading
    property string placeholder
    // What is shown under the line: the answer, or what a change did.
    property string reply
    property bool working: false
    property bool undoable: false
    // A ready-made request offered as a button, when there is one.
    property string offer
    signal offered()
    // Books suggested in answer, each with what a reading site says of it,
    // and the wish to put one on the to-read shelf.
    property var suggestions: []
    signal wanted(int index)
    signal asked(string words)
    signal undo()
    // A collection the question can keep to, by its name; none when empty.
    property string onlyName
    property bool only: false
    signal onlyToggled(bool only)
    // It may be asked by voice, and its answer read aloud.
    property bool voice: false
    // The question was said: its answer is read aloud too, once it is written.
    property bool aloud: false
    readonly property var spoken: backend.spokenAnswer
    function readAloud() {
        spoken.say(reply, assistant.providerName || qsTr("The AI"), Qt.uiLanguage || Qt.locale().name)
    }
    function ask(text, voiced) {
        if (text.trim() === "") return
        aloud = voiced
        asked(text)
    }
    onWorkingChanged: {
        if (working || !aloud) return
        aloud = false
        if (reply !== "") readAloud()
    }

    widest: 640
    onOpened: {
        words.selectAll()
        words.forceActiveFocus()
    }

    DialogBody {
        id: body

        Label {
            Layout.fillWidth: true
            text: dialog.heading
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: !librarian.hasAi
            text: librarian.aiProblem
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            // Clear of the scroll bar, which would take the microphone's presses.
            Layout.rightMargin: body.barRoom
            visible: librarian.hasAi
            spacing: 6
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            TextField {
                id: words
                objectName: dialog.objectName + "Words"
                Layout.fillWidth: true
                enabled: !dialog.working
                placeholderText: dialog.placeholder
                selectByMouse: true
                Material.roundedScale: theme.radius
                // Typed after voxtype's own dictation typed it: still said.
                onAccepted: {
                    dialog.ask(text, mic.voxtyped)
                    mic.voxtyped = false
                }
            }
            // Asked by voice: held while speaking, or Ctrl+M in the line (#148).
            DictationButton {
                id: mic
                objectName: dialog.objectName + "Voice"
                target: words
                active: dialog.voice && dialog.opened && librarian.hasAi
                property bool voxtyped: false
                onStarted: {
                    voxtyped = false
                    dialog.spoken.stop()
                }
                onWritten: dialog.ask(words.text, true)
                onVoxtyping: voxtyped = true
            }
        }
        Switch {
            objectName: dialog.objectName + "Only"
            Layout.fillWidth: true
            // Tall enough for a fingertip.
            Layout.minimumHeight: 40
            visible: dialog.onlyName !== "" && librarian.hasAi
            enabled: !dialog.working
            text: qsTr("Only the books in %1").arg(dialog.onlyName)
            checked: dialog.only
            Accessible.name: text
            onToggled: dialog.onlyToggled(checked)
        }
        // At work: thinking before the first word, writing after it.
        Thinking {
            objectName: dialog.objectName + "Thinking"
            active: dialog.working
            text: dialog.reply !== "" ? qsTr("Writing…") : qsTr("Thinking…")
        }
        // The dialog scrolls a long answer, so it needs no scrolling of its own.
        Text {
            id: answer
            objectName: dialog.objectName + "Reply"
            Layout.fillWidth: true
            visible: dialog.reply !== ""
            text: dialog.reply
            textFormat: Text.MarkdownText
            wrapMode: Text.Wrap
            color: theme.foreground
            font.pixelSize: 14
            lineHeight: 1.3
        }
        ListView {
            id: suggested
            objectName: dialog.objectName + "Suggestions"
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            visible: count > 0
            interactive: false
            spacing: 12
            model: dialog.suggestions
            delegate: RowLayout {
                width: suggested.width
                spacing: 12
                // A book no site knows may not exist, and reads as doubtful.
                opacity: modelData.state === "missing" ? 0.55 : 1
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        text: modelData.author !== "" ? qsTr("%1, by %2").arg(modelData.title).arg(modelData.author) : modelData.title
                        font.bold: true
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: [modelData.rating, modelData.note].filter(part => part !== "").join(" · ")
                        color: theme.colors.muted
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: modelData.why
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                }
                AppButton {
                    objectName: dialog.objectName + "Want" + index
                    Layout.alignment: Qt.AlignTop
                    visible: modelData.canWant
                    text: qsTr("Add to to-read")
                    onClicked: dialog.wanted(index)
                }
            }
        }
        actions: [
            AppButton {
                objectName: dialog.objectName + "Offer"
                visible: dialog.offer !== "" && librarian.hasAi
                enabled: !dialog.working
                text: dialog.offer
                onClicked: dialog.offered()
            },
            AppButton {
                objectName: dialog.objectName + "Aloud"
                visible: dialog.voice && (dialog.spoken.speaking || (dialog.reply !== "" && !dialog.working))
                text: dialog.spoken.speaking ? qsTr("Stop reading") : qsTr("Read aloud")
                Accessible.description: qsTr("Reads the answer aloud")
                onClicked: dialog.spoken.speaking ? dialog.spoken.stop() : dialog.readAloud()
            },
            AppButton {
                objectName: dialog.objectName + "Undo"
                visible: dialog.undoable
                enabled: !dialog.working
                text: qsTr("Undo the last change")
                onClicked: dialog.undo()
            },
            AppButton { text: qsTr("Close"); onClicked: dialog.close() }
        ]
    }
}
