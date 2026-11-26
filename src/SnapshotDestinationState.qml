import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// How a destination stands (#185, #182): a second disk, a place away from
// home or another of the person's hubs (#196), its newest snapshot as its last copy left it, or why its last copy
// didn't go through, in red.
Label {
    id: state
    required property var destination
    required property SnapshotWords words
    readonly property var last: destination.last
    readonly property bool wrong: !!last && !destination.copied
    readonly property string where: destination.kind === "disk" ? qsTr("A second disk")
        : destination.kind === "hub" ? qsTr("Another of your hubs, which can't read them")
        : qsTr("Away from home")
    wrapMode: Text.Wrap
    color: wrong ? theme.colors.red : theme.foreground
    text: !last ? qsTr("%1, at %2. Not copied to yet: the snapshots go there every night.").arg(where).arg(destination.location)
        : last.absent ? qsTr("%1 that isn't there since %2: is it plugged in?").arg(where).arg(words.when(last.tried))
        : wrong ? qsTr("%1. Its last copy, at %2, didn't go through: %3").arg(where).arg(words.when(last.tried)).arg(last.said)
        : qsTr("%1. Newest: %2, copied %3.").arg(where).arg(words.when(last.latest)).arg(words.when(last.copied))
}
