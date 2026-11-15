import QtQuick
import QtQuick.Controls

// The ways of showing the library, as a row of small drawings: the one in
// use stands out, and a click or a tap on another switches to it.
Row {
    id: switcher
    readonly property var views: ["covers", "spines", "carousel", "list", "stacks"]
    // Every way is a tap away, which is what "the next way" is for a pointer.
    readonly property string runs: "library.view_next"
    spacing: 2

    Words { id: words }

    Repeater {
        model: switcher.views
        delegate: Rectangle {
            id: choice
            required property string modelData
            readonly property bool current: backend.view === modelData
            objectName: "view-" + modelData
            readonly property string runs: "library.view_" + modelData
            width: win.touch ? 40 : 30
            height: win.touch ? 40 : 30
            radius: theme.radius
            color: current ? theme.colors.selection : "transparent"

            Glyph {
                anchors.centerIn: parent
                name: choice.modelData
                size: 18
                color: choice.current || hover.hovered ? theme.foreground : theme.colors.muted
            }
            HoverHandler { id: hover }
            TapHandler { onTapped: backend.view = choice.modelData }
            ToolTip.visible: hover.hovered
            ToolTip.delay: 600
            // "Spines  2"
            ToolTip.text: words.view(modelData) + "  " + (custom.keyText["library.view_" + modelData] || "")
        }
    }
    // The library as a graph of what is known is one more way to look at
    // it, though not a shelf: it takes the shelf's place until it is closed.
    Rectangle {
        objectName: "view-graph"
        readonly property string runs: "library.graph"
        width: win.touch ? 40 : 30
        height: win.touch ? 40 : 30
        radius: theme.radius
        color: "transparent"
        Glyph {
            anchors.centerIn: parent
            name: "graph"
            size: 18
            color: graphHover.hovered ? theme.foreground : theme.colors.muted
        }
        HoverHandler { id: graphHover }
        TapHandler { onTapped: win.showGraph(0) }
        ToolTip.visible: graphHover.hovered
        ToolTip.delay: 600
        ToolTip.text: qsTr("Graph") + "  " + (custom.keyText["library.graph"] || "")
    }
}
