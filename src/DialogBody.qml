import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What an AppPopup holds, laid out so it fits any window: the content in a
// column that scrolls when the dialog is lower than it, and under it the
// buttons, which stay where they are and wrap rather than run off. A dialog
// written as
//
//     AppPopup {
//         widest: 560
//         DialogBody {
//             Label { ... }
//             TextField { ... }
//             actions: [ AppButton { ... }, AppButton { ... } ]
//         }
//     }
//
// is never larger than the window, and nothing in it is out of reach. The
// field that takes the keyboard is scrolled into view.
ColumnLayout {
    id: body
    default property alias content: column.data
    property alias actions: buttons.content
    // The space between the things in the column.
    property alias gap: column.spacing
    readonly property alias scroller: flick
    // How much of the column's edge the scroll bar covers while it scrolls:
    // a control there (at the end of a row) stays clear of it by this much.
    readonly property real barRoom: flick.interactive ? bar.width : 0

    anchors.fill: parent
    spacing: 16
    // A popup is not an Item and doesn't inherit the window's mirroring.
    LayoutMirroring.enabled: win.rightToLeft
    LayoutMirroring.childrenInherit: true

    Flickable {
        id: flick
        objectName: "dialogScroller"
        Layout.fillWidth: true
        Layout.fillHeight: true
        implicitWidth: column.implicitWidth
        // A little above the column: a Material field's label stands
        // astride its top edge, and would be cut by the clip. It is part of
        // what scrolls, so a column a few pixels too tall for the dialog
        // says so by its height and scrolls, rather than being cut.
        readonly property real above: 8
        implicitHeight: column.implicitHeight + above
        contentWidth: width
        contentHeight: column.implicitHeight + above
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height + 1
        // Only while it scrolls: unseen, it would still take the presses at the column's edge.
        ScrollBar.vertical: ScrollBar { id: bar; visible: flick.interactive }

        ColumnLayout {
            id: column
            y: flick.above
            // The full width whether it scrolls or not: a column that made
            // room for a scroll bar would wrap its text differently with
            // one than without, and so never settle on either.
            width: flick.width
            spacing: 12
        }

        // Tab moves the keyboard to a field that may be scrolled out of
        // sight; it is brought back into it.
        function reveal(item) {
            if (!item || !interactive)
                return
            for (let up = item; up; up = up.parent) {
                if (up !== column)
                    continue
                const top = item.mapToItem(column, 0, 0).y + column.y
                if (top < contentY)
                    contentY = Math.max(0, top - 8)
                else if (top + item.height > contentY + height)
                    contentY = Math.min(contentHeight - height, top + item.height - height + 8)
                return
            }
        }
        Connections {
            target: win
            function onActiveFocusItemChanged() { flick.reveal(win.activeFocusItem) }
        }
    }
    DialogActions { id: buttons }
}
