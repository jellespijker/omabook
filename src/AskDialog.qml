import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One line to the AI and its answer under it. Used twice: to ask about the
// library as a whole, and to ask for a change to how Omabook looks or works.
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

    widest: 640
    onOpened: {
        words.selectAll()
        words.forceActiveFocus()
    }

    DialogBody {

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
        TextField {
            id: words
            objectName: dialog.objectName + "Words"
            Layout.fillWidth: true
            visible: librarian.hasAi
            enabled: !dialog.working
            placeholderText: dialog.placeholder
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: if (text.trim() !== "") dialog.asked(text)
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
