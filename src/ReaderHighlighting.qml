import QtQuick

// What the reading view does with highlights (#81): the selected words
// highlighted in a colour (the menu's colours, 1 to 5 while selecting), and
// a highlight at a point of the page, which a finger's or the pen's tap
// opens the menu of.
QtObject {
    property Item view
    property Item menu
    property Item touchArea
    property HighlightWords words: HighlightWords {}

    function highlight(color) {
        if (view.selecting) view.stopSelecting(false, true)
        menu.close()
        const content = view.content
        if (content && typeof content.highlightSelection === "function" && content.highlightSelection(color) > 0)
            backend.say(qsTr("Highlighted in %1").arg(words.name(color)))
    }
    // Its menu, for the highlight at a point of the touch area; whether there was one.
    function tapped(x, y) {
        const content = view.content
        if (!content || typeof content.highlightAtPoint !== "function") return false
        const at = touchArea.mapToItem(content, x, y)
        const found = content.highlightAtPoint(at.x, at.y)
        if (found) menu.openHighlight(found, view.fromContent(found.rect))
        return !!found
    }
}
