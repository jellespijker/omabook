import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A dialog in the window's own colors and corners, closed by Escape.
//
// It is never larger than the window. A dialog says how wide it would like
// to be (`widest`) and, if it holds a list that fills it, how tall (`tallest`);
// it gets that, or what the window has less `win.dialogEdge` on each side,
// whichever is smaller. With neither it is as large as what it holds, up to
// the same limit. What it holds goes in a DialogBody, which scrolls the
// content and keeps the buttons in view when the dialog is lower than its
// content: see DialogBody.qml.
Popup {
    // The size it would like, where what it holds doesn't say; 0 for that.
    property real widest: 0
    property real tallest: 0

    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: win.compact || win.low ? 16 : 24
    width: Math.min(widest > 0 ? widest : implicitWidth, win.width - 2 * win.dialogEdge)
    height: Math.min(tallest > 0 ? tallest : implicitHeight, win.height - 2 * win.dialogEdge)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    background: Rectangle {
        color: theme.background
        border.color: theme.colors.selection
        radius: theme.radius
    }
}
