import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The book's index beside the page: its chapters as a tree, the one being
// read marked, and the reader's bookmarks under them. A click goes there
// and the index stays where it is. Its other side is the book's highlights
// (#81, AnnotationsList).
Item {
    id: pane
    signal done()
    // "contents" or "annotations": which of the two it shows.
    property string tab: "contents"
    // The reading view, for the words selected on its page.
    property Item host
    // While the index has the keyboard, the arrow keys walk it, not the book.
    readonly property bool focused: list.activeFocus || annotations.focused

    // Chapters whose parts are folded away, by their place in the contents.
    property var folded: ({})
    // The entry being read: the first one of the chapter the reader is in.
    readonly property int current: {
        let at = -1, best = -1
        for (let i = 0; i < reader.toc.length; ++i)
            if (reader.toc[i].section <= reader.section && reader.toc[i].section > best) {
                best = reader.toc[i].section
                at = i
            }
        return at
    }
    // What is shown: every entry not inside a folded one, then the bookmarks.
    readonly property var rows: {
        const rows = []
        let hiddenBelow = -1
        for (let i = 0; i < reader.toc.length; ++i) {
            const entry = reader.toc[i]
            if (hiddenBelow >= 0 && entry.level > hiddenBelow) continue
            hiddenBelow = -1
            const parent = i + 1 < reader.toc.length && reader.toc[i + 1].level > entry.level
            if (parent && folded[i]) hiddenBelow = entry.level
            rows.push({ at: i, title: entry.title, level: entry.level, section: entry.section, anchor: entry.anchor,
                        parent: parent, folded: !!folded[i], bookmark: 0 })
        }
        for (const mark of reader.bookmarks)
            rows.push({ at: -1, title: mark.text, level: 0, section: mark.section, fraction: mark.fraction,
                        parent: false, folded: false, bookmark: mark.id })
        return rows
    }

    function takeFocus() {
        if (tab === "annotations") return annotations.takeFocus()
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].at === current) list.currentIndex = i
        list.positionViewAtIndex(Math.max(0, list.currentIndex), ListView.Center)
        list.forceActiveFocus()
    }
    function go(row) {
        if (row.bookmark > 0) reader.goToPlace(row.section, row.fraction)
        else reader.goTo(row.section, row.anchor)
    }
    function fold(row, yes) {
        if (!row.parent) return
        const next = Object.assign({}, folded)
        next[row.at] = yes
        folded = next
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: theme.colors.selection
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: [{ tab: "contents", text: qsTr("Contents"), runs: "reader.contents" },
                        { tab: "annotations", text: qsTr("Highlights"), runs: "reader.annotations" }]
                delegate: Item {
                    id: tabItem
                    required property var modelData
                    objectName: "paneTab_" + modelData.tab
                    readonly property string runs: modelData.runs
                    readonly property bool chosen: pane.tab === modelData.tab
                    // A tab that doesn't fill would be fixed at its preferred
                    // width, and the row, then the whole pane, wider than the
                    // pane under fingers, where Close is there too (#122):
                    // it fills up to its word, and narrows and elides below it.
                    Layout.fillWidth: true
                    // A fingertip wide at the least, at the narrowest pane too.
                    Layout.minimumWidth: 40
                    Layout.preferredWidth: tabLabel.implicitWidth + 12
                    Layout.maximumWidth: tabLabel.implicitWidth + 12
                    Layout.preferredHeight: win.touch ? 44 : 30
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.text
                    Label {
                        id: tabLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        leftPadding: 6
                        rightPadding: 6
                        text: tabItem.modelData.text
                        font.pixelSize: 16
                        font.bold: tabItem.chosen
                        color: tabItem.chosen ? theme.foreground : theme.colors.muted
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        visible: tabItem.chosen
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 2
                        color: theme.accent
                    }
                    TapHandler {
                        onTapped: {
                            pane.tab = tabItem.modelData.tab
                            pane.takeFocus()
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "contentsClose"
                visible: win.touch
                focusPolicy: Qt.NoFocus
                text: qsTr("Close")
                onClicked: pane.done()
            }
        }
        AnnotationsList {
            id: annotations
            objectName: "annotationsList"
            visible: pane.tab === "annotations"
            host: pane.host
            Layout.fillWidth: true
            Layout.fillHeight: true
            onDone: pane.done()
        }
        Label {
            visible: list.count === 0 && pane.tab === "contents"
            Layout.fillWidth: true
            text: qsTr("This book has no table of contents.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        ListView {
            id: list
            objectName: "contentsList"
            visible: pane.tab === "contents"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: pane.rows
            ScrollBar.vertical: ScrollBar {}
            Keys.onReturnPressed: if (currentIndex >= 0) pane.go(pane.rows[currentIndex])
            Keys.onEnterPressed: if (currentIndex >= 0) pane.go(pane.rows[currentIndex])
            Keys.onLeftPressed: if (currentIndex >= 0) pane.fold(pane.rows[currentIndex], true)
            Keys.onRightPressed: if (currentIndex >= 0) pane.fold(pane.rows[currentIndex], false)
            Keys.onEscapePressed: pane.done()
            Keys.onDeletePressed: {
                if (currentIndex >= 0 && pane.rows[currentIndex].bookmark > 0)
                    reader.removeBookmark(pane.rows[currentIndex].bookmark)
            }

            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                readonly property bool reading: modelData.at >= 0 && modelData.at === pane.current
                width: list.width
                height: Math.max(win.touch ? 44 : 32, label.implicitHeight + 12)
                radius: Math.min(4, theme.radius)
                color: list.currentIndex === index && list.activeFocus ? theme.colors.selection : "transparent"

                // The chevron of a chapter that has parts folds and unfolds them.
                // It is as wide as a fingertip under fingers.
                Item {
                    id: chevron
                    readonly property real side: win.touch ? 36 : 20
                    visible: row.modelData.parent
                    x: 2 + row.modelData.level * 16
                    width: side
                    height: parent.height
                    Label {
                        anchors.centerIn: parent
                        text: row.modelData.folded ? "▸" : "▾"
                        color: theme.colors.muted
                    }
                    TapHandler { onTapped: pane.fold(row.modelData, !row.modelData.folded) }
                }
                Label {
                    id: label
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 4 + chevron.side + row.modelData.level * 16
                    anchors.rightMargin: unmark.visible ? unmark.width + 4 : 8
                    text: row.modelData.bookmark > 0 ? "★ " + row.modelData.title : row.modelData.title
                    color: row.reading || row.modelData.bookmark > 0 ? theme.accent : theme.foreground
                    font.bold: row.reading
                    elide: Text.ElideRight
                }
                TapHandler {
                    onTapped: {
                        list.currentIndex = row.index
                        pane.go(row.modelData)
                    }
                }
                HoverHandler { id: rowHover }
                // A bookmark is taken away here, as Delete does: shown under
                // a pointer that rests on it, and always under fingers.
                Item {
                    id: unmark
                    objectName: "removeBookmark"
                    readonly property real side: win.touch ? 36 : 24
                    visible: row.modelData.bookmark > 0 && (win.touch || rowHover.hovered)
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: side
                    height: Math.min(parent.height, side)
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Remove the selected bookmark")
                    Glyph {
                        anchors.centerIn: parent
                        name: "cross"
                        size: 12
                        color: unmarkHover.hovered ? theme.foreground : theme.colors.muted
                    }
                    HoverHandler { id: unmarkHover }
                    TapHandler { onTapped: reader.removeBookmark(row.modelData.bookmark) }
                }
            }
        }
    }
}
