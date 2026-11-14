import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The book's index beside the page: its chapters as a tree, the one being
// read marked, and the reader's bookmarks under them. A click goes there
// and the index stays where it is.
Item {
    id: pane
    signal done()
    // While the index has the keyboard, the arrow keys walk it, not the book.
    readonly property bool focused: list.activeFocus

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
            Label {
                Layout.fillWidth: true
                text: qsTr("Contents")
                font.pixelSize: 16
                font.bold: true
                elide: Text.ElideRight
            }
            AppButton {
                visible: win.touch
                focusPolicy: Qt.NoFocus
                text: qsTr("Close")
                onClicked: pane.done()
            }
        }
        Label {
            visible: list.count === 0
            Layout.fillWidth: true
            text: qsTr("This book has no table of contents.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        ListView {
            id: list
            objectName: "contentsList"
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
                    anchors.rightMargin: 8
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
            }
        }
    }
}
