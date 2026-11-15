import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Everything known of the selected node: what it is and is also called,
// what is said of it, the books it is in and where it was first met in
// each, when each thing known of it was revealed (each a way back to that
// moment in the text), and how it relates to other things, each relation a
// way to go on. What lies past the reader's furthest place is a spoiler: it
// is kept in a section of its own, closed until asked for and confirmed,
// and nothing of it shows anywhere else, not even to a screen reader.
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
    readonly property var timeline: node.timeline || []
    readonly property int aheadCount: node.aheadCount || 0
    readonly property bool aheadShown: !!node.aheadShown
    // Asked once, before what lies ahead is shown: for this node only, and
    // still asked when the details are read again (the graph grew).
    property var askingFor: 0
    readonly property bool asking: hasNode && askingFor === node.id && !aheadShown
    function ask(asked) {
        askingFor = asked ? node.id : 0
        if (asked) {
            aheadShowButton.forceActiveFocus()
            sightSoon.show(aheadShowButton)
        }
    }
    // Once what just appeared is laid out.
    Timer {
        id: sightSoon
        property Item target: null
        interval: 40
        function show(item) { target = item; restart() }
        onTriggered: if (target) panel.keepInSight(target)
    }
    // Whether a row is in sight in the scrolled details. A curve-drawn mark
    // of a row scrolled out of sight would still show over what is beside
    // the details, so the rows drop theirs while out of sight.
    function inSight(item, top, height) {
        const y = item.mapToItem(column, 0, 0).y
        return y >= top && y + item.height <= top + height
    }
    // Brings something of the details into sight, scrolling as little as it can.
    function keepInSight(item) {
        const top = item.mapToItem(column, 0, 0).y
        const bottom = top + item.height
        if (bottom > flick.contentY + flick.height) flick.contentY = Math.min(Math.max(0, flick.contentHeight - flick.height), bottom - flick.height + 8)
        if (top < flick.contentY) flick.contentY = Math.max(0, top - 8)
    }

    signal jump(var id)
    signal look()
    signal go(var place)
    signal read(var bookId)
    signal edit()
    signal merge()
    signal remove()
    signal unlink(var relation)
    // What lies ahead of the reader, for this node: opened or closed again.
    signal showAhead(var id, bool shown)
    // The keyboard goes back to the picture.
    signal leave()

    // The keyboard to the first moment, or the section of what lies ahead;
    // false when there is neither.
    function takeKeys() {
        const first = timelineRows.count > 0 ? timelineRows.itemAt(0) : aheadToggle.visible ? aheadToggle : null
        if (!first) return false
        first.forceActiveFocus()
        return true
    }
    // Tab and Shift+Tab move among the moments and Escape leaves them, here
    // rather than as the picture's keys.
    function moves(event) {
        return event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab || event.key === Qt.Key_Escape
    }
    function move(item, event) {
        if (event.key === Qt.Key_Escape) {
            panel.leave()
            return
        }
        const next = item.nextItemInFocusChain(event.key === Qt.Key_Tab)
        if (next && next.visible && panel.holds(next)) next.forceActiveFocus()
        else panel.leave()
    }
    // Whether an item is part of the details, scrolled into sight or not.
    function holds(item) {
        for (let up = item; up; up = up.parent)
            if (up === column) return true
        return false
    }
    // Where the details stand in the picture's place, a way back to it.
    property bool closable: false
    signal close()

    Words { id: shelfWords }
    GraphWords { id: words }

    // Where in a book: the chapter's name when the book is open, else its number.
    function placeText(place) { return words.place(place) }
    // Whether a key works the row that has the keyboard.
    function activates(event) {
        return event.modifiers === Qt.NoModifier
               && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)
    }
    // What a step of the timeline says: the summary, the name, or the relation.
    function stepText(step) {
        if (step.what === "alias") return qsTr("Called %1").arg(step.text)
        if (step.what === "met") return qsTr("First met")
        if (step.what === "relation")
            return step.outgoing ? step.text + " " + step.otherName : step.otherName + " " + step.text
        return step.text
    }

    // A moment of the book: where it is, what was revealed there, and a way
    // to it. The whole row is the way, by pointer, finger or keyboard.
    component MomentRow: Rectangle {
        id: moment
        required property var step
        property bool ahead: false
        objectName: ahead ? "graphAheadStep" : "graphMoment"
        Layout.fillWidth: true
        Layout.leftMargin: -6
        implicitHeight: Math.max(win.touch ? 44 : 32, momentLines.implicitHeight + 10)
        radius: Math.min(4, theme.radius)
        color: (momentHover.hovered || activeFocus) && step.readable ? theme.colors.selection : "transparent"
        border.width: activeFocus ? 1 : 0
        border.color: theme.foreground
        activeFocusOnTab: step.readable
        onActiveFocusChanged: if (activeFocus) panel.keepInSight(moment)
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Go to this moment: %1").arg(panel.placeText(step))
        Accessible.onPressAction: panel.go(step)
        // Enter and Space are the row's while it has the keyboard, not the
        // picture's shortcuts for them.
        Keys.onShortcutOverride: (event) => event.accepted = panel.activates(event) || panel.moves(event)
        Keys.onPressed: (event) => { if (panel.moves(event)) { panel.move(moment, event); event.accepted = true } }
        Keys.onReturnPressed: panel.go(step)
        Keys.onEnterPressed: panel.go(step)
        Keys.onSpacePressed: panel.go(step)
        ColumnLayout {
            id: momentLines
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 6
            anchors.rightMargin: moment.step.readable ? 26 : 6
            spacing: 1
            // "Ahead · The Crossing", or "The Crossing": where it was revealed.
            Label {
                Layout.fillWidth: true
                text: moment.ahead ? qsTr("Ahead · %1").arg(panel.placeText(moment.step)) : panel.placeText(moment.step)
                color: moment.ahead ? theme.accent : theme.colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
            Label {
                Layout.fillWidth: true
                text: panel.stepText(moment.step)
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
        }
        Glyph {
            id: momentMark
            visible: moment.step.readable && panel.inSight(momentMark, flick.contentY, flick.height)
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            name: win.rightToLeft ? "back" : "forward"
            size: 12
            color: theme.colors.muted
        }
        HoverHandler { id: momentHover }
        TapHandler { enabled: moment.step.readable; onTapped: panel.go(moment.step) }
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
            // Met only past the reader's place (a book shown whole): said
            // plainly, with nothing of what is known of it.
            Label {
                objectName: "graphDetailsLater"
                Layout.fillWidth: true
                visible: !!panel.node.nodeAhead
                text: (panel.node.aheadAfter || {}).bookId ? qsTr("Met only after %1, past where you are.").arg(panel.placeText(panel.node.aheadAfter))
                                                          : qsTr("Met later in the book.")
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
                        id: metMark
                        visible: met.modelData.readable && panel.inSight(metMark, flick.contentY, flick.height)
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

            // When what is known of it was revealed, each a way back there.
            Label {
                id: timelineTitle
                objectName: "graphTimelineTitle"
                Layout.fillWidth: true
                Layout.topMargin: 10
                visible: panel.timeline.length > 0
                text: qsTr("When it was revealed")
                color: theme.colors.muted
                font.pixelSize: 12
            }
            Repeater {
                id: timelineRows
                model: panel.timeline
                delegate: MomentRow {
                    required property var modelData
                    step: modelData
                }
            }

            // What lies past the reader's furthest place: closed, and opened
            // only once it is confirmed, for this node.
            ColumnLayout {
                id: aheadBox
                objectName: "graphAhead"
                Layout.fillWidth: true
                Layout.topMargin: 10
                visible: panel.aheadCount > 0
                spacing: 4
                readonly property string after: (panel.node.aheadAfter || {}).bookId ? panel.placeText(panel.node.aheadAfter) : ""

                Rectangle {
                    id: aheadToggle
                    objectName: "graphAheadToggle"
                    Layout.fillWidth: true
                    Layout.leftMargin: -6
                    implicitHeight: Math.max(win.touch ? 44 : 32, aheadHead.implicitHeight + 10)
                    radius: Math.min(4, theme.radius)
                    color: aheadHover.hovered || activeFocus ? theme.colors.selection : "transparent"
                    border.width: activeFocus ? 1 : 0
                    border.color: theme.foreground
                    activeFocusOnTab: true
                    onActiveFocusChanged: if (activeFocus) panel.keepInSight(aheadToggle)
                    // Says how many, and that they are hidden: never what they say.
                    Accessible.role: Accessible.Button
                    Accessible.name: panel.aheadShown ? qsTr("Hide what happens later (%Ln)", "", panel.aheadCount)
                                                      : qsTr("Later in the book (%Ln), hidden", "", panel.aheadCount)
                    Accessible.onPressAction: toggle()
                    Keys.onShortcutOverride: (event) => event.accepted = panel.activates(event) || panel.moves(event)
                    Keys.onPressed: (event) => { if (panel.moves(event)) { panel.move(aheadToggle, event); event.accepted = true } }
                    Keys.onReturnPressed: toggle()
                    Keys.onEnterPressed: toggle()
                    Keys.onSpacePressed: toggle()
                    function toggle() {
                        if (panel.aheadShown) panel.showAhead(panel.node.id, false)
                        else panel.ask(!panel.asking)
                    }
                    RowLayout {
                        id: aheadHead
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 6
                        anchors.rightMargin: 8
                        spacing: 8
                        Glyph {
                            name: "alert"
                            size: 14
                            color: theme.accent
                        }
                        Label {
                            objectName: "graphAheadTitle"
                            Layout.fillWidth: true
                            text: qsTr("Later in the book (%Ln)", "", panel.aheadCount)
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        Glyph {
                            name: panel.aheadShown || panel.asking ? "up" : "down"
                            size: 12
                            color: theme.colors.muted
                        }
                    }
                    HoverHandler { id: aheadHover }
                    TapHandler { onTapped: aheadToggle.toggle() }
                }
                Label {
                    objectName: "graphAheadWarning"
                    Layout.fillWidth: true
                    visible: !panel.aheadShown
                    text: panel.asking
                          ? (aheadBox.after !== "" ? qsTr("This shows what happens after %1.").arg(aheadBox.after)
                                                 : qsTr("This shows what happens later in the book."))
                          : qsTr("Hidden, as it gives away what happens later.")
                    color: panel.asking ? theme.foreground : theme.colors.muted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
                Flow {
                    Layout.fillWidth: true
                    visible: panel.asking && !panel.aheadShown
                    spacing: 8
                    AppButton {
                        id: aheadShowButton
                        objectName: "graphAheadShow"
                        primary: true
                        text: qsTr("Show")
                        Keys.onShortcutOverride: (event) => event.accepted = panel.activates(event)
                        Keys.onSpacePressed: clicked()
                        onClicked: {
                            panel.ask(false)
                            panel.showAhead(panel.node.id, true)
                            sightSoon.show(aheadList)
                        }
                    }
                    AppButton {
                        objectName: "graphAheadCancel"
                        text: qsTr("Not Now")
                        Keys.onShortcutOverride: (event) => event.accepted = panel.activates(event)
                        Keys.onSpacePressed: clicked()
                        onClicked: {
                            panel.ask(false)
                            aheadToggle.forceActiveFocus()
                        }
                    }
                }
                // Opened: each step marked as ahead, on a tint of its own.
                Rectangle {
                    id: aheadList
                    objectName: "graphAheadList"
                    Layout.fillWidth: true
                    Layout.leftMargin: -6
                    visible: panel.aheadShown
                    implicitHeight: aheadSteps.implicitHeight + 8
                    radius: Math.min(4, theme.radius)
                    color: Qt.alpha(theme.accent, 0.07)
                    ColumnLayout {
                        id: aheadSteps
                        x: 6
                        y: 4
                        width: parent.width - 6
                        spacing: 2
                        Repeater {
                            model: panel.aheadShown ? (panel.node.ahead || []) : []
                            delegate: MomentRow {
                                required property var modelData
                                step: modelData
                                ahead: true
                            }
                        }
                    }
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
