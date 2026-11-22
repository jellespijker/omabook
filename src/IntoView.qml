import QtQuick

// Brings an item of a Flickable's into view once it has its size (#161):
// on the next turn of the event loop (restart() it), after laying `layout`
// out itself (ensurePolished), as on a busy computer that turn can come
// before the frame that would have, and the item be measured where it was
// before. A timer of its own owner's: what it would otherwise call back
// into can be gone.
Timer {
    property Flickable flick
    // What holds the item, laid out first.
    property Item layout
    // The item to show, asked for when it is time; null for none.
    property var target: () => null

    interval: 0
    onTriggered: {
        layout.ensurePolished()
        const item = target()
        if (!item) return
        const top = item.mapToItem(layout, 0, 0).y
        if (top < flick.contentY || top + item.height > flick.contentY + flick.height)
            flick.contentY = Math.max(0, Math.min(top, flick.contentHeight - flick.height))
    }
}
