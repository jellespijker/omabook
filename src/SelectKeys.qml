import QtQuick
import QtQuick.Controls

// Words selected with the keyboard in the reading view (V, or Shift+N for a
// margin note), or a part of a comic's page (#115): the keys that move and
// widen the selection, and a line
// saying how, while it lasts. Enter is done: the menu of what to do with the
// words, or the note on them; 1 to 5 highlight them in that colour (#81);
// Escape (or V) stops.
Item {
    id: keys
    // The reading view: its page (`content`), and what ends the selecting.
    property Item view
    // What the words are being selected for: "menu", or "note".
    property string purpose: "menu"
    function take() { selectKeys.forceActiveFocus() }

    Item {
        id: selectKeys
        objectName: "selectKeys"
        focus: false
        Keys.onPressed: (event) => {
            const view = keys.view
            const wide = (event.modifiers & Qt.ShiftModifier) !== 0
            const back = win.rightToLeft ? Qt.Key_Right : Qt.Key_Left
            const on = win.rightToLeft ? Qt.Key_Left : Qt.Key_Right
            if (event.key === back || event.key === Qt.Key_H) view.content.stepSelecting(-1, 0, wide)
            else if (event.key === on || event.key === Qt.Key_L) view.content.stepSelecting(1, 0, wide)
            else if (event.key === Qt.Key_Down || event.key === Qt.Key_J) view.content.stepSelecting(0, 1, wide)
            else if (event.key === Qt.Key_Up || event.key === Qt.Key_K) view.content.stepSelecting(0, -1, wide)
            else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) view.stopSelecting(true)
            else if (event.key === Qt.Key_Escape || event.key === Qt.Key_V) view.stopSelecting(false)
            else if (event.key === Qt.Key_C && (event.modifiers & Qt.ControlModifier)) view.content.copySelection()
            else if (event.key >= Qt.Key_1 && event.key <= Qt.Key_5 && keys.purpose === "menu")
                view.highlighting.highlight(reader.highlights.colors[event.key - Qt.Key_1].key)
            else return
            event.accepted = true
        }
        onActiveFocusChanged: if (!activeFocus && keys.view.selecting) keys.view.stopSelecting(false)
    }
    // Says how, while it lasts.
    Rectangle {
        objectName: "selectHint"
        visible: keys.view.selecting
        z: 6
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: Math.min(hint.implicitWidth + 24, parent.width - 32)
        height: hint.implicitHeight + 14
        radius: theme.radius
        color: theme.background
        border.color: theme.colors.selection
        Label {
            id: hint
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - 24)
            // A part of a comic's page (#115) is moved and sized, not widened by words.
            readonly property bool part: !!keys.view.content && !!keys.view.content.choosingPart
            text: part ? (keys.purpose === "note"
                          ? qsTr("H J K L or the arrows move the part of the page; Shift makes it larger or smaller. Enter writes a margin note on it, Esc stops.")
                          : qsTr("H J K L or the arrows move the part of the page; Shift makes it larger or smaller. 1 to 5 highlight it, Enter shows what else to do with it, Esc stops."))
                  : keys.purpose === "note"
                  ? qsTr("H L or the arrows move a word, J K a line; Shift widens. Enter writes a margin note on it, Esc stops.")
                  : qsTr("H L or the arrows move a word, J K a line; Shift widens. 1 to 5 highlight it, Enter shows what else to do with it, Esc stops.")
            color: theme.colors.muted
            font.pixelSize: 12
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
