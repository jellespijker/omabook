import QtQuick
import QtQuick.Controls

// Any QML item. It sees what a view sees (backend, reader, theme, settings,
// custom), and what its script set with omabook.ui.set.
ListView {
    id: panel
    readonly property var mine: backend.scripts.data["{{name}}"] || ({})
    model: mine.reading || []
    clip: true
    spacing: 4
    topMargin: 12

    header: Label {
        text: "Reading now"
        font.bold: true
        leftPadding: 12
        bottomPadding: 8
    }
    delegate: ItemDelegate {
        id: row
        required property var modelData
        width: panel.width
        text: row.modelData.title + "  ·  " + row.modelData.percent + "%"
        onClicked: reader.openBook(row.modelData.id)
    }
}
