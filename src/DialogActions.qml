import QtQuick
import QtQuick.Layouts

// A dialog's buttons: in a row at the end of the dialog while they fit
// there, and wrapped onto more rows when a narrow window or a language of
// longer words means they don't, so a caption is never cut and a button is
// never pushed out of the dialog.
Item {
    id: actions
    default property alias content: flow.data
    Layout.fillWidth: true
    implicitHeight: flow.height
    // What it would like: its buttons in one row.
    implicitWidth: Math.ceil(flow.oneRow) + 1

    Flow {
        id: flow
        // What the buttons that show take side by side.
        readonly property real oneRow: {
            let across = 0
            for (let i = 0; i < children.length; ++i)
                if (children[i].visible && children[i].implicitWidth > 0)
                    across += children[i].implicitWidth + spacing
            return Math.max(0, across - spacing)
        }
        // At the end of the row, which mirroring makes the left in a
        // right-to-left language, and no wider than the dialog.
        anchors.right: parent.right
        width: Math.min(Math.ceil(oneRow) + 1, parent.width)
        spacing: 8
    }
}
