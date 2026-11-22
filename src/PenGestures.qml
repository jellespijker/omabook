import QtQuick
import Omabook

// The pen over the page (#81): a line under or through words, a loop round
// them or a bar beside lines in the margin highlights them in the colour in
// use; a scribble over a highlight takes it away; a tap of the pen on one
// opens its menu, as a finger's does. The stroke shows while it is made and
// goes once the pen pauses and it is acted on (Ink::gestureOf); a palm on the
// screen while the pen is near is ignored (Ink::InputRules). Fingers and the
// mouse pass through to the page, which they scroll, turn and select as ever.
//
// A stroke that means none of these is writing: a margin note by hand on the
// line under it, or beside it in the margin column (#90, ReadingNotes). Over
// a handwritten note's ink the pen is the note's, and writes on it: the ink
// surface made last hears the pen first (InkSurface's filter of the
// application's events), and a note's is always made after this one.
InkSurface {
    id: pen
    objectName: "penGestures"
    // The reading view: its page (`content`) and its menu.
    property Item host
    fingers: InkSurface.Scroll
    mouseDraws: false
    batchPause: 150
    penWidth: 2.5
    color: {
        const chosen = reader.highlights.colors.find(one => one.key === reader.highlights.current)
        return chosen ? chosen.line : theme.accent
    }

    HighlightWords { id: words }
    // Set once made: as the root of its file the surface's type is seen at
    // its own revision, which has no containmentMask to bind.
    Component.onCompleted: pen.containmentMask = mask
    // A control of the reading view's that is the pen's to press (a note's
    // microphone, #148), at a point of `item`: the pen is then not this surface's.
    function takesPen(item, x, y) {
        if (!item.visible || !item.enabled) return false
        if (item.takesPen === true && x >= 0 && y >= 0 && x <= item.width && y <= item.height) return true
        for (const child of item.children) {
            const at = item.mapToItem(child, x, y)
            if (pen.takesPen(child, at.x, at.y)) return true
        }
        return false
    }
    QtObject {
        id: mask
        function contains(point: point): bool {
            const handles = pen.host ? pen.host.selectionHandles : null
            const at = handles ? pen.mapToItem(handles, point.x, point.y) : point
            if (point.x < 0 || point.y < 0 || point.x > pen.width || point.y > pen.height || (handles && handles.hit(at.x, at.y)))
                return false
            const view = pen.host
            const there = view ? pen.mapToItem(view, point.x, point.y) : point
            return !(view && pen.takesPen(view, there.x, there.y))
        }
    }

    // When the pen last came down and went up, for how long it was held.
    property real downAt: 0
    property real upAt: 0
    onWritingChanged: {
        if (writing) downAt = Date.now()
        else upAt = Date.now()
    }
    // How long the pen stayed where it came down, in milliseconds, and the
    // point it left from there by (-1 for none): a stroke's points come only
    // as it moves, so one that never left is as long as the pen was down.
    function stillFor(points) {
        for (let i = 1; i < points.length; ++i)
            if (Math.hypot(points[i].x - points[0].x, points[i].y - points[0].y) > 8) return { ms: points[i].ms, left: i }
        return { ms: upAt - downAt, left: -1 }
    }

    function length(points) {
        let along = 0
        for (let i = 1; i < points.length; ++i) along += Math.hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y)
        return along
    }
    // The notes (ReadingNotes) of the page, when it has any.
    readonly property var notes: host && host.content && host.content.notes ? host.content.notes : null

    // What one stroke did, said in the window; false when it meant nothing.
    function act(points) {
        const content = host.content
        if (!content || points.length === 0) return true
        const still = stillFor(points)
        if (still.ms >= Application.styleHints.mousePressAndHoldInterval) {
            const handles = host.selectionHandles
            const at = pen.mapToItem(handles, points[0].x, points[0].y), last = points[points.length - 1]
            handles.penHeld(at, still.left >= 0 ? pen.mapToItem(handles, last.x, last.y) : null)
            return true
        }
        if (points.length < 3 || length(points) < 6) {
            host.highlighting.tapped(points[0].x, points[0].y)
            return true
        }
        if (typeof content.penStroke !== "function") return true
        const done = content.penStroke(points.map(p => pen.mapToItem(content, p.x, p.y)))
        if (done.kind === "scribble" && done.removed > 0)
            backend.say(qsTr("%Ln highlights removed", "", done.removed))
        else if (done.id > 0)
            backend.say(qsTr("Highlighted in %1").arg(words.name(reader.highlights.current)))
        return done.kind !== ""
    }
    onInked: {
        const snapshot = pen.snapshot()
        const strokes = reader.highlights.takeStrokes(pen)
        const writing = []
        strokes.forEach((points, index) => { if (!act(points)) writing.push(index) })
        if (writing.length > 0 && notes) {
            const first = strokes[writing[0]][0]
            notes.penWrote(snapshot, writing, pen.mapToItem(host.content, first.x, first.y))
        }
    }
}
