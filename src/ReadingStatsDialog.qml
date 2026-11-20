import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omabook

// What one read, added up (#68): of a year, the time read, the days read on
// and in a row, the books finished and how much was read, by month, with
// the year's goal and each book's reading. Of the whole library or one
// collection. Worked out here from the library; nothing is sent.
AppPopup {
    id: dialog
    objectName: "readingStats"
    widest: 760
    tallest: 760
    closePolicy: Popup.CloseOnEscape

    readonly property var summary: stats.summary
    readonly property var total: summary.total || ({})
    readonly property bool nothing: (total.days || 0) === 0 && (total.finished || 0) === 0
    // What the chart shows by month: the time read, or the books finished.
    property string measure: "time"

    function show() {
        stats.year = new Date().getFullYear()
        stats.collection = ""
        open()
        stats.refresh()
    }
    // "3 h 05 min", "12 min".
    function duration(seconds) {
        const minutes = Math.round((seconds || 0) / 60)
        return minutes >= 60 ? qsTr("%1 h %2 min").arg(Math.floor(minutes / 60)).arg(String(minutes % 60).padStart(2, "0"))
                             : qsTr("%1 min").arg(minutes)
    }
    function dayText(iso) { return iso ? Qt.formatDate(new Date(iso + "T12:00:00"), Locale.ShortFormat) : "" }
    // The time read in the seven days to today, and today.
    function recent(days) {
        const today = new Date(); today.setHours(12, 0, 0, 0)
        let week = 0, now = 0
        for (const day of Array.from(summary.days || [])) {
            const ago = Math.round((today - new Date(day.day + "T12:00:00")) / 86400000)
            if (ago >= 0 && ago < days) week += day.seconds
            if (ago === 0) now += day.seconds
        }
        return days === 1 ? now : week
    }

    ReadingStats { id: stats; source: backend; active: dialog.visible }

    DialogBody {
        gap: 16

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("Reading in %1").arg(stats.year)
                font.pixelSize: 20
                font.bold: true
                elide: Text.ElideRight
            }
            AppButton { objectName: "statsYearBefore"; text: "‹"; Accessible.name: qsTr("The year before"); onClicked: stats.year = stats.year - 1 }
            AppButton {
                objectName: "statsYearAfter"
                text: "›"
                Accessible.name: qsTr("The year after")
                enabled: stats.year < new Date().getFullYear()
                onClicked: stats.year = stats.year + 1
            }
        }
        ComboBox {
            id: collectionChoice
            objectName: "statsCollection"
            visible: backend.collections.length > 0
            Layout.fillWidth: true
            Layout.maximumWidth: 360
            model: [qsTr("The whole library")].concat(backend.collections.map(c => c.path))
            onActivated: index => stats.collection = index === 0 ? "" : backend.collections[index - 1].path
        }

        Label {
            objectName: "statsNothing"
            visible: dialog.nothing && !stats.working
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("Nothing read in %1 yet. The time is counted while a book is open in the reader.").arg(stats.year)
        }

        // The year's numbers, each a tile: a label over its value.
        Flow {
            id: tiles
            objectName: "statsTiles"
            Layout.fillWidth: true
            spacing: 12
            // As many tiles side by side as are 160 wide, each as wide as the row gives them.
            readonly property int across: Math.max(1, Math.floor((width + spacing) / (160 + spacing)))
            readonly property real tileWidth: Math.floor((width - (across - 1) * spacing) / across)
            Repeater {
                model: [
                    { label: qsTr("Time read"), value: dialog.duration(dialog.total.seconds) },
                    { label: qsTr("Days read on"), value: Number(dialog.total.days || 0).toLocaleString(Qt.locale(), "f", 0) },
                    { label: qsTr("Books finished"), value: Number(dialog.total.finished || 0).toLocaleString(Qt.locale(), "f", 0) },
                    { label: qsTr("Read, in whole books"), value: Number(dialog.total.books || 0).toLocaleString(Qt.locale(), "f", 1) },
                    { label: qsTr("Days in a row"), value: Number((dialog.summary.streaks || {}).current || 0).toLocaleString(Qt.locale(), "f", 0) },
                    { label: qsTr("Most days in a row"), value: Number((dialog.summary.streaks || {}).longest || 0).toLocaleString(Qt.locale(), "f", 0) },
                    { label: qsTr("Today"), value: dialog.duration(dialog.recent(1)) },
                    { label: qsTr("The last seven days"), value: dialog.duration(dialog.recent(7)) }
                ]
                delegate: Rectangle {
                    width: tiles.tileWidth
                    height: tile.implicitHeight + 20
                    radius: theme.radius
                    color: "transparent"
                    border.color: theme.colors.selection
                    Column {
                        id: tile
                        x: 12
                        y: 10
                        width: parent.width - 24
                        spacing: 2
                        Label { width: parent.width; text: modelData.label; color: theme.colors.muted; wrapMode: Text.Wrap }
                        Label { width: parent.width; text: modelData.value; font.pixelSize: 18; font.bold: true; elide: Text.ElideRight }
                    }
                }
            }
        }

        StatsGoal {
            Layout.fillWidth: true
            view: stats
            shown: stats.collection === ""
        }

        StatsMonths {
            Layout.fillWidth: true
            visible: !dialog.nothing
            summary: dialog.summary
            measure: dialog.measure
            markColor: stats.mark(theme.accent, theme.background)
            duration: dialog.duration
            onMeasureChosen: what => dialog.measure = what
        }

        Label {
            visible: (dialog.summary.books || []).length > 0
            text: qsTr("Books")
            font.bold: true
        }
        Repeater {
            model: dialog.summary.books || []
            delegate: ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label { Layout.fillWidth: true; text: modelData.title; elide: Text.ElideRight }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: theme.colors.muted
                    text: [modelData.begun ? qsTr("begun %1").arg(dialog.dayText(modelData.begun)) : "",
                           modelData.finished ? (modelData.finished.length === 10 ? qsTr("finished %1").arg(dialog.dayText(modelData.finished))
                                                                                  : qsTr("finished %1").arg(modelData.finished))
                                              : qsTr("%1% read").arg(Math.round(modelData.percent)),
                           modelData.took_days > 0 ? qsTr("in %Ln days", "", modelData.took_days) : "",
                           modelData.seconds > 0 ? dialog.duration(modelData.seconds) : ""].filter(part => part !== "").join(" · ")
                }
            }
        }
        Label {
            visible: (dialog.summary.undated || 0) > 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("%Ln books read without the day they were finished.", "", dialog.summary.undated || 0)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("Counted here from your library; nothing is sent. The time is counted while a book is open, and the time away from it is left out.")
        }

        actions: [
            AppButton { objectName: "statsClose"; text: qsTr("Close"); onClicked: dialog.close() }
        ]
    }
}
