import QtQuick
import Omabook

// One kind of node as it is drawn in the picture: a disc in the kind's
// color from the theme with its small drawing inside.
GraphMark {
    property real size: 16
    width: size
    height: size
    colors: theme.colors
    foreground: theme.foreground
    background: theme.background
}
