import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The AI page of Settings (#144, ADR 0008): a row per thing Omabook asks a
// model for (answers, quick work, handwriting, speech to text, text to
// speech, the meaning index), each with its provider and model, whether it
// may use someone else's computer, and, when it can't be used, why and what
// to do. A row opens to its choices, one at a time so the page fits a small
// window; Up and Down (or J and K) go from row to row, Enter opens one.
AppPopup {
    id: dialog
    objectName: "aiSettings"
    widest: 680
    tallest: content.implicitHeight + 2 * padding

    readonly property var desk: aiAgent.capabilities
    // The row that is open, by its capability.
    property string current: ""

    function show(capability) {
        current = current === capability ? "" : capability
        settle.restart()
    }
    function focusRow(index) {
        const row = rows.itemAt(Math.max(0, Math.min(rows.count - 1, index)))
        if (row) row.head.forceActiveFocus()
    }

    onAboutToShow: desk.watch(true)
    onOpened: {
        // What needs doing first is opened: the first that can't be used,
        // unless that is only because the AI is off.
        const first = desk.rows.findIndex(row => !row.available && row.why !== "ai-off")
        current = first >= 0 ? desk.rows[first].capability : ""
        focusRow(Math.max(0, first))
        settle.restart()
    }
    onClosed: desk.watch(false)

    // Brings the open row into view once it has its size.
    Timer {
        id: settle
        interval: 0
        onTriggered: {
            const index = dialog.desk.rows.findIndex(row => row.capability === dialog.current)
            const row = index >= 0 ? rows.itemAt(index) : null
            if (!row) return
            const top = row.mapToItem(content, 0, 0).y
            if (top < flick.contentY || top + row.height > flick.contentY + flick.height)
                flick.contentY = Math.max(0, Math.min(top, flick.contentHeight - flick.height))
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded }

        ColumnLayout {
            id: content
            // Room for the scroll bar whether it shows or not.
            width: flick.width - 12
            spacing: 10
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label { text: qsTr("AI"); font.pixelSize: 18; font.bold: true }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.muted
                text: qsTr("Each thing Omabook asks a model for has a provider of its own. Nothing goes to someone else's computer unless you allow it for that one.")
            }
            // Off stops every model, every sending and every analysis.
            Switch {
                objectName: "aiOn"
                Layout.fillWidth: true
                // Tall enough for a fingertip.
                Layout.minimumHeight: 40
                text: qsTr("Use AI")
                checked: dialog.desk.aiOn
                Accessible.name: text
                onToggled: dialog.desk.setAiOn(checked)
            }
            Label {
                objectName: "aiOffNote"
                Layout.fillWidth: true
                visible: !dialog.desk.aiOn
                wrapMode: Text.Wrap
                color: theme.colors.muted
                text: qsTr("Off: no model is asked anything, nothing is sent, and nothing is read for meaning, handwriting or speech. A voice on this computer still reads aloud.")
            }

            Repeater {
                id: rows
                model: dialog.desk.rows.length

                AiCapabilityRow {
                    required property int index
                    Layout.fillWidth: true
                    info: dialog.desk.rows[index] || ({})
                    open: dialog.current === info.capability
                    onToggled: dialog.show(info.capability)
                    onStep: (by) => dialog.focusRow(index + by)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Item { Layout.fillWidth: true }
                AppButton { objectName: "aiSettingsClose"; text: qsTr("Close"); onClicked: dialog.close() }
            }
        }
    }
}
