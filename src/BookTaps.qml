import QtQuick

// How a book answers a pointer, the same in every view: a click selects and
// a double click opens; a right click selects it and lists what can be done
// with it; a finger opens with one tap, and holding does what a right click
// does. Its details are one of what is listed.
Item {
    id: taps
    anchors.fill: parent
    // A click or a tap on a book is how a pointer moves the selection.
    readonly property string runs: "library.left library.right library.up library.down"
    // Whether a finger's tap opens the book, or only picks it, where a view
    // brings a book forward before it can be opened.
    property bool tapOpens: true
    signal picked()
    signal opened()

    // Selects the book and lists what can be done with it, beside where it was pressed.
    function list(at) {
        taps.picked()
        win.showMenu("book", taps.mapToItem(null, at.x, at.y))
    }

    TapHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
        onTapped: taps.picked()
        onDoubleTapped: taps.opened()
    }
    TapHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
        acceptedButtons: Qt.RightButton
        onTapped: (point) => taps.list(point.position)
    }
    TapHandler {
        id: finger
        acceptedDevices: PointerDevice.TouchScreen
        onTapped: {
            win.touchSeen = true
            // Asked before the pick, which may be what makes it the one that opens.
            const opens = taps.tapOpens
            taps.picked()
            if (opens)
                taps.opened()
        }
        onLongPressed: {
            win.touchSeen = true
            taps.list(finger.point.position)
        }
    }
}
