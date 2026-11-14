import QtQuick
import QtQuick.Controls

// A pane at the side of the shelf or the page, as wide as the person wants
// it: the details of a book, the contents, the AI, the map. Every one of
// them is put in one of these, so they all behave the same.
//
// Its inner edge is a handle. Dragging it, with a mouse or a finger, makes
// the pane wider or narrower as it goes; a double click or double tap puts
// it back to the width it comes with; `resize(steps)` is what the keys for
// wider and narrower call. The width chosen is kept in the settings under
// the pane's `name`, in pixels (see Settings::paneWidths for why pixels).
//
// The choice is held within bounds while it is shown, and never changed by
// them: `least` keeps the pane usable, and `room` is what its host can give
// it now, which is the width of the host less the `win.contentFloor` the
// shelf or the page keeps. When the window shrinks the pane narrows to
// `least`; when even that doesn't fit, the host sets `over` and the pane
// lies over the content instead of beside it, where the handle still
// resizes it. Grown again, the window gives the chosen width back.
Item {
    id: side
    // The pane's name in the settings: "details", "contents", "assistant".
    property string name
    // The width it comes with, the least it is of use at, and the most it
    // is ever given however wide the window is.
    property real standard: 320
    property real least: 240
    property real most: 720
    // The most its host has for it right now.
    property real room: most
    // On the side the text starts at (the contents), not the side it ends
    // at (everything else): which edge is the inner one.
    property bool leading: false
    // Lying over the content, not beside it.
    property bool over: false
    default property alias content: holder.data

    readonly property real chosen: settings.paneWidths[name] || standard
    // The width while it is being dragged; 0 when it isn't.
    property real dragged: 0
    readonly property bool resizing: drag.active
    // The widest and narrowest it can be right now. A window with less room
    // than `least` gives what it has.
    readonly property real ceiling: Math.max(0, Math.min(most, room))
    readonly property real floor: Math.min(least, ceiling)
    function fitted(wanted) { return Math.round(Math.max(floor, Math.min(wanted, ceiling))) }
    // How far one press of a key moves the edge.
    readonly property real step: 40

    // Its width, said as the width it wants: an anchored pane is then that
    // wide, and one in a layout is that wide unless the layout is told to
    // let it fill.
    implicitWidth: fitted(dragged > 0 ? dragged : chosen)
    // A pane that has the whole width to itself has no edge to drag.
    property bool fixed: false
    // The line along the inner edge, for a host that draws none of its own.
    property bool lined: true

    function resize(steps) { if (!fixed) settings.setPaneWidth(name, fitted(implicitWidth + steps * step)) }
    function reset() { settings.setPaneWidth(name, 0) }

    // Over the content it needs a ground of its own to be read on.
    Rectangle {
        anchors.fill: parent
        color: theme.background
        visible: side.over
    }
    Item {
        id: holder
        anchors.fill: parent
    }
    // The line between the pane and what it stands beside or over.
    Rectangle {
        anchors.left: side.leading ? undefined : parent.left
        anchors.right: side.leading ? parent.right : undefined
        width: 1
        height: parent.height
        color: theme.colors.selection
        visible: side.lined
    }

    // The handle: a strip astride the inner edge, wider than what it shows
    // so it is easy to catch, and as wide as a fingertip under fingers.
    Item {
        id: handle
        objectName: side.objectName === "" ? "paneHandle" : side.objectName + "Handle"
        anchors.horizontalCenter: side.leading ? parent.right : parent.left
        width: win.touch ? 36 : 12
        height: parent.height
        z: 10
        visible: !side.fixed
        Accessible.role: Accessible.Separator
        Accessible.name: qsTr("Drag to resize, double-click to reset")

        // What shows of it: nothing until a pointer is on it, then the edge
        // in the accent. Under fingers, which can't hover, a grip that is
        // always there.
        Rectangle {
            anchors.centerIn: parent
            width: 3
            height: parent.height
            color: theme.accent
            visible: hovering.hovered || drag.active
        }
        Rectangle {
            anchors.centerIn: parent
            visible: win.touch
            width: 5
            height: 44
            radius: 2.5
            color: drag.active ? theme.accent : theme.colors.muted
        }
        HoverHandler {
            id: hovering
            cursorShape: Qt.SizeHorCursor
        }
        DragHandler {
            id: drag
            target: null
            yAxis.enabled: false
            dragThreshold: 2
            property real from: 0
            // The edge moves under the pointer as the pane changes width,
            // so the distance is measured in the window, not in the handle.
            // Dragging away from the pane's outer edge widens it; which way
            // that is turns round with the side it is on and with a
            // right-to-left language.
            readonly property int outward: (side.leading !== win.rightToLeft) ? 1 : -1
            onActiveChanged: {
                if (active) {
                    from = side.implicitWidth
                } else if (side.dragged > 0) {
                    settings.setPaneWidth(side.name, side.fitted(side.dragged))
                    side.dragged = 0
                }
            }
            onCentroidChanged: {
                if (active)
                    side.dragged = Math.max(1, side.fitted(from + outward * (centroid.scenePosition.x - centroid.scenePressPosition.x)))
            }
        }
        TapHandler { onDoubleTapped: side.reset() }
    }
}
