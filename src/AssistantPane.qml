import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The reading companion: ask about what you are reading. It is sent the
// chapter in front of you, or the book so far, and never what comes after.
//
// It can be asked by voice (#149): the question field's microphone, or Ctrl+M
// held in it, hears the question, asks it, and has the answer read aloud as
// well as shown. Asking again, Escape, the button or Shift+R stops it.
Item {
    id: pane
    // The passage selected in the book, if any.
    property string selection: ""
    signal done()
    // A thing of the graph named in an answer was clicked, at a point of this pane.
    signal thingActivated(var id, point at)

    function takeFocus() { question.forceActiveFocus() }
    // The question field, with words to start from (or none), ready to send.
    function prepare(text) {
        question.text = text
        question.selectAll()
        takeFocus()
    }
    function send(text, scope, voiced) {
        if (text.trim() === "" || assistant.busy)
            return
        pane.aloud = !!voiced
        assistant.ask(text, scope, pane.selection)
        question.text = ""
    }

    // The question was said: its answer is read aloud too, once it is written.
    property bool aloud: false
    readonly property var spoken: backend.spokenAnswer
    readonly property string answer: {
        const said = assistant.messages
        return said.length > 0 && said[said.length - 1].role !== "user" ? said[said.length - 1].text : ""
    }
    function readAloud() {
        // One voice at a time: the book's reading aloud waits.
        if (reader.readAloud.speaking) reader.readAloud.toggle()
        spoken.say(answer, assistant.providerName || qsTr("The AI"), Qt.uiLanguage || Qt.locale().name)
    }
    Connections {
        target: assistant
        function onBusyChanged() {
            if (assistant.busy || !pane.aloud) return
            pane.aloud = false
            if (pane.answer !== "") pane.readAloud()
        }
    }

    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: theme.colors.selection
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: assistant.available ? qsTr("Ask %1").arg(assistant.providerName) : qsTr("Ask AI")
                font.pixelSize: 16
                font.bold: true
                elide: Text.ElideRight
            }
            AppButton {
                objectName: "closeAssistant"
                visible: win.touch
                focusPolicy: Qt.NoFocus
                text: qsTr("Close")
                onClicked: pane.done()
            }
        }

        // Without an AI, say what is missing instead of offering a dead box.
        Label {
            objectName: "assistantProblem"
            Layout.fillWidth: true
            visible: !assistant.available
            text: assistant.problem
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }

        ListView {
            id: conversation
            objectName: "conversation"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 12
            model: assistant.messages
            ScrollBar.vertical: ScrollBar {}
            onCountChanged: Qt.callLater(positionViewAtEnd)

            delegate: Item {
                id: turn
                required property var modelData
                readonly property bool mine: modelData.role === "user"
                width: conversation.width
                height: bubble.height

                Rectangle {
                    id: bubble
                    width: parent.width
                    height: words.implicitHeight + (turn.mine ? 16 : 0)
                    radius: Math.min(4, theme.radius)
                    color: turn.mine ? theme.colors.selection : "transparent"

                    Text {
                        id: words
                        // The things of the graph an answer names (a who is
                        // who, a catch-up) marked as on the page, and clickable.
                        readonly property string linked: {
                            reader.marks.known
                            reader.marks.foreground
                            return turn.mine ? "" : reader.marks.linked(turn.modelData.text)
                        }
                        property point pointer: Qt.point(0, 0)
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: turn.mine ? 10 : 0
                        anchors.rightMargin: turn.mine ? 10 : 0
                        text: linked !== "" ? linked : turn.modelData.text
                        textFormat: turn.mine ? Text.PlainText : linked !== "" ? Text.RichText : Text.MarkdownText
                        linkColor: theme.foreground
                        onLinkActivated: (link) => {
                            if (link.startsWith("omabook-thing:")) {
                                const at = words.mapToItem(pane, words.pointer.x, words.pointer.y)
                                pane.thingActivated(Number(link.slice(14)), Qt.point(at.x, at.y))
                            } else {
                                Qt.openUrlExternally(link)
                            }
                        }
                        HoverHandler {
                            cursorShape: words.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onPointChanged: words.pointer = point.position
                        }
                        wrapMode: Text.Wrap
                        color: theme.foreground
                        font.pixelSize: 14
                        lineHeight: 1.3
                    }
                }
            }

            // The answer as it is written.
            footer: Item {
                width: conversation.width
                height: assistant.busy ? writing.y + writing.implicitHeight + 12 : 0
                Thinking {
                    id: working
                    objectName: "assistantThinking"
                    y: 12
                    active: assistant.busy
                    text: assistant.streaming !== "" ? qsTr("Writing…") : qsTr("Reading…")
                }
                Text {
                    id: writing
                    objectName: "streaming"
                    y: working.y + working.height + 8
                    width: parent.width
                    visible: assistant.busy && assistant.streaming !== ""
                    text: assistant.streaming
                    textFormat: Text.MarkdownText
                    wrapMode: Text.Wrap
                    color: theme.foreground
                    font.pixelSize: 14
                    lineHeight: 1.3
                    onImplicitHeightChanged: Qt.callLater(conversation.positionViewAtEnd)
                }
            }
        }

        Label {
            objectName: "assistantNote"
            Layout.fillWidth: true
            visible: text !== ""
            text: assistant.note
            color: theme.colors.orange
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }

        Flow {
            Layout.fillWidth: true
            visible: assistant.available
            spacing: 8

            AppButton {
                objectName: "explainSelection"
                visible: pane.selection !== ""
                enabled: !assistant.busy
                focusPolicy: Qt.NoFocus
                text: qsTr("Explain the selection")
                onClicked: pane.send(qsTr("Explain the passage I selected."), "chapter")
            }
            AppButton {
                objectName: "summarizeChapter"
                enabled: !assistant.busy
                focusPolicy: Qt.NoFocus
                text: qsTr("Summarize this chapter")
                onClicked: pane.send(qsTr("Summarize this chapter."), "chapter")
            }
            AppButton {
                objectName: "catchUp"
                enabled: !assistant.busy
                focusPolicy: Qt.NoFocus
                text: qsTr("Catch me up")
                onClicked: assistant.catchUp()
            }
            AppButton {
                objectName: "whoIsWho"
                enabled: !assistant.busy
                focusPolicy: Qt.NoFocus
                text: qsTr("Who is who")
                onClicked: assistant.whoIsWho()
            }
            AppButton {
                visible: assistant.busy
                focusPolicy: Qt.NoFocus
                text: qsTr("Stop")
                // What was written so far is not read aloud.
                onClicked: {
                    pane.aloud = false
                    assistant.cancel()
                }
            }
            AppButton {
                objectName: "answerAloud"
                visible: pane.spoken.speaking || (pane.answer !== "" && !assistant.busy)
                focusPolicy: Qt.NoFocus
                text: pane.spoken.speaking ? qsTr("Stop reading") : qsTr("Read aloud")
                Accessible.description: qsTr("Reads the answer aloud")
                onClicked: pane.spoken.speaking ? pane.spoken.stop() : pane.readAloud()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: assistant.available
            spacing: 6
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            TextField {
                id: question
                objectName: "question"
                Layout.fillWidth: true
                placeholderText: qsTr("Ask about this chapter")
                selectByMouse: true
                Material.roundedScale: theme.radius
                // Typed after voxtype's own dictation typed it: still said.
                onAccepted: {
                    pane.send(text, "chapter", mic.voxtyped)
                    mic.voxtyped = false
                }
                // An answer being read stops first; then the companion closes.
                Keys.onEscapePressed: pane.spoken.speaking ? pane.spoken.stop() : pane.done()
            }
            // Asked by voice: held while speaking, or Ctrl+M in the field (#148).
            DictationButton {
                id: mic
                objectName: "askByVoice"
                target: question
                active: assistant.available
                property bool voxtyped: false
                onStarted: {
                    voxtyped = false
                    pane.spoken.stop()
                    if (reader.readAloud.speaking) reader.readAloud.toggle()
                }
                onWritten: pane.send(question.text, "chapter", true)
                onVoxtyping: voxtyped = true
            }
        }
    }
}
