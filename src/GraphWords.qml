import QtQuick

// What the graph calls things: the kinds of node, one and many, and the
// graphs there are, in one place so the picture, the list and the details
// say them the same way.
QtObject {
    function kind(kind) {
        return kind === "character" ? qsTr("Character") : kind === "group" ? qsTr("Group") : kind === "place" ? qsTr("Place")
             : kind === "item" ? qsTr("Item") : kind === "concept" ? qsTr("Concept") : kind === "event" ? qsTr("Event")
             : kind === "thread" ? qsTr("Plot thread") : kind === "theme" ? qsTr("Theme") : kind === "topic" ? qsTr("Topic")
             : kind === "question" ? qsTr("Question") : kind === "book" ? qsTr("Book") : kind === "author" ? qsTr("Author")
             : kind === "series" ? qsTr("Series") : kind === "tag" ? qsTr("Tag")
             : kind.charAt(0).toUpperCase() + kind.slice(1)
    }
    function kinds(kind) {
        return kind === "character" ? qsTr("Characters") : kind === "group" ? qsTr("Groups") : kind === "place" ? qsTr("Places")
             : kind === "item" ? qsTr("Items") : kind === "concept" ? qsTr("Concepts") : kind === "event" ? qsTr("Events")
             : kind === "thread" ? qsTr("Plot threads") : kind === "theme" ? qsTr("Themes") : kind === "topic" ? qsTr("Topics")
             : kind === "question" ? qsTr("Questions") : kind === "book" ? qsTr("Books") : kind === "author" ? qsTr("Authors")
             : kind === "series" ? qsTr("Series") : kind === "tag" ? qsTr("Tags")
             : kind.charAt(0).toUpperCase() + kind.slice(1)
    }
    // A graph by its scope: the general one, or a world of its own.
    function scope(scope) {
        return scope === "" ? qsTr("Everything read")
             : scope.startsWith("world:") ? qsTr("World %1").arg(scope.slice(6)) : scope
    }
    // How far around the centre is shown.
    function reach(depth) { return depth <= 0 ? qsTr("Everything") : qsTr("%Ln steps around", "", depth) }
}
