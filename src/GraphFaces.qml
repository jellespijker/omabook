import QtQuick
import QtQuick.Controls

// The two faces of the graph in its header, for a pointer and a finger:
// the picture and the list, the one shown lit (GraphPane's `listing`).
Row {
    id: faces
    property bool listing: false
    // The key that switches between them, for the tip.
    property string keys
    signal chosen(bool list)
    spacing: 2

    Repeater {
        model: ["graph", "list"]
        delegate: Rectangle {
            id: face
            required property string modelData
            readonly property bool current: (modelData === "list") === faces.listing
            objectName: "graphAs-" + modelData
            readonly property string runs: "graph.list"
            width: win.touch ? 40 : 30
            height: win.touch ? 40 : 30
            radius: theme.radius
            color: current ? theme.colors.selection : "transparent"
            Glyph {
                anchors.centerIn: parent
                name: face.modelData
                size: 18
                color: face.current || faceHover.hovered ? theme.foreground : theme.colors.muted
            }
            HoverHandler { id: faceHover }
            TapHandler { onTapped: faces.chosen(face.modelData === "list") }
            ToolTip.visible: faceHover.hovered
            ToolTip.delay: 600
            ToolTip.text: (face.modelData === "list" ? qsTr("List") : qsTr("Picture")) + "  " + faces.keys
        }
    }
}
