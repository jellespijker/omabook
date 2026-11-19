import QtQuick
import QtQuick.Controls

// A quiet word over the top of the graph's picture, centred on it: what the
// graph holds so far, or that the picture is cut and how to reach the rest.
Label {
    // The picture it is over, a sibling to be centred on.
    property Item picture

    anchors.horizontalCenter: picture.horizontalCenter
    anchors.topMargin: 4
    width: Math.min(picture.width - 32, 560)
    horizontalAlignment: Text.AlignHCenter
    wrapMode: Text.Wrap
    // No more of it than a small picture has room for over itself.
    maximumLineCount: Math.max(1, Math.min(4, Math.floor((picture.height - 8) / 18)))
    elide: Text.ElideRight
    color: theme.colors.muted
}
