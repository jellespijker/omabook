import QtQuick

// Puts text on the clipboard as it is given: a reader's selection without
// the soft hyphens hyphenation set in the page (hyphenation.h), which a
// TextEdit's own copy would take along. QML has no clipboard of its own;
// a TextEdit that is never shown copies for it.
TextEdit {
    visible: false
    textFormat: TextEdit.PlainText
    function copyText(words) {
        // A selection's paragraphs end in U+2029, its lines in U+2028.
        text = words.replace(/[\u2028\u2029]/g, "\n")
        selectAll()
        copy()
        text = ""
    }
}
