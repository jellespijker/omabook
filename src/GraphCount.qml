import QtQuick
import QtQuick.Controls

// How much of the graph is shown, at the top of its pane: "38 things · 52
// relations", "12 of 38" when the filters leave some out, and for a picture
// cut at a limit how many there are in all: "2,000 of 6,965 things · 8,685
// of 17,192 relations" (GraphBrowser::cut).
Label {
    id: count
    property var browser
    property bool compact: false

    function number(n) { return n.toLocaleString(Qt.locale(), 'f', 0) }

    text: {
        translator.language
        const graph = count.browser
        if (!graph || graph.knownCount === 0) return ""
        if (graph.cut !== "")
            return qsTr("%1 of %Ln things", "", graph.wholeCount).arg(count.number(graph.nodeCount))
                   + (count.compact ? "" : " · " + qsTr("%1 of %Ln relations", "", graph.wholeEdges).arg(count.number(graph.edgeCount)))
        if (graph.nodeCount < graph.knownCount)
            return qsTr("%1 of %2").arg(count.number(graph.nodeCount)).arg(count.number(graph.knownCount))
        return qsTr("%Ln things", "", graph.nodeCount) + (count.compact ? "" : " · " + qsTr("%Ln relations", "", graph.edgeCount))
    }
    color: theme.colors.muted
    elide: Text.ElideRight
    // Whole or not at all: a count cut to "4…" says nothing,
    // and the kinds under it carry the same numbers.
    opacity: width >= implicitWidth - 1 ? 1 : 0
}
