import QtQuick

// Asking the AI about the library as a whole (W), or for what to read next,
// the whole library's or the shown collection's only.
AskDialog {
    objectName: "askLibrary"
    voice: true
    heading: qsTr("Ask about your library")
    placeholder: qsTr("Which of my books…?")
    reply: librarian.answer
    working: librarian.answering
    offer: qsTr("Suggest what to read next")
    onAsked: (words) => librarian.ask(words)
    onOffered: librarian.suggest()
    onlyName: backend.collection !== 0 ? backend.collectionName : ""
    only: librarian.onlyCollection
    onOnlyToggled: (only) => librarian.onlyCollection = only
    suggestions: librarian.suggestions
    onWanted: (index) => librarian.wantSuggestion(index)
}
