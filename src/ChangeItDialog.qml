import QtQuick

// Changing Omabook by asking the AI for it (Ctrl+K): what to change, the
// AI's note on what it did, and the way back from it.
AskDialog {
    objectName: "changeIt"
    heading: qsTr("Change Omabook")
    placeholder: qsTr("Make it warmer, bigger covers, Space for the next chapter…")
    reply: librarian.changeNote
    working: librarian.changing
    undoable: librarian.canUndo
    onAsked: (words) => librarian.change(words)
    onUndo: librarian.undoChange()
}
