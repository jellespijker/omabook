import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// Find words anywhere in the book and go to where they are: in its text, and
// in what you wrote in it, listed first: your highlights, margin notes (typed
// or read from the handwriting, #90), bookmarks and the words of your ink
// (#115); "notes:" in front looks only there. A book indexed by its meaning
// (#72) is found by what the words mean too, listed after, up to where you
// are unless you ask for the whole book.
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
    readonly property QtObject windowSettings: settings
    property string whySeen: ""
    // The embedding model, found on the server or downloaded onto it, then indexed with.
    ModelHelp {
        id: findModel
        capability: "embeddings"
        settings: popup.windowSettings
        onChosen: meant.index()
    }
    // What to do, by why indexing couldn't start or stopped.
    function toDo(why) {
        switch (why) {
        case "unreachable": return qsTr("Start the server, or check its address on the AI page, then index again.")
        case "refused":
        case "no-key": return qsTr("Give the server's key on the AI page.")
        case "hosted-not-allowed": return qsTr("Allow a hosted service for meaning on the AI page, or choose a server of your own.")
        case "ai-off": return qsTr("Turn on Use AI on the AI page.")
        case "no-server": return qsTr("Run Ollama, or give a server's address for meaning on the AI page.")
        }
        return ""
    }
    AnnotationFinder { id: noted; reader: popup.windowReader }
    // What the words mean, found beside the window and added when it is there.
    MeaningFinder {
        id: meant
        reader: popup.windowReader
        settings: popup.windowSettings
        // The server lacks the model: what it has is looked at, to offer one (#172).
        onChanged: {
            if (meant.why === popup.whySeen) return
            popup.whySeen = meant.why
            if (meant.why === "no-model") findModel.look()
        }
        onFound: (results) => {
            const words = popup.results.filter(result => !result.meant)
            popup.results = words.concat(results)
            if (words.length === 0 && results.length > 0) list.currentIndex = 0
        }
    }
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
                if (noted.onlyNotes(text) === "") meant.find(text)
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
        // Finding by meaning: offered for a book not indexed yet, saying first
        // where its text goes; under way, with a way to stop; and once it is
        // there, whether to look past where you are.
        RowLayout {
            objectName: "findMeaning"
            Layout.fillWidth: true
            visible: reader.open
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 12
                // Why not is said below, with what to do.
                visible: !whyNot.visible
                color: meant.hosted && meant.state !== "ready" ? theme.colors.orange : theme.colors.muted
                text: meant.state === "indexing" ? (meant.where !== "" ? qsTr("Indexing this book by its meaning with %1… %2%").arg(meant.where).arg(Math.round(meant.progress * 100))
                                                                       : qsTr("Indexing this book by its meaning…"))
                      : meant.state === "ready" ? qsTr("Also found by what the words mean.")
                      : meant.hosted ? qsTr("Find what you mean, not only your words: the book's text goes to %1 to be indexed, someone else's computer, as your settings allow.").arg(meant.where)
                      : qsTr("Find what you mean, not only your words: the book's text is indexed by %1, your own server.").arg(meant.where)
            }
            // Why it couldn't be indexed or searched, and what to do (#172).
            ColumnLayout {
                id: whyNot
                objectName: "findMeaningWhy"
                Layout.fillWidth: true
                visible: meant.problem !== "" && meant.state !== "indexing"
                spacing: 6
                Label {
                    objectName: "findMeaningProblem"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                    color: theme.colors.orange
                    text: [meant.problem, popup.toDo(meant.why)].filter(part => !!part).join(" ")
                }
                ModelOffer {
                    Layout.fillWidth: true
                    name: "findModel"
                    help: findModel
                    visible: meant.why === "no-model" && stage !== "" && stage !== "none" && stage !== "there"
                }
                AppButton {
                    objectName: "findMeaningSettings"
                    visible: popup.toDo(meant.why) !== "" && meant.why !== "unreachable"
                    text: qsTr("AI Page…")
                    onClicked: {
                        popup.close()
                        win.showAiSettings()
                    }
                }
            }
            AppButton {
                objectName: "findMeaningIndex"
                visible: (meant.state === "none" || meant.state === "stale") && findModel.state !== "downloading"
                text: meant.state === "stale" ? qsTr("Index again") : meant.problem !== "" ? qsTr("Index again") : qsTr("Index")
                onClicked: meant.index()
            }
            AppButton {
                objectName: "findMeaningCancel"
                visible: meant.state === "indexing"
                text: qsTr("Cancel")
                onClicked: meant.cancel()
            }
            CheckBox {
                objectName: "findMeaningWhole"
                visible: meant.state === "ready"
                text: qsTr("Past where I am")
                checked: meant.whole
                onToggled: {
                    meant.whole = checked
                    if (popup.searched) meant.find(words.text)
                }
            }
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
                // What you wrote, told from the book's own text by a bar in the
                // accent; what was found by its meaning by a fainter one.
                Rectangle {
                    visible: !!row.modelData.kind || !!row.modelData.meant
                    opacity: row.modelData.meant ? 0.4 : 1
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
