import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Building the knowledge graph for books after the fact: books read before
// the graph existed, or read elsewhere. It says what it would send before
// anything is sent, and can be stopped between requests.
AppPopup {
    id: dialog
    objectName: "graphBuild"
    widest: 560
    // The work goes on with the dialog closed; only Escape and Close close it.
    closePolicy: Popup.CloseOnEscape

    readonly property var jobs: backend.graphJobs
    readonly property var choices: ["selected", "shown", "read"]
    property var selectedId: 0

    function show(id) {
        selectedId = id
        which.currentIndex = id > 0 ? 0 : 1
        open()
        reconsider()
        which.forceActiveFocus()
    }
    function reconsider() { if (!jobs.busy) jobs.consider(choices[which.currentIndex], selectedId, whole.checked) }

    contentItem: ScrollView {
        id: scroller
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: scroller.availableWidth
            spacing: 12
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label {
                Layout.fillWidth: true
                text: qsTr("Build the knowledge graph")
                font.pixelSize: 18
                font.bold: true
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.muted
                text: qsTr("The model for quick work (its row on the AI page of Settings) reads what you have read of these books and notes the people, places, ideas and plot lines in them. Each book is taken as far as you read it, and nothing is sent twice.")
            }
            ComboBox {
                id: which
                objectName: "graphBuildWhich"
                Layout.fillWidth: true
                enabled: !dialog.jobs.busy
                Material.roundedScale: theme.radius
                model: [qsTr("The selected book"), qsTr("The books the shelf shows now"), qsTr("Every book I have read or am reading")]
                onActivated: dialog.reconsider()
            }
            CheckBox {
                id: whole
                objectName: "graphBuildWhole"
                enabled: !dialog.jobs.busy
                // A check box's caption is one line as it comes, and this
                // one is longer than a narrow dialog: it wraps instead.
                Layout.fillWidth: true
                contentItem: Label {
                    text: whole.text
                    wrapMode: Text.Wrap
                    verticalAlignment: Text.AlignVCenter
                    opacity: whole.enabled ? 1 : 0.5
                    leftPadding: whole.mirrored ? 0 : whole.indicator.width + whole.spacing
                    rightPadding: whole.mirrored ? whole.indicator.width + whole.spacing : 0
                }
                text: qsTr("The whole of each book, also what I haven't read")
                onToggled: dialog.reconsider()
            }
            // What it would take, or how it is going, or how it went.
            Label {
                objectName: "graphBuildState"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: {
                    translator.language
                    const jobs = dialog.jobs
                    if (jobs.busy)
                        return qsTr("Book %1 of %2: %3. %4 things and %5 relations so far.")
                            .arg(jobs.progress.index).arg(jobs.progress.total).arg(jobs.progress.title || "…")
                            .arg(jobs.progress.nodes).arg(jobs.progress.edges)
                    if (jobs.summary.books !== undefined)
                        return (jobs.summary.stopped ? qsTr("Stopped. ") : "")
                            + qsTr("%1 things and %2 relations were added from %Ln books.", "", jobs.summary.books)
                                  .arg(jobs.summary.nodes).arg(jobs.summary.edges)
                    if (jobs.planning)
                        return qsTr("Looking at the books…")
                    if (!jobs.plan.books)
                        return qsTr("There is nothing to do for these books.")
                    return qsTr("%Ln books, in %1 requests to the AI: about %2 tokens.", "", jobs.plan.books)
                        .arg(jobs.plan.requests).arg(Number(jobs.plan.tokens).toLocaleString(Qt.locale(), 'f', 0))
                        + (aiAgent.quickModel !== "" ? " " + qsTr("The reading is done by %1.").arg(aiAgent.quickModel) : "")
                }
            }
            Thinking {
                objectName: "graphBuildThinking"
                active: dialog.jobs.busy || dialog.jobs.planning
                text: dialog.jobs.planning ? qsTr("Looking at the books…") : qsTr("Reading and noting…")
            }
            ProgressBar {
                Layout.fillWidth: true
                visible: dialog.jobs.busy
                from: 0
                to: Math.max(1, dialog.jobs.plan.requests || 1)
                value: dialog.jobs.progress.requests || 0
            }
            // The books left out, and why, so none is left out in silence.
            Repeater {
                model: dialog.jobs.busy ? [] : (dialog.jobs.plan.skipped || []).slice(0, 6)
                Label {
                    required property var modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                    color: theme.colors.muted
                    text: modelData.title + ": " + modelData.why
                }
            }
            Label {
                Layout.fillWidth: true
                visible: !dialog.jobs.busy && (dialog.jobs.plan.skipped || []).length > 6
                font.pixelSize: 12
                color: theme.colors.muted
                text: qsTr("…and %Ln more left out.", "", (dialog.jobs.plan.skipped || []).length - 6)
            }
            Repeater {
                model: dialog.jobs.summary.problems || []
                Label {
                    required property string modelData
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                    color: theme.colors.orange
                    text: modelData
                    textFormat: Text.PlainText
                }
            }
            DialogActions {
                AppButton { text: qsTr("Close"); onClicked: dialog.close() }
                AppButton {
                    objectName: "graphBuildStop"
                    visible: dialog.jobs.busy
                    text: qsTr("Stop")
                    onClicked: dialog.jobs.stop()
                }
                AppButton {
                    objectName: "graphBuildStart"
                    visible: !dialog.jobs.busy
                    primary: true
                    enabled: !dialog.jobs.planning && (dialog.jobs.plan.books || 0) > 0
                    text: qsTr("Build")
                    onClicked: dialog.jobs.start()
                }
            }
        }
    }
}
