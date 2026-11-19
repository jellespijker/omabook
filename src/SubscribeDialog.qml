import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Subscribing to a collection someone published (#6), from its file: picked
// from the ⋯ menu, or dropped on the window. Before anything is written it
// says what of it matched books of the library, what becomes wishlist
// entries (books without a file, never downloaded), the collection its books
// go in (its name changeable, an existing one of that name joined), and that
// its graph stays a layer of its own, shown only as far as the reader has read.
AppPopup {
    id: dialog
    objectName: "subscribeDialog"
    widest: 580
    readonly property var seen: backend.spaces.subscribing
    readonly property bool hasFile: (seen.file || "") !== ""
    readonly property bool ready: seen.space !== undefined && (seen.error || "") === ""
    readonly property string problem: seen.error || ""
    readonly property var matched: seen.matched || []
    readonly property var wishlist: seen.wishlist || []
    readonly property var skipped: seen.skipped || []
    // The collection named is one there is already: its books join it.
    readonly property bool joins: backend.collections.some(collection => collection.path.toLowerCase() === name.text.trim().toLowerCase())
    readonly property bool canSubscribe: ready && !backend.spaces.working && name.text.trim() !== ""

    // From the ⋯ menu or the key: a file to pick first.
    function show() {
        backend.spaces.read("")
        name.text = ""
        open()
        backend.spaces.chooseFile(false)
    }
    function subscribe() {
        if (canSubscribe)
            backend.spaces.subscribe(seen.file, name.text.trim())
    }
    // A few titles, and how many more there are.
    function titles(books) {
        const shown = books.slice(0, 6).map(book => book.authors !== "" ? qsTr("%1, by %2").arg(book.title).arg(book.authors) : book.title)
        if (books.length > shown.length)
            shown.push(qsTr("and %Ln more", "", books.length - shown.length))
        return shown.join("\n")
    }

    Connections {
        target: backend.spaces
        // A file dropped on the window, or picked: shown here.
        function onOffered() {
            name.text = ""
            if (!dialog.opened)
                dialog.open()
        }
        function onChanged() {
            if (dialog.ready && name.text === "")
                name.text = dialog.seen.collection || ""
        }
        function onSubscribed() { dialog.close() }
    }

    DialogBody {
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            Layout.fillWidth: true
            text: dialog.ready ? qsTr("Subscribe to %1").arg(dialog.seen.name) : qsTr("Subscribe to a shared collection")
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "subscribeOwner"
            Layout.fillWidth: true
            visible: dialog.ready
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: (dialog.seen.owner ? qsTr("Published by %1.").arg(dialog.seen.owner) : qsTr("Published by someone who gave no name."))
                  + (dialog.seen.known ? " " + qsTr("You subscribe to it already: this brings what changed since.") : "")
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.hasFile
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("Choose the .omabook-space file someone gave you, or drop it on the window. Nothing is subscribed to before you say so here.")
        }
        Thinking {
            active: dialog.opened && dialog.hasFile && !dialog.ready && dialog.problem === ""
            text: qsTr("Reading the shared collection…")
            room: dialog.availableWidth
        }

        // What the library has of it, and what it lacks.
        Label {
            objectName: "subscribeMatched"
            Layout.fillWidth: true
            visible: dialog.ready
            font.bold: true
            wrapMode: Text.Wrap
            text: qsTr("%Ln of its books are in your library", "", dialog.matched.length)
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.ready && dialog.matched.length > 0
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: dialog.titles(dialog.matched)
        }
        Label {
            objectName: "subscribeWishlist"
            Layout.fillWidth: true
            visible: dialog.ready
            font.bold: true
            wrapMode: Text.Wrap
            text: qsTr("%Ln become wishlist entries: books without a file, never downloaded", "", dialog.wishlist.length)
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.ready && dialog.wishlist.length > 0
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: dialog.titles(dialog.wishlist)
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.ready && dialog.skipped.length > 0
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: qsTr("%Ln you removed before stay removed", "", dialog.skipped.length)
        }

        // Where its books go.
        Label {
            Layout.fillWidth: true
            visible: dialog.ready
            text: qsTr("Its books go in the collection")
            color: theme.colors.muted
        }
        TextField {
            id: name
            objectName: "subscribeName"
            Layout.fillWidth: true
            visible: dialog.ready
            selectByMouse: true
            maximumLength: 60
            Material.roundedScale: theme.radius
            onAccepted: dialog.subscribe()
        }
        Label {
            objectName: "subscribeJoins"
            Layout.fillWidth: true
            visible: dialog.ready && name.text.trim() !== ""
            wrapMode: Text.Wrap
            color: theme.colors.muted
            font.pixelSize: 12
            text: dialog.joins ? qsTr("You have a collection of that name: its books join it, and nothing in it is taken out.")
                  : qsTr("A new collection, yours to change like any other.")
        }
        // The graph, kept apart.
        Label {
            objectName: "subscribeGraph"
            Layout.fillWidth: true
            visible: dialog.ready
            wrapMode: Text.Wrap
            text: (dialog.seen.things || 0) + (dialog.seen.facts || 0) + (dialog.seen.relations || 0) > 0
                  ? qsTr("Its graph (%1 things, %2 facts, %3 relations) stays a layer of its own, apart from yours, and shows only as far as you have read each book.")
                    .arg(dialog.seen.things).arg(dialog.seen.facts).arg(dialog.seen.relations)
                  : qsTr("No graph comes with it.")
        }
        Label {
            objectName: "subscribeProblem"
            Layout.fillWidth: true
            visible: dialog.problem !== ""
            wrapMode: Text.Wrap
            color: theme.colors.orange
            text: dialog.problem
        }

        actions: [
            AppButton {
                objectName: "subscribeChooseFile"
                text: dialog.hasFile ? qsTr("Another file…") : qsTr("Choose a file…")
                onClicked: backend.spaces.chooseFile(false)
            },
            AppButton { objectName: "subscribeCancel"; text: qsTr("Cancel"); onClicked: dialog.close() },
            AppButton {
                objectName: "subscribeConfirm"
                primary: true
                enabled: dialog.canSubscribe
                text: qsTr("Subscribe")
                onClicked: dialog.subscribe()
            }
        ]
    }
}
