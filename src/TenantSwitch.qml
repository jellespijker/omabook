import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// In a header of the graph or the chronology: the top-level collection
// (or Unfiled) whose part of the graph is shown, which a click or a tap
// opens the list of the others from (TenantChoice). Only where there is
// another to switch to.
Rectangle {
    id: switcher
    // Whose commands it is the pointer's way to: "graph" or "chronology".
    property string place: "graph"
    // The key that lists them, for the tip.
    property string keys
    readonly property string name: graphTenants.shown.unfiled ? qsTr("Unfiled") : (graphTenants.shown.name || "")
    readonly property string runs: place + ".tenant " + place + ".tenant_next " + place + ".tenant_previous"
    signal tapped()

    visible: graphTenants.switchable
    implicitWidth: row.implicitWidth + 16
    implicitHeight: win.touch ? 40 : 30
    Layout.preferredWidth: implicitWidth
    Layout.preferredHeight: implicitHeight
    Layout.minimumWidth: Math.min(implicitWidth, 64)
    radius: theme.radius
    color: hover.hovered ? theme.colors.selection : "transparent"
    border.width: 1
    border.color: theme.colors.selection
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Collection: %1").arg(name)
    Accessible.onPressAction: switcher.tapped()

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 4
        Label {
            objectName: switcher.place + "TenantName"
            Layout.fillWidth: true
            text: switcher.name
            font.italic: !!graphTenants.shown.unfiled
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
        Glyph {
            name: "down"
            size: 12
            color: theme.colors.muted
        }
    }
    HoverHandler { id: hover }
    TapHandler { onTapped: switcher.tapped() }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 600
    ToolTip.text: qsTr("Show another collection") + (switcher.keys !== "" ? "  " + switcher.keys : "")
}
