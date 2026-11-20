import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What the chronology says when it has nothing to draw: that nothing is
// known yet of when things happen, or that the filters leave nothing.
Item {
    id: empty
    property var browser
    readonly property bool filtered: browser.eventCount > 0

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 460)
        spacing: 10
        Label {
            Layout.fillWidth: true
            text: empty.filtered ? qsTr("Nothing matches the filters") : qsTr("Nothing is known yet of when things happen")
            font.pixelSize: 17
            font.bold: true
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
        Label {
            Layout.fillWidth: true
            visible: !empty.filtered
            text: qsTr("As the AI notes what you read, it notes when the events happen too, and how they follow each other. They show here as far as you have read.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
        AppButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: parent.width
            visible: empty.filtered
            runs: "chronology.filter_clear"
            text: qsTr("Show everything again")
            focusPolicy: Qt.NoFocus
            onClicked: empty.browser.clearFilters()
        }
    }
}
