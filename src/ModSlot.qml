import QtQuick
import QtQuick.Layouts

// A place in the window for panels of the user's own: every mod in
// ~/.config/omabook/mods that names this place, side by side.
RowLayout {
    id: slot
    property string place
    readonly property var here: custom.mods.filter(mod => mod.place === slot.place)
    visible: here.length > 0
    spacing: 0

    Repeater {
        model: slot.here
        delegate: Item {
            id: panel
            required property var modelData
            Layout.preferredWidth: modelData.width
            Layout.fillHeight: true
            clip: true

            Rectangle {
                width: 1
                height: parent.height
                color: theme.colors.selection
            }
            Loader {
                objectName: "mod-" + panel.modelData.name
                anchors.fill: parent
                anchors.leftMargin: 1
                source: panel.modelData.url
            }
        }
    }
}
