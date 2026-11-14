import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Find words anywhere in the book and go to where they are.
AppPopup {
    id: popup
    objectName: "finder"
    widest: 560
    tallest: 600
    property var results: []
    property bool searched: false

    function go(result) {
        reader.goToPlace(result.section, result.fraction)
        close()
    }

    onOpened: {
        words.selectAll()
        words.forceActiveFocus()
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
            placeholderText: qsTr("Find in this book")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: {
                popup.results = reader.find(text)
                popup.searched = true
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
                height: column.implicitHeight + 14
                radius: Math.min(4, theme.radius)
                color: list.currentIndex === index ? theme.colors.selection : "transparent"

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
