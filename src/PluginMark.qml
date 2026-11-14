import QtQuick

// How a plugin is, as one quiet mark: a filled dot for on, a ring for off,
// a warning sign for one that needs something, a cross for one that is
// broken. The shape says it as well as the colour.
Item {
    id: mark
    // "on", "off", "needs" or "broken"; not `state`, which every item has.
    property string condition: "on"
    property real size: 16
    width: size
    height: size

    Rectangle {
        visible: mark.condition === "on" || mark.condition === "off"
        anchors.centerIn: parent
        width: Math.round(mark.size * 0.56)
        height: width
        radius: width / 2
        color: mark.condition === "on" ? theme.accent : "transparent"
        border.width: mark.condition === "on" ? 0 : 1.5
        border.color: Qt.alpha(theme.foreground, 0.5)
    }
    Glyph {
        visible: mark.condition === "needs"
        name: "alert"
        size: mark.size
        color: theme.colors.orange
    }
    Glyph {
        visible: mark.condition === "broken"
        name: "cross"
        size: mark.size
        color: theme.colors.red
    }
}
