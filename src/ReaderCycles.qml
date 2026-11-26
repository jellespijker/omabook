import QtQuick

// The reader's choices that a key goes round, each saying where it is now:
// the graph's marks in the text, the pages side by side, and the layout.
QtObject {
    // Shift+M: marks while the map is open, always, never, and round again.
    function marks() {
        const next = settings.marks === "map" ? "always" : settings.marks === "always" ? "never" : "map"
        settings.marks = next
        backend.say(next === "always" ? qsTr("The graph's things are marked in the text, always")
                  : next === "never" ? qsTr("Nothing is marked in the text")
                  : qsTr("The graph's things are marked in the text while the map is open"))
    }
    // Shift+P: as many pages as fit, one, two, three, and round again.
    function pages() {
        settings.pages = (settings.pages + 1) % 4
        const wanted = settings.pages
        backend.say(wanted === 0 ? qsTr("Automatic: %Ln pages here", "", reader.format.automaticColumns)
                  : reader.format.pagesHonoured ? qsTr("%Ln pages side by side", "", wanted)
                  : qsTr("Only %Ln pages fit at this width with lines of at least %1 characters", "", reader.format.columns)
                        .arg(reader.format.minimumMeasure.toLocaleString(Qt.locale(), 'f', 0)))
    }
    // Shift+L: pages, one scrolling column, automatic, and round again.
    function layout() {
        if (!reader.stillThere()) return
        const next = reader.format.nextLayout()
        backend.say(next === "pages" ? qsTr("Pages that turn, each chapter starting on a page of its own")
                  : next === "scroll" ? qsTr("One column that scrolls on through the chapters")
                  : qsTr("Automatic: pages where two fit side by side, else a column that scrolls"))
    }
}
