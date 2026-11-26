import QtQuick

// Fingers on the page: a tap on its right or left edge turns it, a tap in the
// middle shows the bar, a sideways swipe changes chapter, and a pinch makes the
// text or the page larger or smaller. The page itself scrolls under a finger as
// it always does. A tap on a marked thing shows its card; a long press selects
// the word under it and shows its handles and its menu, and the finger moved on
// without lifting grows the selection a word at a time (SelectHandles, #140).
Item {
    id: area
    objectName: "touchArea"
    // The reading view (ReaderView), and the handles of what a finger selects.
    property Item host
    property Item handles
    // A finger is on the page.
    readonly property bool fingerDown: finger.active
    // The page itself is how a pointer moves through it: a finger taps its
    // edges or swipes, a mouse wheel scrolls it or, on pages side by side,
    // turns them.
    readonly property string runs: "reader.page_down reader.page_up reader.line_down reader.line_up"

    function tappedAt(x, y) {
        if (host.highlighting.tapped(x, y)) return true
        const content = host.content
        if (!content || typeof content.thingAt !== "function") return false
        const at = area.mapToItem(content, x, y)
        const found = content.thingAt(at.x, at.y)
        if (!found) return false
        host.readingMenu.openCard(found, host.fromContent(found.rect), false, false)
        return true
    }
    // Something of the page's own that a finger taps at a point of an item
    // (a note's mark, a note in the margin): the tap is its, and the page
    // doesn't turn or show the bar as well (#141). They say so by `takesTaps`.
    function ownTap(item, x, y) {
        if (!item.visible || !item.enabled) return false
        if (item.takesTaps === true && x >= 0 && y >= 0 && x <= item.width && y <= item.height) return true
        for (const child of item.children) {
            const at = item.mapToItem(child, x, y)
            if (area.ownTap(child, at.x, at.y)) return true
        }
        return false
    }
    // A point of this item on a selection's handle, which is the handle's to take.
    function onHandle(point) {
        const at = area.mapToItem(handles, point.x, point.y)
        return handles.hit(at.x, at.y)
    }

    // The finger came down on something of the page's own. Its tap is that
    // thing's even where, by the time the finger lifts, the thing is gone
    // from under it: a note's Done closes the note it is on.
    property bool pressedOwn: false

    TapHandler {
        acceptedDevices: PointerDevice.TouchScreen
        // Each press says it afresh; the release that taps is read against it.
        onPressedChanged: {
            if (!pressed) return
            const content = area.host.content
            const on = content ? area.mapToItem(content, point.position.x, point.position.y) : null
            area.pressedOwn = !!content && area.ownTap(content, on.x, on.y)
        }
        onLongPressed: {
            if (reader.ink.touched() || reader.lineInk.touched() || area.onHandle(point.position)) return
            const at = area.mapToItem(area.handles, point.position.x, point.position.y)
            area.handles.pressAt(at.x, at.y, true)
        }
        onTapped: (point) => {
            win.touchSeen = true
            const content = area.host.content
            if (!content || reader.ink.touched(point.timestamp) || reader.lineInk.touched(point.timestamp) || area.onHandle(point.position)) return
            if (area.host.readingMenu.shown) {
                area.host.readingMenu.close()
                return
            }
            // Words a finger selected are let go by a tap beside them.
            if (area.handles.showing) {
                content.clearSelection()
                return
            }
            const on = area.mapToItem(content, point.position.x, point.position.y)
            if (area.pressedOwn || area.ownTap(content, on.x, on.y) || area.tappedAt(point.position.x, point.position.y)) return
            // A note's card beside its words, tapped beside, closes (#167).
            if (content.notes && content.notes.closeCard()) return
            // What the page has to tap on itself: the line at a chapter's end.
            if (typeof content.activateAt === "function" && content.activateAt(on.x, on.y)) return
            const x = win.pagesRightToLeft ? area.width - point.position.x : point.position.x
            if (x < area.width * 0.25) area.host.turn(false)
            else if (x > area.width * 0.75) area.host.turn(true)
            else area.host.showBar = !area.host.showBar
        }
    }
    DragHandler {
        id: swipe
        objectName: "pageSwipe"
        acceptedDevices: PointerDevice.TouchScreen
        target: null
        yAxis.enabled: false
        // A zoomed page pans sideways instead, and a finger moving a selection turns nothing.
        enabled: !!area.host.content && area.host.content.contentWidth <= area.host.content.width + 1 && !area.handles.holding
        property real travelled: 0
        // How far the finger is from where it came down. Measured
        // from the finger itself, so a fast flick that arrives as
        // one jump counts as much as a slow drag.
        function track() {
            if (area.host.turningNow) return
            const moved = centroid.position.x - centroid.pressPosition.x
            if (moved === 0) return
            travelled = moved
            area.host.slide = moved
        }
        onCentroidChanged: if (active) track()
        onActiveChanged: {
            if (active) {
                win.touchSeen = true
                track()
                return
            }
            // A third of the way across, or a decent flick, turns the page.
            const far = Math.abs(travelled) > Math.min(120, area.width / 3)
            if (far && area.host.content) area.host.turn(travelled * area.host.forwardSign > 0)
            else area.host.slideBack()
            travelled = 0
        }
    }
    // A finger on the page: while it is down, the text doesn't select under
    // it, so it scrolls the page instead (a press and hold still selects a
    // word); after a long press, where it goes the selection grows to.
    PointHandler {
        id: finger
        acceptedDevices: PointerDevice.TouchScreen
        onPointChanged: {
            if (!active || area.handles.moving !== "word") return
            const at = area.mapToItem(area.handles, point.position.x, point.position.y)
            area.handles.follow(at.x, at.y)
        }
        onActiveChanged: {
            if (active || area.handles.moving !== "word") return
            // Where it lifted is the last of where it went.
            const at = area.mapToItem(area.handles, point.position.x, point.position.y)
            area.handles.follow(at.x, at.y)
            area.handles.letGo()
        }
    }
    PinchHandler {
        acceptedDevices: PointerDevice.TouchScreen
        target: null
        onScaleChanged: (delta) => { if (area.host.content) area.host.content.pinch(delta) }
    }
    // Pages that turn don't scroll, so the wheel turns them: a
    // notch of a mouse wheel, or as much from a touchpad.
    WheelHandler {
        objectName: "pageWheel"
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        enabled: !!area.host.content && (area.host.content.objectName === "spreadReader" || area.host.content.turnsPages === true)
        property real gathered: 0
        onWheel: (event) => {
            gathered += event.angleDelta.y
            if (Math.abs(gathered) < 120) return
            area.host.turn(gathered < 0)
            gathered = 0
        }
    }
    // A column that runs on: the wheel scrolls it as always, and
    // turned on two notches past a chapter's end (or back past
    // its start), it goes on into the next chapter (or back to
    // the end of the one before); one notch is only resistance. The column below
    // still takes every turn of the wheel.
    WheelHandler {
        objectName: "runOnWheel"
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        enabled: !!area.host.content && area.host.content.objectName === "flowReader" && area.host.content.runsOn
        blocking: false
        property real gathered: 0
        onWheel: (event) => {
            const content = area.host.content
            const down = event.angleDelta.y < 0
            const there = down ? content.atEnd() : content.atStart()
            if (!there || (gathered !== 0 && (gathered < 0) !== down)) {
                gathered = 0
                if (!there) return
            }
            gathered += event.angleDelta.y
            if (Math.abs(gathered) < 240) return
            gathered = 0
            if (down) content.next()
            else content.backToEnd()
        }
    }
}
