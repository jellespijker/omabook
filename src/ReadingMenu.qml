import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// What can be done with something on the page, beside it: a small card for
// a thing of the graph that is pointed at or tapped (its name, kind and what
// is known of it), and a menu for words that were selected (ask about them,
// who or what they are, the graph, find, copy, and questions worth asking).
// It stands under the words, or over them where there is no room below,
// and never leaves the page. Up and Down (or J and K) and Enter work it from
// the keyboard; Escape puts it away.
FocusScope {
    id: menu
    objectName: "readingMenu"
    // "card" for a thing, "menu" for selected words.
    property string mode: "card"
    // The thing, as ReadingMarks.thing gives it; null for words the graph doesn't know.
    property var thing: null
    property string words: ""
    property string paragraph: ""
    // Opened from an answer of the AI, so where the thing was first met is offered.
    property bool fromAnswer: false
    // "Who or what is this?" was answered from the graph: its relations show.
    property bool answered: false
    property bool shown: false
    // Whether it was opened by a hover, and goes when the pointer leaves.
    property bool passing: false
    readonly property bool hovering: hover.hovered
    // Where what it is about stands, in the parent's coordinates.
    property rect around: Qt.rect(0, 0, 0, 0)
    property string noted: ""
    property var suggestions: []
    readonly property string key: reader.marks.suggestionKey(thing ? thing.id : 0, words)
    property int current: 0
    readonly property bool known: !!thing && !!thing.id
    readonly property real rowHeight: win.touch ? 48 : 34

    signal showInGraph(var id)
    signal addToGraph(string name)
    signal askAbout(string words, bool isThing)
    signal ask(string question, string words)
    signal identify(string words, string paragraph)
    signal find(string words)
    signal copy()
    signal goTo(var met)
    // Put away; `hadFocus` says the keyboard was with it, to go back to the page.
    signal dismissed(bool hadFocus)

    function refreshSuggestions() {
        suggestions = mode === "menu" ? reader.marks.suggestions(known ? thing.id : 0, words).slice(0, 6) : []
    }
    function openCard(found, rect, fromAnswer, passing) {
        mode = "card"
        thing = found
        words = found ? found.name : ""
        paragraph = ""
        menu.fromAnswer = !!fromAnswer
        menu.passing = !!passing
        answered = false
        around = rect
        noted = reader.marks.notedUpTo()
        refreshSuggestions()
        current = 0
        shown = true
        if (!passing) forceActiveFocus()
    }
    function openMenu(chosen) {
        mode = "menu"
        words = chosen.text.trim()
        paragraph = chosen.paragraph || ""
        const named = reader.marks.named(words)
        thing = named.id ? named : null
        fromAnswer = false
        passing = false
        answered = false
        around = chosen.rect
        noted = reader.marks.notedUpTo()
        refreshSuggestions()
        current = 0
        shown = true
        forceActiveFocus()
    }
    function close() {
        if (!shown) return
        const had = activeFocus
        shown = false
        dismissed(had)
    }

    // Each row: what it says and what it does.
    readonly property var actions: {
        translator.language
        const rows = []
        if (mode === "card") {
            rows.push({ id: "graph", text: qsTr("Show in graph") })
            rows.push({ id: "ask", text: qsTr("Ask about this") })
            // The moments of the book it is known from, as far as the reader
            // is: where it was first met, and where what is said of it was.
            if (known) {
                rows.push({ id: "met", text: qsTr("First met: %1").arg(thing.met.title) })
                if (thing.latest) rows.push({ id: "latest", text: qsTr("Latest: %1").arg(thing.latest.title) })
            }
            return rows
        }
        rows.push({ id: "ask", text: qsTr("Ask about this") })
        rows.push({ id: "who", text: qsTr("Who or what is this?") })
        rows.push(known ? { id: "graph", text: qsTr("Show in graph") } : { id: "add", text: qsTr("Add to the graph") })
        rows.push({ id: "find", text: qsTr("Find in book") })
        rows.push({ id: "copy", text: qsTr("Copy") })
        for (const one of suggestions) rows.push({ id: "question", text: one.text, question: true })
        if (assistant.available && !reader.marks.suggested(key) && !assistant.suggesting)
            rows.push({ id: "more", text: suggestions.length > 0 ? qsTr("More suggestions") : qsTr("Suggest questions"), question: true })
        return rows
    }
    function activate(index) {
        const row = actions[index]
        if (!row) return
        if (row.id === "graph") { showInGraph(thing.id); close() }
        else if (row.id === "add") { addToGraph(words); close() }
        else if (row.id === "ask") { askAbout(known && mode === "card" ? thing.name : words, mode === "card"); close() }
        else if (row.id === "who") {
            // Answered from the graph when it knows the name, with no AI;
            // otherwise one short question with the words and their paragraph.
            if (known) answered = true
            else { identify(words, paragraph); close() }
        }
        else if (row.id === "find") { find(words); close() }
        else if (row.id === "copy") { copy(); close() }
        else if (row.id === "met" || row.id === "latest") {
            // To the very words, lit for a moment on the page.
            const moment = row.id === "met" ? thing.met : thing.latest
            close()
            reader.marks.goToMoment(moment.section, moment.fraction, moment.anchor || "")
        }
        else if (row.id === "question") { ask(row.text, words); close() }
        else if (row.id === "more") assistant.suggestMore(key, known ? thing.id : 0, words, paragraph)
    }

    Connections {
        target: reader.marks
        function onSuggestionsChanged() { if (menu.shown) menu.refreshSuggestions() }
    }

    visible: shown
    z: 8
    width: Math.min(360, parent.width - 16)
    height: frame.height
    x: Math.max(8, Math.min(parent.width - width - 8, around.x))
    y: {
        const below = around.y + around.height + 6
        const above = around.y - height - 6
        const fits = below + height <= parent.height - 8
        return Math.max(8, Math.min(parent.height - height - 8, fits || above < 8 ? below : above))
    }

    Keys.onEscapePressed: close()
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Down || event.key === Qt.Key_J) current = Math.min(actions.length - 1, current + 1)
        else if (event.key === Qt.Key_Up || event.key === Qt.Key_K) current = Math.max(0, current - 1)
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) activate(current)
        else return
        event.accepted = true
    }

    GraphWords { id: kindWords }
    HoverHandler { id: hover }
    // A click that falls on the card is the card's, not the page's under it.
    TapHandler { gesturePolicy: TapHandler.WithinBounds }

    Rectangle {
        id: frame
        width: parent.width
        height: body.implicitHeight + 16
        radius: theme.radius
        color: theme.background
        border.color: theme.colors.selection

        ColumnLayout {
            id: body
            x: 8
            y: 8
            width: parent.width - 16
            spacing: 2
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            // The thing: its kind's mark, its name and kind.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 6
                Layout.rightMargin: 6
                Layout.topMargin: 4
                spacing: 8
                visible: menu.known
                GraphKind {
                    size: 16
                    kind: menu.known ? menu.thing.kind : ""
                    Layout.alignment: Qt.AlignVCenter
                }
                Label {
                    objectName: "readingMenuName"
                    Layout.fillWidth: true
                    text: menu.known ? menu.thing.name : ""
                    font.pixelSize: 15
                    font.bold: true
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
                Label {
                    objectName: "readingMenuKind"
                    text: menu.known ? kindWords.kind(menu.thing.kind) : ""
                    color: theme.colors.muted
                    font.pixelSize: 12
                    textFormat: Text.PlainText
                }
            }
            // Words the graph doesn't know: the words themselves.
            Label {
                objectName: "readingMenuWords"
                Layout.fillWidth: true
                Layout.leftMargin: 6
                Layout.rightMargin: 6
                Layout.topMargin: 4
                visible: !menu.known && menu.words !== ""
                text: qsTr("“%1”").arg(menu.words.replace(/\s+/g, " "))
                font.italic: true
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
            Label {
                objectName: "readingMenuSummary"
                Layout.fillWidth: true
                Layout.leftMargin: 6
                Layout.rightMargin: 6
                visible: menu.known && text !== ""
                text: menu.known ? menu.thing.summary : ""
                wrapMode: Text.Wrap
                maximumLineCount: menu.answered ? 8 : 4
                elide: Text.ElideRight
                lineHeight: 1.2
                textFormat: Text.PlainText
            }
            // Answered from the graph: how it relates, and where it was first met.
            Repeater {
                model: menu.answered && menu.known ? menu.thing.relations : []
                delegate: Label {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: 6
                    Layout.rightMargin: 6
                    text: (modelData.outgoing ? "%1 → %2" : "%2 → %1").arg(modelData.relation).arg(modelData.other)
                    color: theme.colors.muted
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
            }
            Label {
                objectName: "readingMenuMet"
                Layout.fillWidth: true
                Layout.leftMargin: 6
                Layout.rightMargin: 6
                visible: menu.answered && menu.known
                text: menu.known ? qsTr("First met in %1").arg(menu.thing.met.title) : ""
                color: theme.colors.muted
                font.pixelSize: 12
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
            // Honest about how far the graph got, when it is behind the reader.
            Label {
                objectName: "readingMenuNoted"
                Layout.fillWidth: true
                Layout.leftMargin: 6
                Layout.rightMargin: 6
                visible: menu.noted !== ""
                text: menu.noted
                color: theme.colors.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            Item { Layout.preferredHeight: 4; visible: menu.known || menu.words !== "" }

            Repeater {
                model: menu.actions
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    objectName: "readingAction_" + modelData.id
                    Layout.fillWidth: true
                    // A question may take two lines rather than be cut short.
                    Layout.preferredHeight: Math.max(menu.rowHeight, caption.implicitHeight + 12)
                    radius: Math.min(4, theme.radius)
                    color: menu.current === index && menu.activeFocus || tap.pressed || rowHover.hovered ? theme.colors.selection : "transparent"
                    // The questions stand apart from what is done with the words.
                    Rectangle {
                        visible: !!row.modelData.question && (row.index === 0 || !menu.actions[row.index - 1].question)
                        width: parent.width
                        height: 1
                        y: -1
                        color: theme.colors.selection
                    }
                    Label {
                        id: caption
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        text: row.modelData.text
                        color: row.modelData.id === "more" ? theme.colors.muted : theme.foreground
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }
                    HoverHandler { id: rowHover; onHoveredChanged: if (hovered) menu.current = row.index }
                    TapHandler { id: tap; onTapped: menu.activate(row.index) }
                }
            }
            Thinking {
                objectName: "readingMenuThinking"
                Layout.leftMargin: 8
                Layout.preferredHeight: menu.rowHeight
                active: assistant.suggesting && menu.mode === "menu"
                text: qsTr("Thinking of questions…")
                room: body.width - 16
            }
        }
    }
}
