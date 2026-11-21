import QtQuick

// The sentence being read aloud (#73, ReadAloud), marked where it stands: a
// wash of the accent with a line under it, beneath the words of a chapter a
// TextEdit shows (`text`), or over the words of a PDF's page (`page`). It
// takes no input, and lies apart from the highlights and notes, which stay as
// they are. Each new sentence it marks it says where (`marked`, in its own
// coordinates), for the view to keep it in sight.
Item {
    id: mark
    objectName: "sentenceMark"
    property TextEdit text: null
    property int page: -1
    // One a line of the sentence: [rect].
    property var rects: []
    // The sentence last said to be marked, so a new layout of the same one doesn't move the view.
    property int told: -1
    readonly property var aloud: reader.readAloud
    signal marked(rect first)

    function lay() {
        if (mark.text) rects = mark.text.textDocument ? aloud.rectsIn(mark.text.textDocument) : []
        else rects = page >= 0 && width > 0 ? aloud.rectsOnPage(page, Qt.size(width, height)) : []
        if (rects.length > 0 && told !== aloud.sentence) {
            told = aloud.sentence
            marked(rects[0])
        }
    }
    // Laid once the text is laid out: a timer of its own, which goes with this item.
    Timer { id: soon; interval: 0; onTriggered: mark.lay() }
    Component.onCompleted: if (aloud.active) soon.restart()
    onWidthChanged: if (aloud.active) soon.restart()
    Connections {
        target: mark.aloud
        function onSentenceChanged() { soon.restart() }
    }
    Connections {
        target: mark.text
        enabled: mark.aloud.active
        function onTextChanged() { soon.restart() }
        function onContentHeightChanged() { soon.restart() }
    }

    Repeater {
        model: mark.rects
        delegate: Item {
            required property rect modelData
            objectName: "sentenceRect"
            x: modelData.x
            y: modelData.y
            width: modelData.width
            height: modelData.height
            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: -2
                anchors.rightMargin: -2
                radius: Math.min(3, theme.radius)
                color: Qt.alpha(theme.accent, 0.16)
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 2
                color: theme.accent
            }
        }
    }
}
