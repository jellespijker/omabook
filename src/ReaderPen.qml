import QtQuick
import QtQuick.Controls

// The pen over the page. Over words, its gestures highlight them (#81,
// PenGestures). On the pages of a PDF or a comic, while the reader writes on
// them ({reader.ink}), it writes and draws book ink on the page instead
// (#83, ReadingInk, an ink surface over each page in PageReader), its other
// end erasing; this bar above the page then says what it writes with. The
// ink is put away and shown again for reading ({reader.show_ink}), and undone
// and done again (Ctrl+Z, Ctrl+Shift+Z) a pause of the pen at a time, on
// whichever page it was.
Item {
    id: pen
    objectName: "readerPen"
    // The reading view: its page (`content`) and its menu.
    property Item host
    readonly property bool canInk: reader.open && reader.kind === "pages"
    readonly property bool inking: canInk && reader.ink.inking
    // What lies over the pages, where the pen writes on it and not on them.
    readonly property Item covering: bar

    function toggle() {
        reader.ink.inking = !reader.ink.inking
        backend.say(reader.ink.inking ? qsTr("Writing on the pages: the pen's other end erases")
                                      : qsTr("Reading the pages"))
    }
    function noPages() {
        backend.say(qsTr("Ink is written on the pages of a PDF or a comic; select words here to highlight them or write a note."))
    }
    function toggleShown() {
        reader.ink.shown = !reader.ink.shown
        backend.say(reader.ink.shown ? qsTr("Ink shown") : qsTr("Ink put away"))
    }

    PenGestures {
        host: pen.host
        anchors.fill: parent
        enabled: reader.open && !!pen.host && !!pen.host.content && !pen.inking
    }

    Key { command: "reader.ink"; when: reader.open; onActivated: pen.canInk ? pen.toggle() : pen.noPages() }
    Key { command: "reader.show_ink"; when: reader.open; onActivated: pen.canInk ? pen.toggleShown() : pen.noPages() }
    // Undo and redo are the editing keys everywhere (as copying is), not commands of their own.
    Shortcut {
        sequences: ["Ctrl+Z"]
        enabled: pen.canInk && reader.ink.canUndo && !win.dialogOpen
        onActivated: reader.ink.undo()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        enabled: pen.canInk && reader.ink.canRedo && !win.dialogOpen
        onActivated: reader.ink.redo()
    }

    // What the pen writes with, while it writes: the pen or the eraser, the
    // ink's colour, a step back and on again, and done.
    Rectangle {
        id: bar
        objectName: "inkBar"
        visible: pen.inking
        anchors.top: parent.top
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        // Its tools in a row where there is room, wrapped in a narrow window.
        width: Math.min(parent.width - 16, win.touch ? 680 : 600)
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
                primary: !reader.ink.erasing
                onClicked: reader.ink.erasing = false
            }
            AppButton {
                objectName: "inkEraser"
                text: qsTr("Eraser")
                focusPolicy: Qt.NoFocus
                primary: reader.ink.erasing
                onClicked: reader.ink.erasing = true
            }
            Repeater {
                model: reader.ink.colors
                delegate: Item {
                    id: swatch
                    required property var modelData
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
                        color: swatch.modelData.color
                        border.width: reader.ink.color === swatch.modelData.key && !reader.ink.erasing ? 3 : 1
                        border.color: reader.ink.color === swatch.modelData.key && !reader.ink.erasing ? theme.foreground : theme.colors.selection
                    }
                    HoverHandler { id: swatchHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            reader.ink.color = swatch.modelData.key
                            reader.ink.erasing = false
                        }
                    }
                }
            }
            AppButton {
                objectName: "inkUndo"
                text: qsTr("Undo")
                focusPolicy: Qt.NoFocus
                enabled: reader.ink.canUndo
                onClicked: reader.ink.undo()
            }
            AppButton {
                objectName: "inkRedo"
                text: qsTr("Redo")
                focusPolicy: Qt.NoFocus
                enabled: reader.ink.canRedo
                onClicked: reader.ink.redo()
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
