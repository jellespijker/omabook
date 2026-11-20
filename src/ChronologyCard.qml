import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What is known of an event of the chronology: when it happens, in the
// book's words too, the timelines it is on and what it is during, who and
// what it involves, the books it was met in, and every contradiction it is
// in with what was said where. From here back to the page it was revealed
// on, or to it in the graph.
Rectangle {
    id: card
    property var event: ({})
    // Beside the picture (a line on its inner edge), or under it (along its top).
    property bool beside: true
    signal page()
    signal graph()
    signal close()
    signal involve(var id)
    color: theme.background

    function placeOf(where) {
        if (!where || !(where.bookId > 0)) return qsTr("put in by hand")
        return qsTr("%1, %2", "a book, and where in it").arg(where.title).arg(words.place(where))
    }

    GraphWords { id: words }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        width: card.beside ? 1 : parent.width
        height: card.beside ? parent.height : 1
        color: theme.colors.selection
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 16
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            width: flick.width
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    objectName: "chronologyCardName"
                    Layout.fillWidth: true
                    text: card.event.name || ""
                    font.pixelSize: 17
                    font.bold: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
                Rectangle {
                    objectName: "chronologyCardClose"
                    readonly property string runs: "chronology.details"
                    Layout.alignment: Qt.AlignTop
                    width: win.touch ? 40 : 28
                    height: width
                    radius: theme.radius
                    color: closeHover.hovered ? theme.colors.selection : "transparent"
                    Glyph {
                        anchors.centerIn: parent
                        name: "cross"
                        size: 12
                        color: theme.colors.muted
                    }
                    HoverHandler { id: closeHover }
                    TapHandler { onTapped: card.close() }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: (card.event.from || "") !== ""
                text: qsTr("From %1").arg(card.event.from || "")
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "chronologyCardWhen"
                Layout.fillWidth: true
                visible: text !== ""
                text: card.event.when || ""
                font.pixelSize: 15
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: (card.event.said || "") !== "" && card.event.said !== card.event.when
                text: qsTr("“%1”, as the book says it").arg(card.event.said || "")
                font.italic: true
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: card.event.summary || ""
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            Label {
                Layout.fillWidth: true
                visible: (card.event.timelines || []).length > 0
                text: qsTr("On %1").arg((card.event.timelines || []).join(", "))
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: (card.event.during || "") !== ""
                text: qsTr("During %1").arg(card.event.during || "")
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            // Who and what it involves: each a filter, a tap away.
            Flow {
                Layout.fillWidth: true
                visible: (card.event.involved || []).length > 0
                spacing: 6
                Repeater {
                    model: card.event.involved || []
                    delegate: ChronologyChip {
                        required property var modelData
                        text: modelData.name
                        room: column.width
                        removes: false
                        onTapped: card.involve(modelData.id)
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: (card.event.books || []).length > 0
                text: qsTr("Met in %1").arg((card.event.books || []).map(book => book.title).join(", "))
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            // What can't all be true, with what was said where, and which was followed.
            Repeater {
                model: card.event.contradictions || []
                delegate: ColumnLayout {
                    id: trouble
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        text: trouble.modelData.kind
                        font.bold: true
                        color: theme.colors.yellow || theme.accent
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: trouble.modelData.sources
                        delegate: Label {
                            required property var modelData
                            Layout.fillWidth: true
                            text: qsTr("%1 — %2", "what was said, and where").arg(modelData.said).arg(card.placeOf(modelData.where))
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Set aside: %1. %2").arg(trouble.modelData.setAside).arg(trouble.modelData.rule)
                        color: theme.colors.muted
                        wrapMode: Text.Wrap
                    }
                }
            }
            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 8
                AppButton {
                    objectName: "chronologyBackToPage"
                    runs: "chronology.page"
                    primary: true
                    width: Math.min(implicitWidth, column.width)
                    text: qsTr("Back to the page")
                    enabled: !!card.event.page && !!card.event.page.readable
                    focusPolicy: Qt.NoFocus
                    onClicked: card.page()
                }
                AppButton {
                    objectName: "chronologyShowInGraph"
                    runs: "chronology.graph"
                    width: Math.min(implicitWidth, column.width)
                    text: qsTr("Show in the graph")
                    focusPolicy: Qt.NoFocus
                    onClicked: card.graph()
                }
            }
        }
    }
}
