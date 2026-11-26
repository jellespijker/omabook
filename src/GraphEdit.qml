import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Changing the graph by hand, a little: renaming a thing or rewriting what
// is said of it, adding one (related to the selected thing), and the three
// questions asked before anything is taken away: removing a thing, removing
// a relation, folding one thing into another.
AppPopup {
    id: sheet
    widest: 488
    objectName: "graphEdit"
    property var browser
    // "edit", "add", "remove", "unlink" or "merge".
    property string what: "edit"
    // The node it is about, as GraphBrowser.details gives it.
    property var node: ({})
    // For "merge" the node folded into it; for "unlink" the relation.
    property var other: ({})
    property string problem: ""
    // The kinds a thing added by hand can be; a book, an author, a series
    // or a tag comes from the library.
    readonly property var kinds: ["character", "group", "place", "item", "concept", "event", "thread", "theme", "topic", "question"]
    readonly property bool asking: what === "remove" || what === "unlink" || what === "merge"

    // `named` is a name to start an added thing with: words selected on the page.
    function show(mode, node, other, named) {
        what = mode
        sheet.node = node || ({})
        sheet.other = other || ({})
        problem = ""
        name.text = mode === "edit" ? sheet.node.name || "" : named || ""
        summary.text = mode === "edit" ? sheet.node.summary || "" : ""
        relation.text = ""
        open()
        if (asking) confirm.forceActiveFocus()
        else name.forceActiveFocus()
    }
    function save() {
        if (name.text.trim() === "") {
            problem = qsTr("It needs a name.")
            return
        }
        if (what === "edit") {
            // What wasn't changed here stays as it is by now: a build or an
            // agent may have written it since (#223).
            const now = browser.selected === node.id ? browser.details : node
            if (!browser.edit(node.id, name.text === (node.name || "") ? now.name : name.text,
                              summary.text === (node.summary || "") ? now.summary || "" : summary.text)) {
                problem = qsTr("Something else of this kind is already called that. Merge the two instead.")
                return
            }
        } else if (browser.add(kinds[kind.currentIndex], name.text, summary.text, relation.text) === 0) {
            problem = qsTr("That could not be added.")
            return
        }
        close()
    }
    function agree() {
        if (what === "remove") browser.remove(node.id)
        else if (what === "unlink") browser.unlink(other.edge)
        else if (what === "merge") browser.merge(node.id, other.id)
        close()
    }

    GraphWords { id: words }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            objectName: "graphEditHeading"
            Layout.fillWidth: true
            font.pixelSize: 18
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: sheet.what === "remove" ? qsTr("Remove “%1” from the graph?").arg(sheet.node.name || "")
                : sheet.what === "unlink" ? qsTr("Remove this relation?")
                : sheet.what === "merge" ? qsTr("Fold “%1” into “%2”?").arg(sheet.other.label || "").arg(sheet.node.name || "")
                : sheet.what === "add" ? qsTr("Add to the graph")
                : words.kind(sheet.node.kind || "")
        }
        Label {
            Layout.fillWidth: true
            visible: sheet.asking
            color: theme.colors.muted
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: sheet.what === "remove" ? qsTr("Its relations go with it. No book is touched.")
                : sheet.what === "unlink"
                  ? (sheet.other.outgoing ? "%1 · %2 · %3" : "%3 · %2 · %1").arg(sheet.node.name || "").arg(sheet.other.relation || "")
                                                                         .arg(sheet.other.otherName || "")
                : qsTr("They are one and the same: its relations, the places it was met and its name move over.")
        }

        ComboBox {
            id: kind
            objectName: "graphEditKind"
            visible: sheet.what === "add"
            Layout.fillWidth: true
            Material.roundedScale: theme.radius
            model: sheet.kinds.map(kind => words.kind(kind))
        }
        TextField {
            id: name
            objectName: "graphEditName"
            visible: !sheet.asking
            Layout.fillWidth: true
            placeholderText: qsTr("Name")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: sheet.save()
        }
        TextField {
            id: summary
            objectName: "graphEditSummary"
            visible: !sheet.asking
            Layout.fillWidth: true
            placeholderText: qsTr("What is known of it")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: sheet.save()
        }
        TextField {
            id: relation
            objectName: "graphEditRelation"
            // How the selected thing relates to the new one, when one is selected.
            visible: sheet.what === "add" && sheet.node.id !== undefined
            Layout.fillWidth: true
            placeholderText: qsTr("What “%1” is to it: mentor of, lives in…").arg(sheet.node.name || "")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: sheet.save()
        }
        Label {
            objectName: "graphEditProblem"
            Layout.fillWidth: true
            visible: text !== ""
            text: sheet.problem
            color: theme.colors.red
            wrapMode: Text.Wrap
        }

        DialogActions {
            AppButton {
                objectName: "graphEditCancel"
                text: sheet.asking ? qsTr("Keep") : qsTr("Cancel")
                onClicked: sheet.close()
            }
            AppButton {
                id: confirm
                objectName: "graphEditConfirm"
                primary: true
                text: sheet.what === "merge" ? qsTr("Merge") : sheet.asking ? qsTr("Remove")
                    : sheet.what === "add" ? qsTr("Add") : qsTr("Save")
                onClicked: sheet.asking ? sheet.agree() : sheet.save()
            }
        }
    }
}
