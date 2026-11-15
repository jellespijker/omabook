import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The books the AI finds for a question the search box can't put: "the
// books where a mentor dies", "something about control theory I haven't
// finished". The question is asked of the collection shown, a batch of books
// at a time; what it found shows as it comes in, the best first, each with
// why. A book opens with Enter or a click; the arrows move between them.
AppPopup {
    id: dialog
    widest: 640
    // Opened with a question to ask at once, or with none to type one.
    function ask(question) {
        const words = (question || "").trim().replace(/^\?\s*/, "")
        open()
        if (words !== "") {
            field.text = words
            librarian.find(words)
        }
    }
    // Selected on the shelf, which has the keyboard again, and read when it
    // has a file: once the dialog has closed and given the keyboard back.
    property var picked: null
    function openBook(book) {
        picked = book
        close()
    }

    onOpened: {
        field.forceActiveFocus()
        field.selectAll()
    }
    // Closed, it stops: nobody is there to read what it finds.
    onClosed: {
        librarian.stopFinding()
        if (!picked) return
        const book = picked
        picked = null
        win.selectOnShelf(book.bookId)
        if (book.readable) reader.openBook(book.bookId)
    }

    DialogBody {
        id: body
        Label {
            Layout.fillWidth: true
            text: qsTr("Ask the AI to find books")
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: !librarian.hasAi
            text: librarian.aiProblem
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "aiSearchWhere"
            Layout.fillWidth: true
            visible: librarian.hasAi
            text: backend.collection !== 0
                  ? qsTr("Looks through the books in %1: their details and what you know of them, never past where you are.").arg(backend.collectionName)
                  : qsTr("Looks through your books: their details and what you know of them, never past where you are.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        TextField {
            id: field
            objectName: "aiSearchWords"
            Layout.fillWidth: true
            visible: librarian.hasAi
            placeholderText: qsTr("The books where a mentor dies…")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: if (text.trim() !== "") librarian.find(text)
            Keys.onDownPressed: if (found.count > 0) found.forceActiveFocus()
        }
        Thinking {
            objectName: "aiSearchThinking"
            active: librarian.finding
            text: librarian.findStatus !== "" ? librarian.findStatus : qsTr("Thinking…")
        }
        Label {
            objectName: "aiSearchError"
            Layout.fillWidth: true
            visible: librarian.findError !== ""
            text: librarian.findError
            color: theme.colors.red
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "aiSearchNone"
            Layout.fillWidth: true
            visible: !librarian.finding && librarian.findError === "" && librarian.findQuestion !== "" && found.count === 0
            text: qsTr("No book answers that. Try other words, or ask about all books.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "aiSearchKept"
            Layout.fillWidth: true
            visible: librarian.findCached && found.count > 0
            text: qsTr("Asked before; the library hasn't changed since, so the AI wasn't asked again.")
            color: theme.colors.muted
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        ListView {
            id: found
            objectName: "aiSearchFound"
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            visible: count > 0
            interactive: false
            spacing: 4
            keyNavigationEnabled: true
            model: librarian.found
            Keys.onReturnPressed: if (currentIndex >= 0) dialog.openBook(librarian.found[currentIndex])
            Keys.onEnterPressed: if (currentIndex >= 0) dialog.openBook(librarian.found[currentIndex])
            Keys.onSpacePressed: if (currentIndex >= 0) dialog.openBook(librarian.found[currentIndex])
            Keys.onUpPressed: (event) => {
                if (currentIndex <= 0) field.forceActiveFocus()
                else decrementCurrentIndex()
            }
            onActiveFocusChanged: if (activeFocus && currentIndex < 0) currentIndex = 0
            // The book the keys are on is kept in sight.
            onCurrentIndexChanged: {
                if (!activeFocus || !currentItem) return
                const flick = body.scroller
                const top = currentItem.mapToItem(flick.contentItem, 0, 0).y
                if (top < flick.contentY) flick.contentY = Math.max(0, top - 8)
                else if (top + currentItem.height > flick.contentY + flick.height)
                    flick.contentY = Math.min(flick.contentHeight - flick.height, top + currentItem.height - flick.height + 8)
            }
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                objectName: "aiSearchBook" + index
                width: found.width
                height: Math.max(win.touch ? 48 : 40, lines.implicitHeight + 12)
                radius: theme.radius
                readonly property bool current: found.activeFocus && found.currentIndex === index
                color: current || rowHover.hovered ? theme.colors.selection : "transparent"
                border.width: current ? 2 : 0
                border.color: theme.foreground
                Accessible.role: Accessible.Button
                Accessible.name: modelData.title + ". " + modelData.why
                Accessible.onPressAction: dialog.openBook(modelData)

                ColumnLayout {
                    id: lines
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        text: modelData.authors !== "" ? qsTr("%1, by %2").arg(modelData.title).arg(modelData.authors) : modelData.title
                        font.bold: true
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: modelData.why
                        font.pixelSize: 13
                        color: theme.colors.muted
                        wrapMode: Text.Wrap
                    }
                }
                HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        found.currentIndex = row.index
                        dialog.openBook(row.modelData)
                    }
                }
            }
        }
        actions: [
            AppButton {
                objectName: "aiSearchAsk"
                visible: librarian.hasAi && !librarian.finding
                primary: true
                enabled: field.text.trim() !== ""
                text: qsTr("Ask")
                onClicked: librarian.find(field.text)
            },
            AppButton {
                objectName: "aiSearchStop"
                visible: librarian.finding
                text: qsTr("Stop")
                onClicked: librarian.stopFinding()
            },
            AppButton { objectName: "aiSearchClose"; text: qsTr("Close"); onClicked: dialog.close() }
        ]
    }
}
