import QtQuick
import QtQuick.Controls
import Omabook

// How many books the shelf shows, and beside it, quietly, how far the
// year's reading goal is (#68): a number to look at when one wants to,
// in the shelf's muted words, never a reminder. It opens the statistics.
// A Row, not a layout: hidden in a header with no room for it, it takes
// none of the header's.
Row {
    id: shelfCount
    // Whether the shelf shows only some of its books (a search, a filter).
    property bool some: false
    // As tall as what is tapped beside it, where fingers tap.
    readonly property real lineHeight: Math.max(books.implicitHeight, win.touch ? 40 : 0)
    spacing: 6

    ReadingStats { id: stats; source: backend }

    Label {
        id: books
        objectName: "bookCount"
        height: shelfCount.lineHeight
        verticalAlignment: Text.AlignVCenter
        text: !shelfCount.some ? qsTr("%Ln books", "", backend.books.total)
              : qsTr("%1 of %2").arg(backend.books.books).arg(backend.books.total)
        color: theme.colors.muted
    }
    Label {
        objectName: "shelfGoal"
        visible: stats.goal.kind !== undefined
        width: Math.min(implicitWidth, 220)
        height: shelfCount.lineHeight
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        text: stats.goal.kind === "hours"
              ? qsTr("· %1 of %Ln hours this year", "", stats.goal.target || 0).arg(Number(Math.floor(stats.goal.done || 0)).toLocaleString(Qt.locale(), "f", 0))
              : qsTr("· %1 of %Ln books this year", "", stats.goal.target || 0).arg(Number(stats.goal.done || 0).toLocaleString(Qt.locale(), "f", 0))
        color: theme.colors.muted
        readonly property string runs: "library.stats"
        TapHandler { onTapped: win.run("library.stats") }
    }
}
