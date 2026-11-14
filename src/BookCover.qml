import QtQuick
import QtQuick.Controls

// A book on the shelf: its cover, or its title on a plain tile when it has
// none, with how far it has been read along the bottom edge. A book that is
// not a file says so in a corner, and one that is finished or waiting to be
// read carries a small mark.
Item {
    id: tile
    property url cover
    property string title
    property string authors
    property real percent: 0
    property bool current: false
    // "to-read", "reading", "read" or nothing; "paper", "both", "none" (a
    // wish), "digital" or nothing.
    property string status
    property string held
    readonly property bool wish: held === "none"

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Math.min(4, theme.radius)
        color: theme.colors.selection
        clip: true

        Label {
            visible: picture.status !== Image.Ready
            anchors.fill: parent
            anchors.margins: Math.round(tile.width / 12)
            anchors.bottomMargin: Math.round(tile.width / 4)
            text: tile.title
            font.pixelSize: Math.round(Math.max(12, Math.min(24, tile.width / 10)))
            font.bold: true
            wrapMode: Text.Wrap
            elide: Text.ElideRight
            maximumLineCount: 6
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        Label {
            visible: picture.status !== Image.Ready
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Math.round(tile.width / 12)
            text: tile.authors
            // Muted would be too faint on the tile, which is the selection color.
            opacity: 0.7
            font.pixelSize: Math.round(Math.max(11, Math.min(16, tile.width / 13)))
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
        }

        Image {
            id: picture
            anchors.fill: parent
            source: tile.cover
            // Decoded at the size it is shown, off the GUI thread: a shelf of
            // hundreds of covers scrolls without a hitch.
            sourceSize.width: tile.width > 220 ? 720 : 360
            asynchronous: true
            fillMode: Image.PreserveAspectCrop
        }

        Rectangle {
            visible: tile.percent > 0 && tile.status !== "read"
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            height: 3
            width: parent.width * Math.min(1, tile.percent / 100)
            color: theme.accent
        }
    }

    // A wish is not on the shelf yet: it is drawn fainter, inside a line.
    Rectangle {
        visible: tile.wish
        anchors.fill: frame
        radius: frame.radius
        color: Qt.alpha(theme.background, 0.45)
        border.color: theme.colors.muted
        border.width: 1
    }

    Rectangle {
        objectName: "heldBadge"
        visible: badge.text !== "" && tile.width >= 72
        anchors.left: frame.left
        anchors.top: frame.top
        anchors.margins: 6
        width: badge.implicitWidth + 12
        height: badge.implicitHeight + 4
        radius: Math.min(4, theme.radius)
        color: theme.background
        border.color: theme.colors.selection
        Label {
            id: badge
            anchors.centerIn: parent
            text: tile.wish ? qsTr("Wishlist") : tile.held === "paper" || tile.held === "both" ? qsTr("Paper") : ""
            font.pixelSize: 11
        }
    }

    // Finished is a check; meaning to read it is a ribbon.
    Rectangle {
        objectName: "statusMark"
        visible: (tile.status === "read" || tile.status === "to-read") && tile.width >= 72
        anchors.right: frame.right
        anchors.top: frame.top
        anchors.margins: 6
        width: 20
        height: 20
        radius: 10
        color: tile.status === "read" ? theme.accent : theme.background
        border.color: tile.status === "read" ? theme.accent : theme.colors.selection
        Glyph {
            anchors.centerIn: parent
            name: tile.status === "read" ? "check" : "ribbon"
            size: tile.status === "read" ? 14 : 12
            color: tile.status === "read" ? theme.accentForeground : theme.foreground
        }
    }

    Rectangle {
        visible: tile.current
        anchors.fill: frame
        anchors.margins: -4
        radius: frame.radius + 4
        color: "transparent"
        border.color: theme.accent
        border.width: 2
    }
}
