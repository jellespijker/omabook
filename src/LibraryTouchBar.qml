import QtQuick
import QtQuick.Layouts

// What the keys do, as buttons in the library's header, where fingers are the
// keyboard. All of them where there is room. Where there isn't, they give way
// one at a time (#268): Filter and Settings first, to the drawings the header
// has for them, then Libraries, Suggest and Add, which the ⋯ lists.
Row {
    id: bar
    // Room for every one of them in words.
    property bool all: true
    // Where there isn't: how many of Add, Suggest and Libraries show still.
    property int kept: 0
    signal filterWanted()
    spacing: 8
    // The header lays out by this at once: the row's own width follows a frame
    // later, and what stands after it would be pushed out of the window meanwhile.
    Layout.preferredWidth: all ? wholeWidth : keptWidth(kept)

    // What they take, all of them or so many kept, shown or not.
    readonly property real wholeWidth: widthOf([addButton, filterButton, suggestButton, librariesButton, settingsButton])
    function keptWidth(count) { return widthOf([addButton, suggestButton, librariesButton].slice(0, count)) }
    function widthOf(buttons) {
        return buttons.reduce((sum, button) => sum + Math.ceil(button.implicitWidth), 0) + Math.max(0, buttons.length - 1) * spacing
    }

    AppButton { id: addButton; objectName: "touchAdd"; visible: bar.all || bar.kept >= 1; runs: "library.add"; text: qsTr("Add"); focusPolicy: Qt.NoFocus; onClicked: backend.addBooks() }
    AppButton { id: filterButton; objectName: "touchFilter"; visible: bar.all; runs: "library.filter library.filter_clear"; text: qsTr("Filter"); focusPolicy: Qt.NoFocus; onClicked: bar.filterWanted() }
    AppButton { id: suggestButton; objectName: "touchSuggest"; visible: bar.all || bar.kept >= 2; runs: "library.suggest"; text: qsTr("Suggest"); focusPolicy: Qt.NoFocus; onClicked: win.showSuggestions() }
    AppButton { id: librariesButton; objectName: "touchLibraries"; visible: bar.all || bar.kept >= 3; runs: "library.libraries"; text: qsTr("Libraries"); focusPolicy: Qt.NoFocus; onClicked: win.showLibraries() }
    AppButton { id: settingsButton; objectName: "touchSettings"; visible: bar.all; runs: "global.settings"; text: qsTr("Settings"); focusPolicy: Qt.NoFocus; onClicked: win.showSettings() }
}
