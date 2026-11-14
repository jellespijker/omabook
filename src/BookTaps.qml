import QtQuick

// How a book answers a pointer, the same in every view: a click selects and
// a double click opens; a finger opens with one tap, and selects by holding.
Item {
    id: taps
    anchors.fill: parent
    // Whether a finger's tap opens the book, or only picks it, where a view
    // brings a book forward before it can be opened.
    property bool tapOpens: true
    signal picked()
    signal opened()
    signal held()

    TapHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
        onTapped: taps.picked()
        onDoubleTapped: taps.opened()
    }
    TapHandler {
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
            taps.picked()
            taps.held()
        }
    }
}
