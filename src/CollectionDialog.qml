import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Making a collection, or changing one: its name, and the search that fills
// it, with how many books that finds as it is typed. Books put in or taken
// out by hand stay so whatever the rule; this says how many there are.
// Removing a collection asks first, and only ever takes the collection away:
// its books stay in the library. From here a collection is published for
// others, and one filled by a space someone shared is unsubscribed from.
AppPopup {
    id: dialog
    objectName: "collectionDialog"
    widest: 560
    // The collection being changed; 0 for a new one.
    property var collectionId: 0
    // The collection it is inside; 0 for the top.
    property var parentId: 0
    property bool confirming: false
    property bool unsubscribing: false
    // What following this collection's link found, when it was looked for.
    readonly property var updates: dialog.editing !== null && backend.spaces.following.space === dialog.editing.space
                                   ? backend.spaces.following : ({})
    // Where it can go: the top, or any collection but itself and those inside it.
    readonly property var places: {
        const self = editing
        const list = [({ id: 0, path: qsTr("At the top") })]
        for (const collection of backend.collections)
            if (!self || (collection.id !== self.id && !collection.path.startsWith(self.path + "/")))
                list.push({ id: collection.id, path: collection.path })
        return list
    }
    readonly property var editing: {
        for (const collection of backend.collections)
            if (collection.id === collectionId)
                return collection
        return null
    }
    readonly property string problem: backend.ruleProblem(rule.text)
    readonly property int matching: {
        backend.revision
        return problem === "" ? backend.ruleCount(rule.text) : 0
    }
    readonly property bool canSave: name.text.trim() !== "" && problem === ""

    // A new collection, its rule filled in when one is given (the shelf's search and filters).
    function create(withRule, parent) {
        collectionId = 0
        parentId = parent || 0
        name.text = ""
        rule.text = withRule || ""
        confirming = false
        open()
        name.forceActiveFocus()
    }
    function change(id) {
        collectionId = id
        parentId = editing ? editing.parent : 0
        name.text = editing ? editing.name : ""
        rule.text = editing ? editing.rule : ""
        confirming = false
        open()
        name.forceActiveFocus()
    }
    function save() {
        if (!canSave)
            return
        const id = backend.saveCollection(collectionId, name.text.trim(), rule.text.trim(), parentId)
        if (id === 0)
            return
        close()
        // A new collection is shown at once, so it is clear what it holds.
        if (collectionId === 0)
            backend.collection = id
    }
    onClosed: {
        confirming = false
        unsubscribing = false
    }
    // Once it is up: a popup that is still opening doesn't keep the keyboard.
    onOpened: name.forceActiveFocus()

    DialogBody {
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        Label {
            Layout.fillWidth: true
            text: dialog.collectionId === 0 ? qsTr("New collection") : qsTr("Change the collection")
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("A collection is a part of the library, named as you like. A book can be in several, and one collection can hold others; nothing is copied or moved.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        TextField {
            id: name
            objectName: "collectionName"
            Layout.fillWidth: true
            placeholderText: qsTr("Name")
            selectByMouse: true
            maximumLength: 60
            Material.roundedScale: theme.radius
            onAccepted: rule.forceActiveFocus()
        }
        // Inside another, or at the top.
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label { text: qsTr("Inside"); color: theme.colors.muted }
            ComboBox {
                id: inside
                objectName: "collectionParent"
                Layout.fillWidth: true
                Layout.minimumHeight: 40
                model: dialog.places
                textRole: "path"
                valueRole: "id"
                currentIndex: Math.max(0, dialog.places.findIndex(place => place.id === dialog.parentId))
                Material.roundedScale: theme.radius
                onActivated: dialog.parentId = currentValue
            }
        }
        TextField {
            id: rule
            objectName: "collectionRule"
            Layout.fillWidth: true
            placeholderText: qsTr("Books it holds: genre:fiction, tag:programming…")
            selectByMouse: true
            Material.roundedScale: theme.radius
            onAccepted: dialog.save()
        }
        // What the rule finds, or what is wrong with it, as it is typed.
        Label {
            objectName: "collectionMatches"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: dialog.problem !== "" ? theme.colors.orange : theme.colors.muted
            text: dialog.problem !== "" ? dialog.problem
                  : rule.text.trim() === "" ? qsTr("With no rule it holds only the books you put in it.")
                  : qsTr("The rule finds %Ln books.", "", dialog.matching)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: theme.colors.muted
            font.pixelSize: 12
            text: qsTr("The rule is a search: author:, title:, series:, tag:, genre: (fiction, nonfiction, or a tag), format:, lang:, about:, status:, is: and rating:. \"Quotes\" keep words together, -word leaves out, and OR joins: genre:fiction OR series:Dune. A new book joins every collection whose rule finds it.")
        }
        Label {
            objectName: "collectionByHand"
            Layout.fillWidth: true
            visible: dialog.editing !== null && (dialog.editing.included > 0 || dialog.editing.excluded > 0)
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: dialog.editing === null ? ""
                  : qsTr("%Ln put in by hand", "", dialog.editing.included) + " · " + qsTr("%Ln taken out by hand", "", dialog.editing.excluded)
        }
        AppButton {
            objectName: "collectionFromShelf"
            readonly property string shelf: {
                backend.books.query
                backend.books.filters
                return backend.shelfRule()
            }
            visible: shelf !== "" && rule.text.trim() !== shelf
            text: qsTr("Use the shelf's search and filters")
            onClicked: rule.text = shelf
        }
        // Whose space fills it, and unsubscribing from that, asked first in place.
        Label {
            objectName: "collectionShared"
            Layout.fillWidth: true
            visible: dialog.editing !== null && (dialog.editing.space || "") !== ""
            wrapMode: Text.Wrap
            color: dialog.unsubscribing ? theme.colors.orange : theme.colors.muted
            text: dialog.editing === null ? ""
                  : dialog.unsubscribing ? qsTr("Unsubscribe from %1's shared collection? This collection stays, with its books and wishlist entries; the shared graph goes.").arg(dialog.editing.sharedBy || "?")
                  : qsTr("Subscribed to, from %1's shared collection: what they add to it comes here too.").arg(dialog.editing.sharedBy || "?")
        }
        // What its link has that it hasn't, once looked for.
        Label {
            objectName: "collectionUpdates"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: dialog.updates.problem ? theme.colors.orange : theme.colors.muted
            text: dialog.updates.space === undefined ? ""
                  : dialog.updates.problem ? dialog.updates.problem
                  : dialog.updates.operations === undefined ? qsTr("Looking for updates…")
                  : dialog.updates.operations > 0 ? qsTr("%Ln changes are new there: %1.", "", dialog.updates.operations).arg(dialog.updates.changes)
                  : qsTr("Nothing new there.")
        }
        Flow {
            Layout.fillWidth: true
            visible: dialog.collectionId !== 0
            spacing: 8
            AppButton {
                objectName: "collectionPublish"
                runs: "library.publish"
                text: qsTr("Publish…")
                onClicked: {
                    const id = dialog.collectionId
                    dialog.close()
                    win.showPublish(id)
                }
            }
            AppButton {
                objectName: "collectionLookForUpdates"
                visible: dialog.editing !== null && dialog.editing.followed === true
                text: qsTr("Look for updates")
                onClicked: backend.spaces.lookForUpdates(dialog.editing.space)
            }
            AppButton {
                objectName: "collectionTakeUpdates"
                visible: dialog.updates.operations > 0
                text: qsTr("Take them in")
                onClicked: backend.spaces.takeUpdates(dialog.editing.space)
            }
            AppButton {
                objectName: "collectionUnsubscribe"
                visible: dialog.editing !== null && (dialog.editing.space || "") !== ""
                text: dialog.unsubscribing ? qsTr("Unsubscribe") : qsTr("Unsubscribe…")
                onClicked: {
                    if (!dialog.unsubscribing) {
                        dialog.unsubscribing = true
                        return
                    }
                    backend.spaces.unsubscribe(dialog.editing.space)
                    dialog.close()
                }
            }
        }
        // Removing asks first, in place.
        Label {
            objectName: "collectionRemoveAsk"
            Layout.fillWidth: true
            visible: dialog.confirming
            wrapMode: Text.Wrap
            color: theme.colors.orange
            text: qsTr("Remove the collection %1? Its books stay in the library.").arg(dialog.editing ? dialog.editing.name : "")
        }

        actions: [
            AppButton {
                objectName: "collectionRemove"
                visible: dialog.collectionId !== 0
                text: dialog.confirming ? qsTr("Remove it") : qsTr("Remove…")
                onClicked: {
                    if (!dialog.confirming) {
                        dialog.confirming = true
                        return
                    }
                    backend.removeCollection(dialog.collectionId)
                    dialog.close()
                }
            },
            AppButton { objectName: "collectionCancel"; text: qsTr("Cancel"); onClicked: dialog.close() },
            AppButton {
                objectName: "collectionSave"
                primary: true
                enabled: dialog.canSave
                text: dialog.collectionId === 0 ? qsTr("Make it") : qsTr("Save")
                onClicked: dialog.save()
            }
        ]
    }
}
