import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// Find words anywhere in the book and go to where they are: in its text, and
// in what you wrote in it, listed first: your highlights, margin notes (typed
// or read from the handwriting, #90), bookmarks and the words of your ink
// (#115); "notes:" in front looks only there.
AppPopup {
    id: popup
    objectName: "finder"
    widest: 560
    tallest: 600
    property var results: []
    property bool searched: false

    // Opens on these words, found at once: words selected on the page.
    property string pending: ""
    function findWords(text) {
        pending = text.trim()
        open()
    }
    function go(result) {
        // A highlight or a note on words of a chapter goes to the very words, lit.
        if (result.quote && reader.kind === "flow") reader.marks.goToMoment(result.section, result.fraction, result.quote.slice(0, 120))
        else if (!result.lost) reader.goToPlace(result.section, result.fraction)
        close()
    }
    // The window's reader, by a name of the popup's own: inside AnnotationFinder `reader` is its own.
    readonly property QtObject windowReader: reader
    AnnotationFinder { id: noted; reader: popup.windowReader }
    function search(text) {
        const only = noted.onlyNotes(text)
        return only !== "" ? noted.find(only) : noted.find(text).concat(reader.find(text))
    }

    onOpened: {
        if (pending !== "") {
            words.text = pending
            pending = ""
            words.accepted()
        }
        words.selectAll()
        if (!list.activeFocus) words.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        TextField {
            id: words
            objectName: "findWords"
            Layout.fillWidth: true
            placeholderText: qsTr("Find in this book and your notes")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: {
                popup.results = popup.search(text)
                popup.searched = true
                if (popup.results.length > 0) {
                    list.currentIndex = 0
                    list.forceActiveFocus()
                }
            }
            Keys.onDownPressed: if (list.count > 0) list.forceActiveFocus()
        }
        Label {
            objectName: "findCount"
            visible: popup.searched
            text: popup.results.length === 0 ? qsTr("Not found in this book.")
                                             : qsTr("%Ln places", "", popup.results.length)
            color: theme.colors.muted
        }
        ListView {
            id: list
            objectName: "findList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: popup.results
            ScrollBar.vertical: ScrollBar {}
            Keys.onReturnPressed: if (currentIndex >= 0) popup.go(popup.results[currentIndex])
            Keys.onEnterPressed: if (currentIndex >= 0) popup.go(popup.results[currentIndex])

            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                width: list.width
                // A finger's width to tap, however short what it says.
                height: Math.max(win.touch ? 44 : 0, column.implicitHeight + 14)
                radius: Math.min(4, theme.radius)
                color: list.currentIndex === index ? theme.colors.selection : "transparent"
                // What you wrote, told from the book's own text by a bar in the accent.
                Rectangle {
                    visible: !!row.modelData.kind
                    x: 2
                    y: 6
                    width: 3
                    height: parent.height - 12
                    radius: 1.5
                    color: theme.accent
                }

                Column {
                    id: column
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 10
                    spacing: 2
                    Label {
                        width: parent.width
                        visible: text !== ""
                        text: row.modelData.title
                        color: theme.colors.muted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Label {
                        width: parent.width
                        text: row.modelData.snippet
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
                TapHandler { onTapped: popup.go(row.modelData) }
            }
        }
    }
}
