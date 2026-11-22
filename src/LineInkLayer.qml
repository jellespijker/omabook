import QtQuick
import Omabook

// Free ink on a flowing book (#152, ReadingLineInk): an ink surface over a
// laid-out chapter (its TextEdit, `text`), reaching past the column into the
// page's side margins, as far as it is set. While the reader writes (the
// pen's bar, D) the pen, the mouse and, as chosen, a finger (#134) write and
// draw there, highlight, erase and lasso, between the lines and beside them;
// each stroke stays with the line beside it when the text is laid out again.
// Two fingers scroll the page. Nothing is written under what lies over the
// page (the ink's bar, `covering`; a note's card, a button), nor beyond the
// reading view (`view`). Under it, others' free ink in a space (#85,
// AnnotationLayerView `others`), each person's in their colour, never written on.
Item {
    id: layer
    property Item text
    property Item view
    property Item covering: null
    property var others: null
    // What of the page shows, where it is cut off (a page of a spread); none for all of it.
    property Item within: null

    InkSurface {
        id: theirs
        objectName: "othersLineInk"
        anchors.fill: parent
        visible: reader.ink.shown && count > 0
        readOnly: true
        enabled: false
        inkScale: 1
        function follow() { if (layer.text && layer.others) reader.lineInk.showTheirs(theirs, layer.text, layer.others) }
        Component.onCompleted: follow()
        Component.onDestruction: reader.lineInk.forget(theirs)
        Connections {
            target: layer
            function onTextChanged() { theirs.follow() }
            function onOthersChanged() { theirs.follow() }
        }
    }

    InkSurface {
        id: ink
        objectName: "lineInk"
        anchors.fill: parent
        visible: reader.ink.shown
        enabled: reader.ink.inking
        readOnly: !reader.ink.inking
        inkScale: 1
        fingers: reader.ink.fingerUse
        mouseDraws: reader.ink.inking
        batchPause: 600
        penWidth: reader.ink.highlighter ? 14 : 2.4
        pressure: !reader.ink.highlighter
        highlighter: reader.ink.highlighter
        eraserWidth: 28
        color: reader.lineInk.penColor
        tool: reader.ink.surfaceTool
        selection.color: theme.accent
        containmentMask: QtObject {
            function contains(point: point): bool {
                if (!layer.view) return true
                const at = ink.mapToItem(layer.view, point.x, point.y)
                if (at.x < 0 || at.y < 0 || at.x > layer.view.width || at.y > layer.view.height) return false
                const shown = layer.within ? ink.mapToItem(layer.within, point.x, point.y) : null
                if (shown && (shown.x < 0 || shown.y < 0 || shown.x > layer.within.width || shown.y > layer.within.height)) return false
                if (!layer.covering || !layer.covering.visible) return true
                const over = ink.mapToItem(layer.covering, point.x, point.y)
                return !(over.x >= 0 && over.y >= 0 && over.x <= layer.covering.width && over.y <= layer.covering.height)
            }
        }
        Component.onCompleted: if (layer.text) reader.lineInk.show(ink, layer.text)
        Component.onDestruction: reader.lineInk.forget(ink)
        Connections {
            target: layer
            function onTextChanged() { if (layer.text) reader.lineInk.show(ink, layer.text) }
        }
        onInked: (batch) => reader.lineInk.take(ink, batch)
        onPinched: (scale) => { if (layer.view && typeof layer.view.pinch === "function") layer.view.pinch(scale) }
    }
}
