import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The computers linked to a hub (#189), as it says them: each by its name,
// its key and when it was linked; this computer marked, and each of the
// others to take off the hub (its key there taken away), after asking. Up
// and Down move through them; Delete asks to take the one there off.
ColumnLayout {
    id: page
    // The hub, as HubDesk::hubs has it; null when there is none.
    property var hub: null
    property string forgetting: ""
    property bool busy: false
    property int at: 0
    readonly property var computers: hub && hub.computers ? hub.computers : []
    signal forgetWanted(string name)
    signal forgetLeft()
    signal forgetSure()
    spacing: 6

    // A key of the panel's while this page shows; whether it was taken.
    function key(event) {
        const plain = event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier
        const computer = at >= 0 && at < computers.length ? computers[at] : null
        if (event.key === Qt.Key_Down || (event.key === Qt.Key_J && plain))
            at = Math.min(computers.length - 1, at + 1)
        else if (event.key === Qt.Key_Up || (event.key === Qt.Key_K && plain))
            at = Math.max(0, at - 1)
        else if ((event.key === Qt.Key_Delete || (event.key === Qt.Key_D && plain)) && computer && !computer["this"] && !busy)
            forgetWanted(computer.name)
        else
            return false
        return true
    }
    onVisibleChanged: at = 0

    Label {
        objectName: "hubComputersNone"
        visible: page.computers.length === 0
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        text: page.hub && page.hub.state !== "reachable" ? qsTr("The hub can't be asked which computers it has now.")
                                                         : qsTr("No computers are linked to the hub.")
    }
    Repeater {
        model: page.computers
        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            readonly property bool mine: modelData["this"] === true
            readonly property bool asking: page.forgetting === modelData.name
            Layout.fillWidth: true
            implicitHeight: parts.implicitHeight + 12
            radius: Math.min(4, theme.radius)
            color: index === page.at ? theme.colors.selection : "transparent"

            GridLayout {
                id: parts
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10
                anchors.rightMargin: 6
                columns: win.compact ? 1 : 2
                columnSpacing: 10
                rowSpacing: 4
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        objectName: "hubComputer"
                        Layout.fillWidth: true
                        text: row.mine ? qsTr("%1 (this computer)").arg(row.modelData.name) : row.modelData.name
                        font.bold: row.mine
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Linked %1 · %2").arg(new Date(row.modelData.linked).toLocaleDateString(Qt.locale(), Locale.ShortFormat))
                                                    .arg(row.modelData.fingerprint)
                        color: theme.colors.muted
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                    Label {
                        objectName: "hubForgetQuestion"
                        visible: row.asking
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        color: theme.colors.orange
                        text: qsTr("Take %1 off the hub? Its key there is taken away, and it syncs with the hub no more until it is linked again.").arg(row.modelData.name)
                    }
                }
                Row {
                    Layout.alignment: Qt.AlignTop
                    spacing: 6
                    z: 1
                    AppButton {
                        objectName: "hubForget"
                        visible: !row.mine && !row.asking
                        enabled: !page.busy
                        text: qsTr("Forget…")
                        onClicked: {
                            page.at = row.index
                            page.forgetWanted(row.modelData.name)
                        }
                    }
                    AppButton {
                        objectName: "hubForgetYes"
                        visible: row.asking
                        enabled: !page.busy
                        text: qsTr("Forget")
                        onClicked: page.forgetSure()
                    }
                    AppButton {
                        objectName: "hubForgetNo"
                        visible: row.asking
                        text: qsTr("Keep")
                        onClicked: page.forgetLeft()
                    }
                }
            }
            TapHandler { onTapped: page.at = row.index }
        }
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
        text: qsTr("To take this computer off, unlink it in the list of hubs.")
    }
}
