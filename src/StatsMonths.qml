import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A year's reading by month (#68): one bar a month, of the time read or of
// the books finished, one measure at a time on one axis, and the same
// numbers as a table under it, for whoever doesn't read them off a bar.
// The bars are in one colour that stands out from the background in a
// light theme and a dark one (ReadingStats::mark), each a little apart from
// the next, rounded at its top and standing on the baseline.
ColumnLayout {
    id: months
    objectName: "statsMonths"
    property var summary: ({})
    property string measure: "time"
    property color markColor: theme.accent
    property var duration: seconds => String(seconds)
    signal measureChosen(string what)
    spacing: 8

    readonly property var list: Array.from(summary.months || [])
    // The cells of a table, a row of them for each of `rows`.
    function cells(rows, row) {
        const out = []
        for (let i = 0; i < rows.length; ++i)
            out.push(...row(rows[i]))
        return out
    }
    function valueOf(month) { return measure === "time" ? month.seconds / 3600 : month.finished }
    readonly property real most: Math.max(1, ...list.map(month => valueOf(month)))
    function said(month) { return measure === "time" ? duration(month.seconds) : qsTr("%Ln books", "", month.finished) }
    function monthName(month) { return Qt.locale().standaloneMonthName(month - 1, Locale.ShortFormat) }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: months.measure === "time" ? qsTr("Time read, by month") : qsTr("Books finished, by month")
            font.bold: true
            elide: Text.ElideRight
        }
        AppButton {
            objectName: "statsMeasureTime"
            text: qsTr("Time")
            primary: months.measure === "time"
            onClicked: months.measureChosen("time")
        }
        AppButton {
            objectName: "statsMeasureFinished"
            text: qsTr("Finished")
            primary: months.measure === "finished"
            onClicked: months.measureChosen("finished")
        }
    }

    Item {
        objectName: "statsChart"
        Layout.fillWidth: true
        implicitHeight: 160
        // The baseline, recessive, and the bars on it.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: bars.height
            height: 1
            color: theme.colors.selection
        }
        Row {
            id: bars
            width: parent.width
            height: parent.height - 22
            spacing: 2
            LayoutMirroring.enabled: win.rightToLeft
            Repeater {
                model: months.list
                delegate: Item {
                    id: column
                    width: (bars.width - 11 * bars.spacing) / 12
                    height: bars.height
                    readonly property real value: months.valueOf(modelData)
                    Rectangle {
                        objectName: "statsBar"
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.min(28, parent.width)
                        height: column.value > 0 ? Math.max(2, parent.height * column.value / months.most) : 0
                        color: hover.hovered ? Qt.lighter(months.markColor, theme.light ? 0.85 : 1.15) : months.markColor
                        topLeftRadius: 4
                        topRightRadius: 4
                    }
                    HoverHandler { id: hover }
                    ToolTip.visible: hover.hovered
                    ToolTip.text: months.monthName(modelData.month) + ": " + months.said(modelData)
                    Label {
                        anchors.top: parent.bottom
                        anchors.topMargin: 4
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: months.monthName(modelData.month).slice(0, parent.width < 26 ? 1 : 3)
                        color: theme.colors.muted
                        font.pixelSize: 11
                        elide: Text.ElideNone
                    }
                }
            }
        }
    }

    // The same, as a table: each month read in, with its time, days and books.
    GridLayout {
        objectName: "statsMonthTable"
        Layout.fillWidth: true
        Layout.topMargin: 8
        columns: 4
        columnSpacing: 16
        rowSpacing: 2
        Label { text: qsTr("Month"); color: theme.colors.muted }
        Label { text: qsTr("Time"); color: theme.colors.muted }
        Label { text: qsTr("Days"); color: theme.colors.muted }
        Label { Layout.fillWidth: true; text: qsTr("Finished"); color: theme.colors.muted }
        Repeater {
            model: months.cells(months.list.filter(month => month.days > 0 || month.finished > 0),
                                month => [months.monthName(month.month), months.duration(month.seconds), String(month.days), String(month.finished)])
            delegate: Label { Layout.fillWidth: index % 4 === 3; text: modelData; elide: Text.ElideRight }
        }
    }
    // Every year read in, when there is more than this one.
    GridLayout {
        objectName: "statsYearTable"
        visible: (months.summary.years || []).length > 1
        Layout.fillWidth: true
        Layout.topMargin: 8
        columns: 4
        columnSpacing: 16
        rowSpacing: 2
        Label { text: qsTr("Year"); color: theme.colors.muted }
        Label { text: qsTr("Time"); color: theme.colors.muted }
        Label { text: qsTr("Days"); color: theme.colors.muted }
        Label { Layout.fillWidth: true; text: qsTr("Finished"); color: theme.colors.muted }
        Repeater {
            model: months.cells(Array.from(months.summary.years || []),
                                year => [String(year.year), months.duration(year.seconds), String(year.days), String(year.finished)])
            delegate: Label { Layout.fillWidth: index % 4 === 3; text: modelData; elide: Text.ElideRight }
        }
    }
}
