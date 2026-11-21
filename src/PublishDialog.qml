import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Publishing a collection, for others to subscribe to (#6): before anything
// is written it says what goes in, counted as `omabook collections share`
// counts it, and each part that is the user's own with its switch, all off
// until switched on: the books' files too (#63), which then go to everyone
// the space reaches, as the dialog says plainly when it is switched on. The first time, it
// asks the name the user goes by; and where to save the file that is given
// to others, or that it goes as a link (a gist, secret unless asked, #7), or
// both. Publishing again is publishing the same space, which brings what
// changed since; by link, to the same gist.
AppPopup {
    id: dialog
    objectName: "publishDialog"
    widest: 580
    // The collection being published.
    property var collectionId: 0
    readonly property var seen: backend.spaces.publishing
    // What was counted is of this collection, and nothing went wrong.
    readonly property bool counted: seen.collection === collectionId && seen.path !== undefined
    readonly property string problem: seen.collection === collectionId ? (seen.error || "") : ""
    readonly property bool firstTime: counted && (seen.name || "") === ""
    // The books a plugin keeps private, and the one being asked about.
    readonly property var kept: seen.kept || []
    property var asking: null
    readonly property bool canPublish: counted && problem === "" && !backend.spaces.working && (file.text.trim() !== "" || asLink.checked)
                                       && (!firstTime || name.text.trim() !== "")
    readonly property var parts: ({ graph: graph.checked, questions: questions.checked, ratings: ratings.checked, reading: reading.checked,
                                    files: files.checked, annotations: annotations.checked })

    function show(id) {
        const known = backend.collections.find(collection => collection.id === id)
        collectionId = known ? id : (backend.collections.length > 0 ? backend.collections[0].id : 0)
        for (const part of [graph, questions, ratings, reading, files, annotations, asLink, openLink])
            part.checked = false
        name.text = ""
        file.text = ""
        asking = null
        backend.spaces.look(collectionId)
        open()
    }
    function publish() {
        if (canPublish)
            backend.spaces.publish(collectionId, parts, name.text.trim(), file.text.trim(), asLink.checked, asLink.checked && openLink.checked)
    }
    onOpened: (firstTime ? name : file).forceActiveFocus()

    // A part's switch, its caption wrapped where the dialog is narrower than
    // it, and tall enough for a fingertip.
    component PartSwitch: Switch {
        id: part
        Layout.fillWidth: true
        Layout.minimumHeight: 40
        visible: dialog.counted
        Accessible.name: text
        contentItem: Label {
            text: part.text
            wrapMode: Text.Wrap
            verticalAlignment: Text.AlignVCenter
            leftPadding: part.mirrored ? 0 : part.indicator.width + part.spacing
            rightPadding: part.mirrored ? part.indicator.width + part.spacing : 0
        }
    }

    Connections {
        target: backend.spaces
        // The suggested file once the counts are in, unless one was given.
        function onChanged() {
            if (dialog.counted && file.text === "")
                file.text = dialog.seen.file || ""
        }
        function onFileChosen(chosen) { file.text = chosen }
        function onPublished() { dialog.close() }
    }

    DialogBody {
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            Layout.fillWidth: true
            text: qsTr("Publish a collection")
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Whoever you give the file to can subscribe to it: their library finds the books they have, and the rest become wishes. Nothing of theirs changes, and nothing of it can be changed by them.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        ComboBox {
            id: which
            objectName: "publishCollection"
            Layout.fillWidth: true
            Layout.minimumHeight: 40
            model: backend.collections
            textRole: "path"
            valueRole: "id"
            currentIndex: Math.max(0, backend.collections.findIndex(collection => collection.id === dialog.collectionId))
            Material.roundedScale: theme.radius
            onActivated: {
                dialog.collectionId = currentValue
                file.text = ""
                backend.spaces.look(currentValue)
            }
        }
        Thinking {
            active: dialog.opened && !dialog.counted && dialog.problem === ""
            text: qsTr("Counting what goes in…")
            room: dialog.availableWidth
        }

        // What goes in, always.
        Label {
            Layout.fillWidth: true
            visible: dialog.counted
            text: qsTr("Goes in")
            font.bold: true
        }
        Label {
            objectName: "publishGoesIn"
            Layout.fillWidth: true
            visible: dialog.counted
            wrapMode: Text.Wrap
            text: qsTr("%Ln collections, with their names, rules and how their shelves show", "", dialog.seen.collections || 0) + "\n"
                  + qsTr("%Ln books: title, authors, series, ISBN and their names on other sites", "", dialog.seen.books || 0)
        }
        // A book a plugin keeps private (a notebook) stays out, all of it,
        // unless let go one by one, asked first, and kept out again as
        // easily (#127). Each says why it is out: its rule took it in, or
        // it was put in by hand.
        Label {
            Layout.fillWidth: true
            visible: dialog.counted && dialog.kept.length > 0
            text: qsTr("Kept private")
            font.bold: true
        }
        Repeater {
            model: dialog.counted ? dialog.kept : []
            delegate: RowLayout {
                id: keptRow
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: 8
                Label {
                    objectName: "publishKeptBook" + keptRow.index
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: keptRow.modelData.shared ? Material.foreground : theme.colors.muted
                    text: keptRow.modelData.shared
                          ? qsTr("%1 goes in, as you chose, though %2 keeps its books private.")
                                .arg(keptRow.modelData.title).arg(keptRow.modelData.plugin_title)
                          : keptRow.modelData.by_rule
                            ? qsTr("%1 matches the collection's rule, but %2 keeps its books private, so it stays out.")
                                  .arg(keptRow.modelData.title).arg(keptRow.modelData.plugin_title)
                            : qsTr("%1 is in the collection, but %2 keeps its books private, so it stays out.")
                                  .arg(keptRow.modelData.title).arg(keptRow.modelData.plugin_title)
                }
                AppButton {
                    objectName: "publishKeptShare" + keptRow.index
                    enabled: !backend.spaces.working
                    text: keptRow.modelData.shared ? qsTr("Keep it private") : qsTr("Share it…")
                    onClicked: {
                        if (keptRow.modelData.shared)
                            backend.spaces.shareKept(dialog.collectionId, keptRow.modelData.book, false)
                        else
                            dialog.asking = keptRow.modelData
                    }
                }
            }
        }
        // Letting one go is asked once more, saying what goes and to whom.
        Label {
            objectName: "publishKeptAsk"
            Layout.fillWidth: true
            visible: dialog.counted && dialog.asking !== null
            wrapMode: Text.Wrap
            color: theme.colors.orange
            text: dialog.asking === null ? ""
                  : qsTr("Share %1 in the spaces it is in? Its details go to everyone they reach, and as their parts say, its file and what you know and wrote of it. You can keep it private again; what others have of it by then stays theirs.")
                        .arg(dialog.asking.title)
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.counted && dialog.asking !== null
            spacing: 8
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "publishKeptCancel"
                text: qsTr("Keep it private")
                onClicked: dialog.asking = null
            }
            AppButton {
                objectName: "publishKeptConfirm"
                text: qsTr("Share it")
                onClicked: {
                    backend.spaces.shareKept(dialog.collectionId, dialog.asking.book, true)
                    dialog.asking = null
                }
            }
        }

        // What is the user's own goes only when switched on.
        Label {
            Layout.fillWidth: true
            visible: dialog.counted
            text: qsTr("Yours, only if you switch it on")
            font.bold: true
        }
        PartSwitch {
            id: graph
            objectName: "publishGraph"
            text: qsTr("The graph: %1, %2 and %3, only what you have read, nothing past your place in a book")
                  .arg(qsTr("%Ln things", "", dialog.seen.things || 0)).arg(qsTr("%Ln facts", "", dialog.seen.facts || 0))
                  .arg(qsTr("%Ln relations", "", dialog.seen.relations || 0))
        }
        PartSwitch {
            id: questions
            objectName: "publishQuestions"
            text: qsTr("The questions you asked of its books (%1)").arg(dialog.seen.questions || 0)
        }
        PartSwitch {
            id: ratings
            objectName: "publishRatings"
            text: qsTr("Your ratings (%1)").arg(dialog.seen.ratings || 0)
        }
        PartSwitch {
            id: reading
            objectName: "publishReading"
            text: qsTr("Your reading: how far you are in %1, and %2 of reading")
                  .arg(qsTr("%Ln books", "", dialog.seen.places || 0)).arg(qsTr("%Ln days", "", dialog.seen.days || 0))
        }
        PartSwitch {
            id: annotations
            objectName: "publishAnnotations"
            text: qsTr("Your highlights, margin notes and ink in its books, seen by each only as far as they have read")
        }
        PartSwitch {
            id: files
            objectName: "publishFiles"
            text: qsTr("The books' files (%1), beside the file you save").arg(dialog.seen.files || 0)
        }
        // Who gets them, said plainly before anything goes.
        Label {
            objectName: "publishFilesReach"
            Layout.fillWidth: true
            visible: dialog.counted && files.checked
            wrapMode: Text.Wrap
            color: theme.colors.orange
            text: asLink.checked ? qsTr("The files go to everyone you give the saved file to, and everyone they pass it to: they are copied into a folder beside it. The link carries no files.")
                                 : qsTr("The files go to everyone you give the saved file to, and everyone they pass it to: they are copied into a folder beside it.")
        }

        // Who it is from: asked the first time, and kept with the space.
        Label {
            Layout.fillWidth: true
            visible: dialog.counted && !dialog.firstTime
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("Published as %1, as before.").arg(dialog.seen.name || "")
        }
        TextField {
            id: name
            objectName: "publishName"
            Layout.fillWidth: true
            visible: dialog.firstTime
            placeholderText: qsTr("The name you go by, as those you share it with will see it")
            selectByMouse: true
            maximumLength: 60
            Material.roundedScale: theme.radius
            onAccepted: file.forceActiveFocus()
        }
        // Where the file goes, and the link it can go by too.
        Label {
            Layout.fillWidth: true
            visible: dialog.counted
            text: qsTr("Save it as")
            color: theme.colors.muted
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.counted
            spacing: 8
            TextField {
                id: file
                objectName: "publishFile"
                Layout.fillWidth: true
                selectByMouse: true
                Material.roundedScale: theme.radius
                onAccepted: dialog.publish()
            }
            AppButton {
                objectName: "publishChooseFile"
                text: qsTr("Choose…")
                onClicked: backend.spaces.chooseFile(true, file.text)
            }
        }
        PartSwitch {
            id: asLink
            objectName: "publishLink"
            text: qsTr("Also as a link: a gist on GitHub, sent through its gh command, that those you give it to can follow")
        }
        PartSwitch {
            id: openLink
            objectName: "publishLinkPublic"
            visible: dialog.counted && asLink.checked
            text: qsTr("Anyone may find it (a public gist); else only those given the link")
        }
        Label {
            objectName: "publishProblem"
            Layout.fillWidth: true
            visible: dialog.problem !== ""
            wrapMode: Text.Wrap
            color: theme.colors.orange
            text: dialog.problem
        }

        actions: [
            AppButton { objectName: "publishCancel"; text: qsTr("Cancel"); onClicked: dialog.close() },
            AppButton {
                objectName: "publishConfirm"
                primary: true
                enabled: dialog.canPublish
                text: qsTr("Publish")
                onClicked: dialog.publish()
            }
        ]
    }
}
