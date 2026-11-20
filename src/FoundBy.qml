import QtQuick
import QtQuick.Controls

// Why the search found a book by what is known of it, said briefly on its
// tile on the shelf: the thing and its kind, by the name the reader knows
// it by at their place in the book (BookListModel::knownFor), marked when it
// is of a graph someone shared. The details beside the shelf say it in full.
Item {
    id: found
    // What knownFor answered for the book; empty when nothing known found it.
    property var known: ({})
    property real fontSize: 12
    property color color: theme.accent
    readonly property bool shown: known !== undefined && known.node !== undefined
    readonly property bool shared: shown && !!known.from
    visible: shown
    implicitWidth: (mark.visible ? mark.width + 4 : 0) + label.implicitWidth
    implicitHeight: Math.max(label.implicitHeight, mark.visible ? mark.height : 0)
    Accessible.role: Accessible.StaticText
    Accessible.name: !shown ? ""
                     : shared ? qsTr("Found by what you know from %3: %1 (%2)").arg(known.name).arg(graphWords.kind(known.kind)).arg(known.from)
                     : qsTr("Found by what you know: %1 (%2)").arg(known.name).arg(graphWords.kind(known.kind))

    GraphWords { id: graphWords }

    Glyph {
        id: mark
        objectName: "foundByShared"
        visible: found.shared
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        name: "shared"
        size: Math.round(found.fontSize * 1.2)
        color: found.color
    }
    Label {
        id: label
        objectName: "foundByText"
        anchors.left: mark.visible ? mark.right : parent.left
        anchors.leftMargin: mark.visible ? 4 : 0
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        // "Mara Venn (Character)"
        text: found.shown ? qsTr("%1 (%2)").arg(found.known.name).arg(graphWords.kind(found.known.kind)) : ""
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.pixelSize: found.fontSize
        color: found.color
    }
}
