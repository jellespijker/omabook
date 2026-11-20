import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A stretch of story time to show, from one year or date to another, as
// the books write them ("1939", "1944-06-06", "3019-03"), on the calendar
// of each band. Either end may be left open; a band with no dates has no
// time in it to show, and is left out while a stretch is asked for.
AppPopup {
    id: range
    objectName: "chronologyRange"
    widest: 460
    property var browser
    property bool wrong: false

    function show() {
        wrong = false
        open()
        from.forceActiveFocus()
    }
    function take() {
        if (range.browser.setRange(from.text, to.text)) close()
        else wrong = true
    }

    DialogBody {
        // A popup is not an Item and doesn't inherit the window's mirroring.
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            Layout.fillWidth: true
            text: qsTr("Show a stretch of time")
            font.pixelSize: 17
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("A year, or a date as year, month and day: 1944, 1944-06 or 1944-06-06. A year before the common era is a minus: -43.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        TextField {
            id: from
            objectName: "chronologyRangeFrom"
            Layout.fillWidth: true
            placeholderText: qsTr("From")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onTextEdited: range.wrong = false
            Keys.onReturnPressed: range.take()
            Keys.onEnterPressed: range.take()
        }
        TextField {
            id: to
            objectName: "chronologyRangeTo"
            Layout.fillWidth: true
            placeholderText: qsTr("To")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onTextEdited: range.wrong = false
            Keys.onReturnPressed: range.take()
            Keys.onEnterPressed: range.take()
        }
        Label {
            Layout.fillWidth: true
            visible: range.wrong
            text: qsTr("That isn't a year or a date.")
            color: theme.colors.red || theme.accent
            wrapMode: Text.Wrap
        }
        actions: [
            AppButton {
                text: qsTr("All of time")
                onClicked: {
                    range.browser.setRange("", "")
                    range.close()
                }
            },
            AppButton {
                primary: true
                text: qsTr("Show")
                onClicked: range.take()
            }
        ]
    }
}
