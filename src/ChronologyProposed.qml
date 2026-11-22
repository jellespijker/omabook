import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omabook

// The branches the AI proposed (#171): an alternate history, or a world that
// goes another way than one it follows, read in a book, with what it
// branches off and at which event, to take as a lane of its own beside that
// timeline or to refuse. Until then its events are in their book's own order,
// never on what it branches off. Only once the reader read the split. One at
// a time, over the lanes, on one line; where the lanes have no room for it
// (`room` false) it waits for a larger window.
Rectangle {
    id: strip
    property var browser
    property bool room: true
    readonly property var first: proposals.list.length > 0 ? proposals.list[0] : null
    visible: first !== null && room
    implicitHeight: row.implicitHeight + 12
    color: theme.background

    ChronologyProposals {
        id: proposals
        browser: strip.browser
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: theme.colors.selection
    }

    RowLayout {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 8
        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: strip.first ? qsTr("The AI reads a branch: %1, off %2 at %3.").arg(strip.first.name).arg(strip.first.from)
                                .arg(strip.first.at) : ""
            elide: Text.ElideRight
            maximumLineCount: 1
            textFormat: Text.PlainText
            ToolTip.text: text
            ToolTip.visible: tip.hovered && truncated
            HoverHandler { id: tip }
        }
        AppButton {
            objectName: "chronologyAccept"
            primary: true
            text: qsTr("Take it")
            focusPolicy: Qt.NoFocus
            onClicked: proposals.decide(strip.first.timeline, true)
        }
        AppButton {
            objectName: "chronologyRefuse"
            text: qsTr("Not a branch")
            focusPolicy: Qt.NoFocus
            onClicked: proposals.decide(strip.first.timeline, false)
        }
    }
}
