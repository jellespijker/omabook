import QtQuick
import QtQuick.Controls

// Over a picture cut at a limit (#64): how what is shown was picked and how
// to reach the rest; of the connections between books, a button that shows
// more of them, those between the most connected things first (#91).
Item {
    id: note
    // The picture it is over, a sibling, and the browser it tells of.
    property Item picture
    property var browser

    anchors.left: picture.left
    anchors.right: picture.right
    height: more.visible ? more.y + more.height : words.height

    GraphNote {
        id: words
        objectName: "graphCut"
        picture: note
        anchors.top: parent.top
        // No more of it than the picture under it has room for.
        maximumLineCount: Math.max(1, Math.min(4, Math.floor((note.picture.height - 8) / 18)))
        text: {
            const graph = note.browser
            if (graph.cut === "nearest")
                return qsTr("The %Ln nearest things are shown. Centre on one of them to see further.", "", graph.knownCount)
            if (graph.cut !== "across")
                return qsTr("The %Ln most connected things are shown. Find a thing, centre on one, or pick a book under Filter to reach the rest.", "", graph.knownCount)
            return graph.moreAcross
                   ? qsTr("The %Ln connections between the most connected things are shown. Show more of them, or pick a book under Filter to reach the rest.", "", graph.acrossCount)
                   : qsTr("The %Ln connections between the most connected things are shown. Pick a book under Filter to reach the rest.", "", graph.acrossCount)
        }
    }
    // The picture's colour under the button, so its caption reads over the nodes.
    Rectangle {
        anchors.fill: more
        visible: more.visible
        radius: theme.radius
        color: theme.background
    }
    AppButton {
        id: more
        objectName: "graphMoreAcross"
        visible: note.browser.moreAcross
        anchors.top: words.bottom
        anchors.topMargin: 6
        anchors.horizontalCenter: parent.horizontalCenter
        focusPolicy: Qt.NoFocus
        text: qsTr("Show More Connections")
        onClicked: note.browser.showMoreAcross()
    }
}
