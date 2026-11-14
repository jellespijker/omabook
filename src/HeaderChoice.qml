import QtQuick
import QtQuick.Controls

// One thing in a header that is clicked or tapped: in words while there is
// room for them, as a small drawing when there isn't. Either way it is as
// tall as a fingertip under fingers, and says what it is to a pointer that
// rests on it. The header decides which of the two it is (see LibraryView).
Item {
    id: choice
    property string text
    // The drawing that stands for the words, by its name in Glyph.qml.
    property string glyph
    // A second, small drawing after the words or the first: which way the
    // order runs, or that there is more underneath.
    property string mark
    // Whether the words show; the drawing alone otherwise.
    property bool worded: true
    // Told apart from the rest, as the one in use or the one that is on.
    property bool lit: false
    // The key that does the same, for the tip.
    property string keys
    // Names for the words and the mark, where something looks for them.
    property alias wordsName: label.objectName
    property alias markName: marked.objectName
    signal tapped()

    // What each form takes, so a header can tell whether the words fit
    // before it shows them.
    readonly property real side: win.touch ? 40 : 30
    readonly property real wordsWidth: Math.max(side, Math.ceil(label.implicitWidth) + (mark !== "" ? 14 : 0) + 12)
    readonly property real drawnWidth: side + (mark !== "" ? 12 : 0)

    implicitWidth: worded ? wordsWidth : drawnWidth
    implicitHeight: side
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.onPressAction: choice.tapped()

    Rectangle {
        anchors.fill: parent
        radius: theme.radius
        color: hover.hovered || choice.lit ? theme.colors.selection : "transparent"
        opacity: choice.lit && !hover.hovered ? 0.7 : 1
    }
    Row {
        anchors.centerIn: parent
        spacing: 2
        Label {
            id: label
            visible: choice.worded
            anchors.verticalCenter: parent.verticalCenter
            text: choice.text
            color: hover.hovered ? theme.foreground : theme.colors.muted
        }
        Glyph {
            visible: !choice.worded
            anchors.verticalCenter: parent.verticalCenter
            name: choice.glyph
            size: 18
            color: hover.hovered || choice.lit ? theme.foreground : theme.colors.muted
        }
        Glyph {
            id: marked
            visible: choice.mark !== ""
            anchors.verticalCenter: parent.verticalCenter
            name: choice.mark
            size: 12
            color: theme.colors.muted
        }
    }
    HoverHandler { id: hover }
    TapHandler { onTapped: choice.tapped() }
    // The words are their own tip; the drawing needs one.
    ToolTip.visible: hover.hovered && (!choice.worded || choice.keys !== "")
    ToolTip.delay: 600
    ToolTip.text: choice.keys !== "" ? choice.text + "  " + choice.keys : choice.text
}
