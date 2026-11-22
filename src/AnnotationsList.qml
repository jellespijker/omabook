import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The open book's highlights beside the page (#81), in the order of the
// book: each its colour, its words and where it is; a click or Enter goes
// there and lights the words. Above them the colours, by the names the
// reader gave them, to show one colour alone, and those names to change;
// below, the ones that can no longer be found in this edition of the book,
// each placed again on words selected on the page, or removed. What is
// shown is copied as Markdown, to take elsewhere. Under them, on a PDF or a
// comic, the pages with book ink (#83), each the part of the page its ink
// covers, going there and taken away as a highlight is; on a flowing book
// the chapters with ink in their margins and between their lines (#152), each
// with the words of its first line; a highlight on a
// part of a comic's page (#115) shows that part, as small. Above all of them,
// whose annotations are shown beside one's own (OthersAnnotations, #85).
//
// Keys: Up and Down (J, K) walk the list, Enter goes, 1 to 5 colour the one
// selected anew, Delete removes it, Escape leaves.
Item {
    id: list
    // The reading view, for the words selected on its page.
    property Item host
    signal done()
    readonly property bool focused: rows.activeFocus
    // The colour shown alone, or "" for all.
    property string only: ""
    property bool naming: false
    readonly property var shown: reader.highlights.list.filter(one => only === "" || one.color === only)
    readonly property var found: shown.filter(one => !one.lost)
    readonly property var lost: shown.filter(one => one.lost)
    readonly property var inked: only !== "" ? []
                               : reader.kind === "pages" ? reader.ink.pages.map(one => Object.assign({ ink: true, lost: false, color: "", quote: "" }, one))
                               : reader.lineInk.list.map(one => Object.assign({ ink: true, lineInk: true, lost: false, color: "" }, one))
    readonly property var looks: {
        const looks = {}
        for (const color of reader.highlights.colors) looks[color.key] = color
        return looks
    }
    readonly property bool selected: !!host && !!host.content && host.content.selectedText !== ""

    function takeFocus() {
        if (rows.currentIndex < 0 && rows.count > 0) rows.currentIndex = 0
        rows.forceActiveFocus()
    }
    function go(row) {
        if (row.lost) return
        if (row.lineInk) return reader.goToPlace(row.section, row.fraction)
        if (row.ink) return reader.goToPlace(row.page, row.fraction)
        if (reader.kind === "pages") reader.goToPlace(row.section, row.fraction)
        else reader.marks.goToMoment(row.section, row.fraction, row.quote.slice(0, 120))
    }
    function remove(row) {
        if (row.lineInk) reader.lineInk.eraseSection(row.section)
        else if (row.ink) reader.ink.erasePage(row.page)
        else reader.highlights.remove(row.id)
    }
    function count(color) { return reader.highlights.list.filter(one => one.color === color).length }
    function place(row) {
        if (host && host.content && typeof host.content.placeSelection === "function" && host.content.placeSelection(row.id))
            backend.say(qsTr("Placed on the selected words"))
    }

    HighlightWords { id: words }
    PlainCopy { id: copier }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // Whose annotations are shown beside one's own (#85).
        OthersAnnotations { Layout.fillWidth: true }
        // The colours: all, or one alone, each with its name and how many.
        Flow {
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: [""].concat(reader.highlights.colors.map(one => one.key))
                delegate: Rectangle {
                    id: chip
                    required property string modelData
                    objectName: "highlightFilter_" + (modelData === "" ? "all" : modelData)
                    readonly property bool chosen: list.only === modelData
                    width: Math.min(list.width, chipRow.implicitWidth + 16)
                    height: win.touch ? 44 : 28
                    radius: height / 2
                    color: chosen ? theme.colors.selection : "transparent"
                    border.color: theme.colors.selection
                    Accessible.role: Accessible.Button
                    Accessible.name: chipLabel.text
                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 6
                        Rectangle {
                            visible: chip.modelData !== ""
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 12
                            radius: 6
                            color: (list.looks[chip.modelData] || {}).tint || "transparent"
                            border.color: (list.looks[chip.modelData] || {}).line || "transparent"
                            border.width: 1.5
                        }
                        Label {
                            id: chipLabel
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, list.width - 44)
                            elide: Text.ElideRight
                            font.pixelSize: 12
                            // The reader's highlights can be gone for a moment while a book is closed or
                            // swapped; the chip then counts none rather than failing (#161).
                            readonly property int all: reader.highlights && reader.highlights.list ? reader.highlights.list.length : 0
                            text: chip.modelData === "" ? qsTr("All %1").arg(all.toLocaleString(Qt.locale(), 'f', 0))
                                                        : "%1 %2".arg(words.name(chip.modelData)).arg(Number(list.count(chip.modelData) || 0).toLocaleString(Qt.locale(), 'f', 0))
                        }
                    }
                    TapHandler { onTapped: list.only = chip.modelData }
                }
            }
        }
        // What the reader calls each colour.
        AppButton {
            objectName: "nameColors"
            Layout.fillWidth: true
            focusPolicy: Qt.NoFocus
            text: list.naming ? qsTr("Done naming") : qsTr("Name the colours")
            onClicked: list.naming = !list.naming
        }
        // In place of the list while it is open, scrolling in a short pane.
        ListView {
            objectName: "colorNames"
            visible: list.naming
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: list.naming ? reader.highlights.colors : []
            ScrollBar.vertical: ScrollBar {}
            delegate: RowLayout {
                id: naming
                required property var modelData
                width: ListView.view.width
                spacing: 8
                Rectangle {
                    width: 16
                    height: 16
                    radius: 8
                    color: naming.modelData.tint
                    border.color: naming.modelData.line
                    border.width: 1.5
                }
                TextField {
                    objectName: "colorName_" + naming.modelData.key
                    Layout.fillWidth: true
                    placeholderText: words.color(naming.modelData.key)
                    text: settings.highlightNames[naming.modelData.key] || ""
                    Accessible.name: qsTr("Name for %1").arg(words.color(naming.modelData.key))
                    onEditingFinished: settings.setHighlightName(naming.modelData.key, text)
                }
            }
        }

        Label {
            objectName: "noHighlights"
            visible: list.shown.length === 0 && list.inked.length === 0 && !list.naming
            Layout.fillWidth: true
            text: list.only === "" ? qsTr("No highlights yet. Select words and choose a colour, or draw a line under them with the pen.")
                                   : qsTr("No highlights in %1.").arg(words.name(list.only))
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }

        ListView {
            id: rows
            objectName: "highlightsList"
            visible: !list.naming
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: list.found.concat(list.lost).concat(list.inked)
            ScrollBar.vertical: ScrollBar {}
            readonly property var row: currentIndex >= 0 && currentIndex < model.length ? model[currentIndex] : null
            Keys.onReturnPressed: if (row) list.go(row)
            Keys.onEnterPressed: if (row) list.go(row)
            Keys.onDeletePressed: if (row) list.remove(row)
            Keys.onEscapePressed: list.done()
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_J) incrementCurrentIndex()
                else if (event.key === Qt.Key_K) decrementCurrentIndex()
                else if (event.key >= Qt.Key_1 && event.key <= Qt.Key_5 && row && !row.lost && !row.ink)
                    reader.highlights.recolor(row.id, reader.highlights.colors[event.key - Qt.Key_1].key)
                else return
                event.accepted = true
            }

            delegate: Item {
                id: entry
                required property int index
                required property var modelData
                // The first of the lost ones says what they are.
                readonly property bool firstLost: modelData.lost && (index === 0 || !rows.model[index - 1].lost)
                readonly property bool firstInk: !!modelData.ink && (index === 0 || !rows.model[index - 1].ink)
                readonly property real heading: firstLost ? lostHeading.height : firstInk ? inkHeading.height : 0
                width: rows.width
                height: heading + row.height
                Label {
                    id: lostHeading
                    objectName: "lostHeading"
                    visible: entry.firstLost
                    width: parent.width
                    topPadding: 12
                    bottomPadding: 4
                    text: qsTr("Not found in this edition: select their words on the page to place them again.")
                    color: theme.colors.muted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Label {
                    id: inkHeading
                    objectName: "inkHeading"
                    visible: entry.firstInk
                    width: parent.width
                    topPadding: 12
                    bottomPadding: 4
                    text: reader.kind === "flow" ? qsTr("Ink beside the lines") : qsTr("Ink on the pages")
                    color: theme.colors.muted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Rectangle {
                    id: row
                    readonly property int index: entry.index
                    readonly property var modelData: entry.modelData
                    readonly property var look: list.looks[modelData.color] || {}
                    objectName: (modelData.ink ? "inkRow_" : "highlightRow_") + modelData.id
                    y: entry.heading
                    width: rows.width
                    height: Math.max(win.touch ? 56 : 44, body.implicitHeight + 14)
                    radius: Math.min(4, theme.radius)
                    color: rows.currentIndex === index && rows.activeFocus || rowHover.hovered ? theme.colors.selection : "transparent"

                    Rectangle {
                        x: 2
                        y: 6
                        width: 4
                        height: parent.height - 12
                        radius: 2
                        color: row.modelData.ink ? theme.foreground : row.look.line || theme.accent
                        opacity: row.modelData.lost ? 0.4 : 1
                    }
                    ColumnLayout {
                        id: body
                        anchors.left: parent.left
                        anchors.right: tools.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 14
                        anchors.rightMargin: 4
                        spacing: 2
                        // A page's ink, small: the part of the page it covers; or the part a highlight is on (#115).
                        Loader {
                            active: (!!row.modelData.ink && !row.modelData.lineInk) || !!row.modelData.region
                            visible: active
                            Layout.fillWidth: true
                            Layout.preferredHeight: active ? (win.touch ? 72 : 56) : 0
                            sourceComponent: Component { InkThumb { region: row.modelData.region || row.modelData } }
                        }
                        Label {
                            visible: (!row.modelData.ink || !!row.modelData.lineInk) && !row.modelData.region
                            Layout.fillWidth: true
                            text: row.modelData.quote || ""
                            color: row.modelData.lost ? theme.colors.muted : theme.foreground
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                        Label {
                            Layout.fillWidth: true
                            text: {
                                if (row.modelData.lineInk)
                                    return qsTr("%Ln lines with ink", "", row.modelData.lines) + " · " + qsTr("%Ln strokes", "", row.modelData.strokes)
                                if (row.modelData.ink)
                                    return qsTr("Page %1 · %Ln strokes", "", row.modelData.strokes).arg((row.modelData.page + 1).toLocaleString(Qt.locale(), 'f', 0))
                                const where = row.modelData.page > 0 ? qsTr("Page %1").arg(row.modelData.page.toLocaleString(Qt.locale(), 'f', 0))
                                                                     : row.modelData.title
                                return where !== "" ? "%1 · %2".arg(words.name(row.modelData.color)).arg(where) : words.name(row.modelData.color)
                            }
                            color: theme.colors.muted
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                    }
                    TapHandler {
                        onTapped: {
                            rows.currentIndex = row.index
                            list.go(row.modelData)
                        }
                    }
                    HoverHandler { id: rowHover }
                    // Placed again on the words selected (a lost one), and taken away.
                    Row {
                        id: tools
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        AppButton {
                            objectName: "placeHighlight"
                            visible: row.modelData.lost
                            enabled: list.selected
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Place here")
                            Accessible.name: qsTr("Place on the selected words")
                            onClicked: list.place(row.modelData)
                        }
                        Item {
                            objectName: "removeHighlight"
                            readonly property real side: win.touch ? 44 : 28
                            visible: win.touch || rowHover.hovered || row.modelData.lost
                            width: side
                            height: side
                            Accessible.role: Accessible.Button
                            Accessible.name: row.modelData.lineInk ? qsTr("Erase the ink in this chapter")
                                           : row.modelData.ink ? qsTr("Erase the ink on this page") : qsTr("Remove the highlight")
                            Glyph {
                                anchors.centerIn: parent
                                name: "cross"
                                size: 12
                                color: removeHover.hovered ? theme.foreground : theme.colors.muted
                            }
                            HoverHandler { id: removeHover }
                            TapHandler { onTapped: list.remove(row.modelData) }
                        }
                    }
                }
            }
        }

        AppButton {
            objectName: "copyHighlights"
            Layout.fillWidth: true
            visible: list.shown.length > 0 && !list.naming
            focusPolicy: Qt.NoFocus
            text: qsTr("Copy as Markdown")
            onClicked: {
                copier.copyText(reader.highlights.exported(list.only, words.names()))
                backend.say(qsTr("%Ln highlights copied", "", list.shown.length))
            }
        }
    }
}
