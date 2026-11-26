import QtQuick
import QtQuick.Controls

// config.json can't be read: said at the top until it is fixed. A click puts
// the notice aside; it comes back when what is wrong changes.
Rectangle {
    id: notice
    objectName: "settingsProblem"

    readonly property string problem: { translator.language; return settings.problem }
    property string putAside: ""

    visible: problem !== "" && problem !== putAside
    width: Math.min(parent.width - 2 * win.dialogEdge, words.implicitWidth + 40)
    height: words.height + 24
    radius: theme.radius
    color: theme.colors.selection
    border.color: theme.colors.red

    TapHandler { onTapped: notice.putAside = notice.problem }

    Label {
        id: words
        objectName: "settingsProblemText"
        anchors.centerIn: parent
        width: Math.min(implicitWidth, notice.parent.width - 2 * win.dialogEdge - 40)
        text: notice.problem
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
    }
}
