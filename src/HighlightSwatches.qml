import QtQuick
import QtQuick.Controls

// The five highlight colours in a row, to choose one (#81): each a disc of
// its wash on the page with its line round it, the one in use ringed, and
// its name for a screen reader and a tooltip. Under fingers each is a
// fingertip across. Keys 1 to 5 choose them where this is offered.
Row {
    id: swatches
    objectName: "highlightSwatches"
    // The colour shown as chosen: the highlight's own, or the one in use.
    property string chosen: reader.highlights.current
    signal picked(string color)
    readonly property real side: win.touch ? 44 : 30
    spacing: win.touch ? 6 : 4

    HighlightWords { id: words }

    Repeater {
        model: reader.highlights.colors
        delegate: Item {
            id: swatch
            required property var modelData
            required property int index
            objectName: "swatch_" + modelData.key
            width: swatches.side
            height: swatches.side
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Highlight in %1").arg(words.name(modelData.key))
            ToolTip.visible: hover.hovered
            ToolTip.delay: 500
            ToolTip.text: "%1 · %2".arg(index + 1).arg(words.name(modelData.key))
            Rectangle {
                anchors.centerIn: parent
                width: swatches.side - 6
                height: width
                radius: width / 2
                color: swatch.modelData.tint
                border.width: swatches.chosen === swatch.modelData.key ? 3 : 1.5
                border.color: swatches.chosen === swatch.modelData.key ? theme.foreground : swatch.modelData.line
            }
            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: swatches.picked(swatch.modelData.key) }
        }
    }
}
