import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Everything known of the selected node: what it is and is also called,
// what is said of it, the books it is in and where it was first met in
// each, and how it relates to other things, each relation a way to go on.
Item {
    id: panel
    // GraphBrowser.details: empty when nothing is selected.
    property var node: ({})
    // Whether it stands under the picture rather than beside it.
    property bool below: false
    readonly property bool hasNode: node.id !== undefined
    readonly property var places: node.places || []
    readonly property var relations: node.relations || []
    readonly property bool isBook: node.kind === "book" && (node.bookId || 0) > 0

    signal jump(var id)
    signal look()
    signal go(var place)
    signal read(var bookId)
    signal edit()
    signal merge()
    signal remove()
    signal unlink(var relation)
    // Where the details stand in the picture's place, a way back to it.
    property bool closable: false
    signal close()

    Words { id: shelfWords }
    GraphWords { id: words }

    // Where in a book: the chapter's name when the book is open, else its number.
    function placeText(place) {
        if (reader.open && reader.bookId === place.bookId) {
            let title = ""
            for (const entry of reader.toc)
                if (entry.section <= place.section) title = entry.title
            if (title !== "") return title
        }
        return qsTr("part %1").arg((place.section + 1).toLocaleString(Qt.locale(), 'f', 0))
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        width: panel.below ? parent.width : 1
        height: panel.below ? 1 : parent.height
        color: theme.colors.selection
    }

    Flickable {
        id: flick
        objectName: "graphDetailsFlick"
        anchors.fill: parent
        anchors.margins: panel.below ? 16 : 20
        contentHeight: column.implicitHeight
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            // Short of the scroll bar, which would take the taps meant for
            // what is under it.
            width: flick.width - 14
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                GraphKind {
                    kind: panel.node.kind || ""
                    size: 20
                }
                Label {
                    objectName: "graphDetailsKind"
                    Layout.fillWidth: true
                    text: words.kind(panel.node.kind || "")
                    color: theme.colors.muted
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Rectangle {
                    objectName: "graphDetailsClose"
                    visible: panel.closable
                    implicitWidth: win.touch ? 40 : 28
                    implicitHeight: implicitWidth
                    radius: theme.radius
                    color: closeHover.hovered ? theme.colors.selection : "transparent"
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Back to the picture")
                    Glyph {
                        anchors.centerIn: parent
                        name: "cross"
                        size: 14
                        color: theme.colors.muted
                    }
                    HoverHandler { id: closeHover }
                    TapHandler { onTapped: panel.close() }
                }
            }
            Label {
                objectName: "graphDetailsName"
                Layout.fillWidth: true
                text: panel.node.name || ""
                font.pixelSize: 19
                font.bold: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            // A thing of another book says which book it is from.
            Label {
                objectName: "graphDetailsFrom"
                Layout.fillWidth: true
                visible: !!panel.node.outer && (panel.node.from || "") !== ""
                text: qsTr("From %1").arg(panel.node.from || "")
                color: theme.accent
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            Label {
                objectName: "graphDetailsAliases"
                Layout.fillWidth: true
                visible: (panel.node.aliases || []).length > 0
                text: qsTr("Also called %1").arg((panel.node.aliases || []).join(", "))
                color: theme.colors.muted
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }

            // A book: who wrote it, how it stands, and the days it was read on.
            Label {
                Layout.fillWidth: true
                visible: panel.isBook && (panel.node.authors || "") !== ""
                text: panel.node.authors || ""
                wrapMode: Text.Wrap
            }
            Stars {
                objectName: "graphDetailsStars"
                visible: panel.isBook && (panel.node.rating || 0) > 0
                rating: panel.node.rating || 0
                size: 15
                color: theme.accent
            }
            Label {
                objectName: "graphDetailsStatus"
                Layout.fillWidth: true
                visible: panel.isBook && text !== ""
                text: {
                    translator.language
                    if (!panel.isBook) return ""
                    const status = panel.node.status || ""
                    const percent = Math.round(panel.node.percent || 0)
                    const where = status === "read" && panel.node.finished ? qsTr("Read, finished %1").arg(panel.node.finished)
                                : status === "" ? "" : shelfWords.status(status)
                    const far = status !== "read" && percent > 0 ? qsTr("%1% read").arg(percent.toLocaleString(Qt.locale(), 'f', 0)) : ""
                    return [where, far].filter(part => part !== "").join(" · ")
                }
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "graphDetailsDays"
                Layout.fillWidth: true
                readonly property var days: panel.node.days || []
                visible: panel.isBook && days.length > 0
                text: days.length > 0 ? qsTr("Read on %Ln days, last on %1", "", days.length).arg(days[days.length - 1].day) : ""
                color: theme.colors.muted
                wrapMode: Text.Wrap
            }

            // What is said of it. For a question, that is the answer's gist.
            Label {
                objectName: "graphDetailsSummary"
                Layout.fillWidth: true
                Layout.topMargin: 2
                visible: text !== ""
                text: panel.node.summary || panel.node.answer || ""
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                lineHeight: 1.15
            }

            // What can be done with it. A Flow, so longer words wrap.
            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 8
                AppButton {
                    objectName: "graphRead"
                    visible: panel.isBook && !!panel.node.readable
                    primary: true
                    focusPolicy: Qt.NoFocus
                    text: qsTr("Read")
                    onClicked: panel.read(panel.node.bookId)
                }
                AppButton {
                    objectName: "graphLook"
                    focusPolicy: Qt.NoFocus
                    text: panel.node.outer ? qsTr("See it in its book") : qsTr("Look around")
                    onClicked: panel.look()
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 10
                visible: panel.places.length > 0
                text: qsTr("First met")
                color: theme.colors.muted
                font.pixelSize: 12
            }
            Repeater {
                model: panel.places
                delegate: Rectangle {
                    id: met
                    required property var modelData
                    objectName: "graphPlace"
                    Layout.fillWidth: true
                    Layout.leftMargin: -6
                    implicitHeight: Math.max(win.touch ? 44 : 30, metText.implicitHeight + 10)
                    radius: Math.min(4, theme.radius)
                    color: metHover.hovered && modelData.readable ? theme.colors.selection : "transparent"
                    Label {
                        id: metText
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 6
                        anchors.rightMargin: met.modelData.readable ? 26 : 6
                        // "The Lantern Tide · The Crossing"
                        text: met.modelData.title + " · " + panel.placeText(met.modelData)
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                    }
                    // A book that can be opened here is a way to that place in it.
                    Glyph {
                        visible: met.modelData.readable
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        name: win.rightToLeft ? "back" : "forward"
                        size: 12
                        color: theme.colors.muted
                    }
                    HoverHandler { id: metHover }
                    TapHandler { enabled: met.modelData.readable; onTapped: panel.go(met.modelData) }
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 10
                visible: panel.relations.length > 0
                text: qsTr("Relations")
                color: theme.colors.muted
                font.pixelSize: 12
            }
            Repeater {
                model: panel.relations
                delegate: Rectangle {
                    id: relation
                    required property var modelData
                    objectName: "graphRelation"
                    // "mentor of → Aldo" for what it does, "Aldo → mentor of"
                    // for what is done to it: the selected node is at the open end.
                    readonly property string arrow: win.rightToLeft ? "←" : "→"
                    Layout.fillWidth: true
                    Layout.leftMargin: -6
                    implicitHeight: Math.max(win.touch ? 44 : 32, lines.implicitHeight + 10)
                    radius: Math.min(4, theme.radius)
                    color: relationHover.hovered ? theme.colors.selection : "transparent"
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.outgoing ? modelData.relation + " " + modelData.otherName
                                                        : modelData.otherName + " " + modelData.relation

                    ColumnLayout {
                        id: lines
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: drop.visible ? drop.left : parent.right
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        spacing: 1
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                visible: relation.modelData.outgoing
                                text: relation.modelData.relation
                                color: Qt.alpha(theme.foreground, 0.7)
                                elide: Text.ElideRight
                                Layout.maximumWidth: lines.width * 0.5
                            }
                            Label {
                                visible: relation.modelData.outgoing
                                text: relation.arrow
                                color: theme.colors.muted
                            }
                            GraphKind {
                                kind: relation.modelData.otherKind
                                size: 16
                            }
                            Label {
                                objectName: "graphRelationName"
                                Layout.fillWidth: relation.modelData.outgoing
                                Layout.maximumWidth: relation.modelData.outgoing ? -1 : lines.width * 0.5
                                text: relation.modelData.otherName
                                elide: Text.ElideRight
                            }
                            Label {
                                visible: !relation.modelData.outgoing
                                text: relation.arrow
                                color: theme.colors.muted
                            }
                            Label {
                                visible: !relation.modelData.outgoing
                                Layout.fillWidth: true
                                text: relation.modelData.relation
                                color: Qt.alpha(theme.foreground, 0.7)
                                elide: Text.ElideRight
                            }
                        }
                        // "echoes → Tide-binding", and under it "in The Salt Clock".
                        Label {
                            objectName: "graphRelationFrom"
                            Layout.fillWidth: true
                            visible: text !== ""
                            text: (relation.modelData.otherFrom || "") !== "" ? qsTr("in %1").arg(relation.modelData.otherFrom) : ""
                            color: theme.accent
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: text !== ""
                            text: relation.modelData.note
                            color: theme.colors.muted
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                    }
                    // Taking the relation out, for a pointer over it or a finger.
                    Rectangle {
                        id: drop
                        objectName: "graphUnlink"
                        visible: relationHover.hovered || win.touch
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.rightMargin: 4
                        width: win.touch ? 36 : 24
                        height: width
                        radius: Math.min(4, theme.radius)
                        color: dropHover.hovered ? theme.background : "transparent"
                        Glyph {
                            anchors.centerIn: parent
                            name: "cross"
                            size: 12
                            color: Qt.alpha(theme.foreground, 0.6)
                        }
                        HoverHandler { id: dropHover }
                        TapHandler { onTapped: panel.unlink(relation.modelData) }
                    }
                    HoverHandler { id: relationHover }
                    // The rest of the row goes to the node at the other end.
                    Item {
                        anchors.fill: parent
                        anchors.rightMargin: drop.visible ? drop.width + 8 : 0
                        TapHandler { onTapped: panel.jump(relation.modelData.other) }
                    }
                }
            }

            // Changing it by hand: rarer, so after everything there is to read.
            Flow {
                Layout.fillWidth: true
                Layout.topMargin: 12
                spacing: 8
                AppButton {
                    objectName: "graphEditButton"
                    focusPolicy: Qt.NoFocus
                    text: qsTr("Edit")
                    onClicked: panel.edit()
                }
                AppButton {
                    objectName: "graphMergeButton"
                    // A book stands for a book of the library; those are merged there.
                    visible: !panel.isBook
                    focusPolicy: Qt.NoFocus
                    text: qsTr("Merge")
                    onClicked: panel.merge()
                }
                AppButton {
                    objectName: "graphRemoveButton"
                    visible: !panel.isBook
                    focusPolicy: Qt.NoFocus
                    text: qsTr("Remove")
                    onClicked: panel.remove()
                }
            }
        }
    }
}
