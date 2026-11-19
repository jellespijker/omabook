import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Publishing a collection, for others to subscribe to (#6): before anything
// is written it says what goes in, counted as `omabook collections share`
// counts it, what never goes (the books' files), and each part that is the
// user's own with its switch, all off until switched on. The first time, it
// asks the name the user goes by; and where to save the file that is given
// to others. Publishing again is publishing the same space, which brings
// what changed since.
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
    readonly property bool canPublish: counted && problem === "" && !backend.spaces.working && file.text.trim() !== ""
                                       && (!firstTime || name.text.trim() !== "")
    readonly property var parts: ({ graph: graph.checked, questions: questions.checked, ratings: ratings.checked, reading: reading.checked })

    function show(id) {
        const known = backend.collections.find(collection => collection.id === id)
        collectionId = known ? id : (backend.collections.length > 0 ? backend.collections[0].id : 0)
        for (const part of [graph, questions, ratings, reading])
            part.checked = false
        name.text = ""
        file.text = ""
        backend.spaces.look(collectionId)
        open()
    }
    function publish() {
        if (canPublish)
            backend.spaces.publish(collectionId, parts, name.text.trim(), file.text.trim())
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
        // What never goes.
        Label {
            Layout.fillWidth: true
            visible: dialog.counted
            text: qsTr("Never goes")
            font.bold: true
        }
        Label {
            objectName: "publishNeverGoes"
            Layout.fillWidth: true
            visible: dialog.counted
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("The books' files (%1): those you share it with need their own copies.").arg(dialog.seen.files || 0)
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
            text: qsTr("The graph: %1 things, %2 facts and %3 relations, only what you have read, nothing past your place in a book")
                  .arg(dialog.seen.things || 0).arg(dialog.seen.facts || 0).arg(dialog.seen.relations || 0)
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
            text: qsTr("Your reading: how far you are in %1 books, and the %2 days you read").arg(dialog.seen.places || 0).arg(dialog.seen.days || 0)
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
        // Where the file goes. A link to share in its place (#7) goes beside it.
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
