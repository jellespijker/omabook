import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The library diff (#185, #183) from a snapshot to now or to another one,
// as the library is seen: the books added, removed and changed, detail by
// detail, and how many annotations and things of the knowledge graph. A
// book changed since can be put back as the snapshot has it, after showing
// what that changes and asking.
ColumnLayout {
    id: page
    required property SnapshotWords words
    // The snapshot it is from.
    property var from: ({})
    readonly property var diff: snapshotDesk.diff
    readonly property bool shown: !!diff.from && !!from.id && diff.from.id === from.id
    readonly property var books: shown ? diff.books : ({ added: [], removed: [], changed: [] })
    readonly property bool nothing: shown && books.added.length === 0 && books.removed.length === 0 && books.changed.length === 0
                                    && graphSaid === "" && notesSaid === ""
    readonly property string notesSaid: shown ? words.counts(diff.annotations) : ""
    readonly property string graphSaid: shown ? words.counts(diff.graph) : ""
    // What it can be compared with: now, or another snapshot.
    readonly property var others: snapshotDesk.snapshots.filter(snapshot => snapshot.id !== from.id)
    spacing: 8

    component Heading: Label {
        Layout.fillWidth: true
        Layout.topMargin: 8
        font.bold: true
        wrapMode: Text.Wrap
    }

    Label {
        objectName: "snapshotDiffFrom"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("From the snapshot of %1").arg(page.words.named(page.from))
    }
    GridLayout {
        Layout.fillWidth: true
        columns: win.compact ? 1 : 2
        columnSpacing: 8
        Label { text: qsTr("To") }
        ComboBox {
            id: to
            objectName: "snapshotDiffTo"
            Layout.fillWidth: true
            model: [qsTr("Now")].concat(page.others.map(snapshot => page.words.named(snapshot)))
            Material.roundedScale: theme.radius
            // Back to now whenever another snapshot is compared.
            Connections {
                target: page
                function onFromChanged() { to.currentIndex = 0 }
            }
            onActivated: (index) => snapshotDesk.compare(page.from.id, index === 0 ? "" : page.others[index - 1].id)
        }
    }
    Label {
        objectName: "snapshotDiffNothing"
        visible: page.nothing
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Nothing changed.")
    }

    Heading {
        visible: page.books.added.length > 0
        text: qsTr("%n books added", "", page.books.added.length)
    }
    Repeater {
        model: page.books.added
        SnapshotBookChange {
            objectName: "snapshotBookAdded"
            Layout.fillWidth: true
            words: page.words
        }
    }
    Heading {
        visible: page.books.removed.length > 0
        text: qsTr("%n books removed", "", page.books.removed.length)
    }
    Label {
        visible: page.books.removed.length > 0 && page.shown && !page.diff.to.id
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        text: qsTr("A book removed since comes back with the whole library, restored into a new folder.")
    }
    Repeater {
        model: page.books.removed
        SnapshotBookChange {
            objectName: "snapshotBookRemoved"
            Layout.fillWidth: true
            words: page.words
        }
    }
    Heading {
        visible: page.books.changed.length > 0
        text: qsTr("%n books changed", "", page.books.changed.length)
    }
    Repeater {
        model: page.books.changed
        SnapshotBookChange {
            objectName: "snapshotBookChanged"
            Layout.fillWidth: true
            words: page.words
            snapshot: page.from
            changed: true
        }
    }
    Heading {
        visible: page.notesSaid !== ""
        text: qsTr("Annotations")
    }
    Label {
        objectName: "snapshotDiffNotes"
        visible: page.notesSaid !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: page.notesSaid
    }
    Heading {
        visible: page.graphSaid !== ""
        text: qsTr("Knowledge graph")
    }
    Label {
        objectName: "snapshotDiffGraph"
        visible: page.graphSaid !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: page.graphSaid
    }
}
