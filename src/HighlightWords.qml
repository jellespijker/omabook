import QtQuick

// What the highlight colours are called (#81): the name the reader gave each
// ("Important"), else the colour itself, in the window's language.
QtObject {
    function color(key) {
        return key === "blue" ? qsTr("Blue") : key === "green" ? qsTr("Green") : key === "orange" ? qsTr("Orange")
             : key === "purple" ? qsTr("Purple") : qsTr("Yellow")
    }
    function name(key) {
        translator.language
        const given = settings.highlightNames[key]
        return given ? given : color(key)
    }
    // Every colour's name, by its key, for an export.
    function names() {
        const all = {}
        for (const one of reader.highlights.colors) all[one.key] = name(one.key)
        return all
    }
}
