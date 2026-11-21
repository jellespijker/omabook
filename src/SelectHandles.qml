import QtQuick

// The two handles at the ends of words a finger or the pen selected (#140). A
// long press selects the word under it and shows them; dragging either grows
// or shrinks the selection a whole word at a time, across lines, and held at
// the page's edge on through the page, which scrolls or turns. A long press
// moved on without lifting grows it the same way (ReaderTouch), and so does
// the pen held still and then moved (PenGestures). Each is drawn in the accent
// the words are selected in, with a fingertip's room to take it by, inside the
// page; in a right-to-left text they change sides with it. The words' menu goes
// while an end moves and comes back beside them, clear of the handles.
//
// The page (FlowReader, SpreadReader, PageReader) says where the ends are
// (`selectionEnds`), holds one end while the other moves (`holdSelection`),
// moves it to the word at a point (`extendSelection`) and goes on at its edge
// (`edgeStep`, and `edgeOf` where its edge is not its foot and head).
Item {
    id: handles
    objectName: "selectHandles"
    // The reading view: its page (`content`), and the menu of what is selected.
    property Item host
    readonly property Item content: host ? host.content : null
    // The selection was made by a finger or the pen, which is what shows them.
    property bool shown: false
    // What is moving: "start" or "end", a handle; "word", the finger of a
    // long press still down; "" for nothing.
    property string moving: ""
    // A finger or the pen on a handle, before it moves.
    readonly property bool pressed: startHandle.pressed || endHandle.pressed
    // While either, the page holds still under it and a swipe turns nothing.
    readonly property bool holding: moving !== "" || pressed
    // Where the ends are, in this item: {start, end, rtl}, an end null where
    // it is out of sight; null for no words selected.
    property var ends: null
    readonly property bool showing: shown && !!ends && !reader.ink.inking
    // The room a fingertip takes, and the knob drawn in its middle.
    readonly property real reach: 44
    readonly property real knob: 18
    // Where the moving end is taken to (the finger, and from it to the words
    // it aims at), where it was taken from, and whether it has moved since.
    property point last: Qt.point(0, 0)
    property point offset: Qt.point(0, 0)
    property point from: Qt.point(0, 0)
    property bool moved: false
    // How tall the line of the end being moved is.
    property real aimHeight: 20
    visible: showing

    function place() {
        const found = shown && content && typeof content.selectionEnds === "function" ? content.selectionEnds() : null
        if (!found) {
            ends = null
            return
        }
        const inSight = (r) => {
            if (!r) return null
            const at = handles.mapFromItem(handles.content, r.x, r.y)
            const fits = at.x >= -1 && at.x <= width + 1 && at.y + r.height >= 0 && at.y <= height
            return fits ? Qt.rect(at.x, at.y, 0, r.height) : null
        }
        ends = { start: inSight(found.start), end: inSight(found.end), rtl: !!found.rtl }
    }
    // A long press of a finger (`down` while it is still there), or the pen
    // held still, at a point of this item: the word there selected and its
    // handles shown, and its menu. Whether there was one.
    function pressAt(x, y, down) {
        if (!content || typeof content.selectAt !== "function") return false
        const at = mapToItem(content, x, y)
        if (!content.selectAt(at.x, at.y)) return false
        shown = true
        if (down && typeof content.holdSelection === "function" && content.holdSelection("word")) {
            from = Qt.point(x, y)
            last = from
            offset = Qt.point(0, 0)
            moved = false
            moving = "word"
        }
        place()
        host.released()
        return true
    }
    // The pen held still where it came down, and perhaps moved on after to `to`.
    function penHeld(at, to) {
        if (!pressAt(at.x, at.y, !!to) || !to) return
        follow(to.x, to.y)
        letGo()
    }
    // A handle taken at a point: the other end holds, and this one goes
    // where the finger aims, which is the words at its end, not the knob under them.
    function grab(which, x, y) {
        const caret = ends ? (which === "start" ? ends.start : ends.end) : null
        if (!caret || !content.holdSelection(which)) return
        const inward = (which === "start") !== ends.rtl ? 2 : -2
        offset = Qt.point(caret.x + inward - x, caret.y + caret.height / 2 - y)
        aimHeight = caret.height
        from = Qt.point(x, y)
        last = from
        moved = false
        moving = which
    }
    function follow(x, y) {
        if (moving === "") return
        last = Qt.point(x, y)
        if (!moved && Math.abs(x - from.x) + Math.abs(y - from.y) < 8) return
        if (!moved) host.readingMenu.close()
        moved = true
        extend()
    }
    // The end moved to the words aimed at; and the finger itself at the
    // page's edge (its foot or head, or where the page says) goes on.
    function extend() {
        const at = mapToItem(content, last.x + offset.x, last.y + offset.y)
        content.extendSelection(at.x, at.y)
        place()
        const finger = mapToItem(content, last.x, last.y)
        const going = typeof content.edgeOf === "function" ? content.edgeOf(finger.x, finger.y)
                    : finger.y > content.height - edge.room ? 1 : finger.y < edge.room ? -1 : 0
        if (going === 0) edge.waiting = false
        edge.going = going
        if (going !== 0 && !edge.waiting && !edge.running) edge.start()
        else if (going === 0) edge.stop()
    }
    function letGo() {
        const was = moved
        moving = ""
        moved = false
        edge.stop()
        edge.waiting = false
        place()
        if (was) host.released()
    }
    // Whether a point of this item is on a handle.
    function hit(x, y) {
        if (!showing) return false
        return [startHandle, endHandle].some(one => one.visible && x >= one.x && x <= one.x + one.width && y >= one.y && y <= one.y + one.height)
    }
    // A rectangle around the words, made deep enough to keep the menu off
    // the knobs, under the words or, at the page's foot, over them.
    function clearOf(rect) { return showing ? Qt.rect(rect.x, rect.y - knob - 6, rect.width, rect.height + (knob + 6) * 2) : rect }

    // At the page's edge the end goes on: a column scrolls while it stays
    // there; pages turn once a moment, and again only once it left the edge
    // and came back, the selection waiting for the finger to go on to the
    // word it wants on the new page.
    Timer {
        id: edge
        property int going: 0
        property bool waiting: false
        readonly property bool turns: !!handles.content && handles.content.objectName === "spreadReader"
        readonly property real room: handles.content ? Math.min(40, handles.content.height / 8) : 40
        interval: turns ? 600 : 40
        repeat: !turns
        onTriggered: {
            const content = handles.content
            if (!content || handles.moving === "" || typeof content.edgeStep !== "function" || !content.edgeStep(going)) {
                stop()
                return
            }
            if (turns) {
                waiting = true
                handles.place()
            } else {
                handles.extend()
            }
        }
    }

    onShownChanged: place()
    onWidthChanged: place()
    onHeightChanged: place()
    Connections {
        target: handles.content
        ignoreUnknownSignals: true
        function onContentYChanged() { handles.place() }
        function onContentXChanged() { handles.place() }
        function onPageChanged() { handles.place() }
        function onSelectedTextChanged() {
            if (handles.content.selectedText === "" && handles.moving === "") handles.shown = false
            handles.place()
        }
    }

    component Handle: Item {
        id: handle
        // Which end it is: "start" or "end".
        property string which
        readonly property bool held: handles.moving === which
        readonly property bool pressed: tap.pressed
        // The line at its end; while it is moved, where the finger takes it.
        readonly property var caret: handles.ends ? (which === "start" ? handles.ends.start : handles.ends.end) : null
        readonly property var line: held ? Qt.rect(handles.last.x + handles.offset.x, handles.last.y + handles.offset.y - handles.aimHeight / 2, 0, handles.aimHeight)
                                         : caret
        // Away from the words: before the start, after the end, the other way round in a right-to-left text.
        readonly property real outward: (which === "start") !== (!!handles.ends && handles.ends.rtl) ? -1 : 1
        // The knob's middle: under the line, or over it where the page ends below it.
        readonly property real knobX: line ? line.x + outward * handles.knob * 0.4 : 0
        readonly property real knobY: !line ? 0 : line.y + line.height + handles.knob / 2 + 1 <= handles.height - handles.knob / 2
                                                ? line.y + line.height + handles.knob / 2 + 1 : line.y - handles.knob / 2 - 1
        visible: !!line
        width: handles.reach
        height: handles.reach
        x: Math.max(0, Math.min(handles.width - width, knobX - width / 2))
        y: Math.max(0, Math.min(handles.height - height, knobY - height / 2))

        Rectangle {
            objectName: handle.objectName + "Stem"
            x: (handle.line ? handle.line.x : 0) - handle.x - 1
            y: (handle.line ? handle.line.y : 0) - handle.y
            width: 2
            height: handle.line ? handle.line.height : 0
            color: theme.accent
        }
        Rectangle {
            objectName: handle.objectName + "Knob"
            x: handle.knobX - handle.x - width / 2
            y: handle.knobY - handle.y - height / 2
            width: handles.knob
            height: handles.knob
            radius: width / 2
            color: theme.accent
            border.color: theme.background
            border.width: 2
        }
        TapHandler {
            id: tap
            acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus | PointerDevice.Mouse | PointerDevice.TouchPad
        }
        DragHandler {
            target: null
            acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus | PointerDevice.Mouse | PointerDevice.TouchPad
            onActiveChanged: {
                if (active) {
                    const at = handles.mapFromItem(null, centroid.scenePressPosition.x, centroid.scenePressPosition.y)
                    handles.grab(handle.which, at.x, at.y)
                    const now = handles.mapFromItem(null, centroid.scenePosition.x, centroid.scenePosition.y)
                    handles.follow(now.x, now.y)
                } else if (handle.held) {
                    handles.letGo()
                }
            }
            onCentroidChanged: {
                if (!active || !handle.held) return
                const at = handles.mapFromItem(null, centroid.scenePosition.x, centroid.scenePosition.y)
                handles.follow(at.x, at.y)
            }
        }
    }

    Handle { id: startHandle; objectName: "selectStart"; which: "start" }
    Handle { id: endHandle; objectName: "selectEnd"; which: "end" }
}
