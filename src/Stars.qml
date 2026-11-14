import QtQuick

// A rating as stars: as many as were given, and the rest of the five faint
// where the row should keep its width.
Row {
    id: stars
    property int rating: 0
    property real size: 12
    property color color: theme.foreground
    property bool showEmpty: false
    visible: rating > 0
    spacing: Math.round(size * 0.12)

    Repeater {
        model: stars.showEmpty ? 5 : stars.rating
        delegate: Glyph {
            required property int index
            name: "star"
            solid: true
            size: stars.size
            color: index < stars.rating ? stars.color : Qt.alpha(stars.color, 0.22)
        }
    }
}
