import QtQuick
import QtQuick.Controls

// The pen over the page. Over words, its gestures highlight them (#81,
// PenGestures). While the reader writes ({reader.ink}) it writes and draws
// ink instead, its other end erasing: on the pages of a PDF or a comic (#83,
// ReadingInk, an ink surface over each page in PageReader), and on a flowing
// book in its margins and between its lines (#152, ReadingLineInk, an ink
// surface over each chapter shown, LineInkLayer); this bar above the page then
// says what it writes with: the pen, a highlighter, the eraser or a lasso. The
// ink is put away and shown again for reading ({reader.show_ink}), and undone
// and done again (Ctrl+Z, Ctrl+Shift+Z) a pause of the pen at a time, wherever it was.
Item {
    id: pen
    objectName: "readerPen"
    // The reading view: its page (`content`) and its menu.
    property Item host
    readonly property bool canInk: reader.open
    readonly property bool inking: canInk && reader.ink.inking
    // Where what the pen did is kept and undone: a flowing book's lines, or the pages.
    readonly property var kept: reader.kind === "flow" ? reader.lineInk : reader.ink
    // What lies over the pages, where the pen writes on it and not on them.
    readonly property Item covering: bar

    function toggle() {
        reader.ink.inking = !reader.ink.inking
        backend.say(reader.ink.inking ? qsTr("Writing on the pages: the pen's other end erases")
                                      : qsTr("Reading the pages"))
    }
    // Whether a finger writes (#134): until a pen is used, always, never, and round again; kept.
    function cycleFingers() {
        const next = reader.ink.fingers === "auto" ? "on" : reader.ink.fingers === "on" ? "off" : "auto"
        reader.ink.fingers = next
        backend.say(next === "on" ? qsTr("A finger writes; two fingers scroll")
                  : next === "off" ? qsTr("A finger scrolls; the pen and the mouse write")
                  : qsTr("A finger writes until a pen is used, then scrolls"))
    }
    function toggleShown() {
        reader.ink.shown = !reader.ink.shown
        backend.say(reader.ink.shown ? qsTr("Ink shown") : qsTr("Ink put away"))
    }

    // A flowing book's inks are drawn to read on the page as the theme colours it.
    Binding { target: reader.lineInk; property: "foreground"; value: theme.foreground }
    Binding { target: reader.lineInk; property: "background"; value: theme.background }

    PenGestures {
        host: pen.host
        anchors.fill: parent
        enabled: reader.open && !!pen.host && !!pen.host.content && !pen.inking
    }

    Key { command: "reader.ink"; when: pen.canInk; onActivated: pen.toggle() }
    Key { command: "reader.show_ink"; when: pen.canInk; onActivated: pen.toggleShown() }
    // Undo and redo are the editing keys everywhere (as copying is), not commands of their own.
    Shortcut {
        sequences: ["Ctrl+Z"]
        enabled: pen.canInk && pen.kept.canUndo && !win.dialogOpen
        onActivated: pen.kept.undo()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        enabled: pen.canInk && pen.kept.canRedo && !win.dialogOpen
        onActivated: pen.kept.redo()
    }

    // What the pen writes with, while it writes: the pen or the eraser,
    // whether a finger writes too, the ink's colour, a step back and on
    // again, and done.
    Rectangle {
        id: bar
        objectName: "inkBar"
        visible: pen.inking
        anchors.top: parent.top
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        // Its tools in a row where there is room, wrapped in a narrow window.
        width: Math.min(parent.width - 16, win.touch ? 1040 : 900)
        height: tools.implicitHeight + 12
        radius: theme.radius
        color: theme.background
        border.color: theme.colors.selection

        Flow {
            id: tools
            anchors.centerIn: parent
            width: bar.width - 12
            spacing: 4
            readonly property real side: win.touch ? 44 : 30

            AppButton {
                id: penButton
                objectName: "inkPen"
                text: qsTr("Pen")
                focusPolicy: Qt.NoFocus
                primary: reader.ink.tool === "pen"
                onClicked: reader.ink.tool = "pen"
            }
            AppButton {
                objectName: "inkHighlighter"
                text: qsTr("Highlighter")
                focusPolicy: Qt.NoFocus
                primary: reader.ink.tool === "highlighter"
                onClicked: reader.ink.tool = "highlighter"
            }
            AppButton {
                objectName: "inkEraser"
                text: qsTr("Eraser")
                focusPolicy: Qt.NoFocus
                primary: reader.ink.tool === "eraser"
                onClicked: reader.ink.tool = "eraser"
            }
            AppButton {
                objectName: "inkLasso"
                text: qsTr("Lasso")
                focusPolicy: Qt.NoFocus
                primary: reader.ink.tool === "lasso"
                onClicked: reader.ink.tool = "lasso"
            }
            AppButton {
                objectName: "inkFingers"
                readonly property string said: reader.ink.fingers === "on" ? qsTr("Write with a finger: on")
                                             : reader.ink.fingers === "off" ? qsTr("Write with a finger: off")
                                             : qsTr("Write with a finger: automatic")
                text: reader.ink.fingers === "on" ? qsTr("Finger: on") : reader.ink.fingers === "off" ? qsTr("Finger: off") : qsTr("Finger: automatic")
                focusPolicy: Qt.NoFocus
                Accessible.name: said
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: said
                onClicked: pen.cycleFingers()
            }
            Repeater {
                model: reader.ink.colors
                delegate: Item {
                    id: swatch
                    required property var modelData
                    readonly property bool chosen: reader.ink.color === modelData.key && !reader.ink.erasing && reader.ink.tool !== "lasso"
                    objectName: "ink_" + modelData.key
                    width: tools.side
                    // As tall as the buttons beside it, its disc in their middle.
                    height: Math.max(tools.side, penButton.height)
                    Accessible.role: Accessible.Button
                    Accessible.name: inkWords.name(modelData.key)
                    ToolTip.visible: swatchHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: inkWords.name(modelData.key)
                    Rectangle {
                        anchors.centerIn: parent
                        width: tools.side - 8
                        height: width
                        radius: width / 2
                        // As it is drawn: on a flowing book's page, in the theme's colours.
                        color: reader.kind === "flow" && reader.lineInk.penColor.valid ? reader.lineInk.shownColor(swatch.modelData.color) : swatch.modelData.color
                        border.width: swatch.chosen ? 3 : 1
                        border.color: swatch.chosen ? theme.foreground : theme.colors.selection
                    }
                    HoverHandler { id: swatchHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            reader.ink.color = swatch.modelData.key
                            // An ink is to write with: a highlighter stays one, anything else is the pen again.
                            if (reader.ink.tool !== "highlighter") reader.ink.tool = "pen"
                        }
                    }
                }
            }
            AppButton {
                objectName: "inkUndo"
                text: qsTr("Undo")
                focusPolicy: Qt.NoFocus
                enabled: pen.kept.canUndo
                onClicked: pen.kept.undo()
            }
            AppButton {
                objectName: "inkRedo"
                text: qsTr("Redo")
                focusPolicy: Qt.NoFocus
                enabled: pen.kept.canRedo
                onClicked: pen.kept.redo()
            }
            AppButton {
                objectName: "inkDone"
                runs: "reader.ink"
                text: qsTr("Done")
                focusPolicy: Qt.NoFocus
                onClicked: pen.toggle()
            }
        }
    }

    QtObject {
        id: inkWords
        function name(key) {
            return key === "blue" ? qsTr("Blue ink") : key === "red" ? qsTr("Red ink") : key === "green" ? qsTr("Green ink") : qsTr("Black ink")
        }
    }
}
