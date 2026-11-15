import QtQuick
import QtQuick.Shapes

// The app's few icons, drawn as lines on a 24 by 24 grid in one weight, so
// none depends on a font having the right character.
Item {
    id: glyph
    property string name
    property real size: 16
    property color color: theme.foreground
    // Filled, where the shape is a mark rather than a drawing.
    property bool solid: false
    readonly property var paths: ({
        "check": "M5 12.5 L10 17.5 L19 7",
        "cross": "M7 7 L17 17 M17 7 L7 17",
        "plus": "M12 5 V19 M5 12 H19",
        "down": "M6 9 L12 15 L18 9",
        "up": "M6 15 L12 9 L18 15",
        "back": "M15 5 L8 12 L15 19",
        "forward": "M9 5 L16 12 L9 19",
        "ribbon": "M7 3 H17 V21 L12 16.5 L7 21 Z",
        "star": "M12 2.5 L14.8 8.9 L21.8 9.6 L16.5 14.3 L18.1 21.1 L12 17.5 L5.9 21.1 L7.5 14.3 L2.2 9.6 L9.2 8.9 Z",
        "covers": "M4 4 H10.5 V10.5 H4 Z M13.5 4 H20 V10.5 H13.5 Z M4 13.5 H10.5 V20 H4 Z M13.5 13.5 H20 V20 H13.5 Z",
        "spines": "M4.5 4 H8 V20 H4.5 Z M10.5 7.5 H14.5 V20 H10.5 Z M17 5 H19.5 V20 H17 Z",
        "carousel": "M8 4.5 H16 V19.5 H8 Z M3 8 H5 V16 H3 Z M19 8 H21 V16 H19 Z",
        "list": "M4 6.5 H20 M4 12 H20 M4 17.5 H20",
        "stacks": "M4 16.5 H20 V20 H4 Z M6 11.5 H19 V15 H6 Z M5 6.5 H18 V10 H5 Z",
        "graph": "M8.3 7.2 L15.7 7.8 M7 9.2 L10 15.9 M16.7 10.1 L12.5 16.1 M3.7 7 a2.3 2.3 0 1 0 4.6 0 a2.3 2.3 0 1 0 -4.6 0 M15.7 8 a2.3 2.3 0 1 0 4.6 0 a2.3 2.3 0 1 0 -4.6 0 M8.7 18 a2.3 2.3 0 1 0 4.6 0 a2.3 2.3 0 1 0 -4.6 0",
        "filter": "M4 6 H20 L14 13 V19 L10 17 V13 Z",
        "sort": "M5 7 H19 M5 12 H15 M5 17 H11",
        "search": "M4.5 10.5 a6 6 0 1 0 12 0 a6 6 0 1 0 -12 0 M15 15 L20 20",
        "more": "M4 12 a1.3 1.3 0 1 0 2.6 0 a1.3 1.3 0 1 0 -2.6 0 M10.7 12 a1.3 1.3 0 1 0 2.6 0 a1.3 1.3 0 1 0 -2.6 0 M17.4 12 a1.3 1.3 0 1 0 2.6 0 a1.3 1.3 0 1 0 -2.6 0",
        "alert": "M12 4.5 L21 19.5 H3 Z M12 10.5 V14.5 M12 17 V17.1",
        "alignLeft": "M4 6 H20 M4 10 H14 M4 14 H20 M4 18 H12",
        "alignRight": "M4 6 H20 M10 10 H20 M4 14 H20 M12 18 H20",
        "alignCenter": "M4 6 H20 M7 10 H17 M4 14 H20 M8 18 H16",
        "alignJustify": "M4 6 H20 M4 10 H20 M4 14 H20 M4 18 H13"
    })
    width: size
    height: size

    // The drawing exists only while it shows: a curve-rendered shape under
    // something hidden still leaves a trace of itself on screen.
    Loader {
        active: glyph.visible
        sourceComponent: Shape {
            width: glyph.size
            height: glyph.size
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                scale: Qt.size(glyph.size / 24, glyph.size / 24)
                strokeColor: glyph.solid ? "transparent" : glyph.color
                strokeWidth: Math.max(1.2, glyph.size / 11)
                fillColor: glyph.solid ? glyph.color : "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: glyph.paths[glyph.name] || "" }
            }
        }
    }
}
