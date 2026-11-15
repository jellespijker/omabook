import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The collections as tabs above the shelf: All books, then each collection
// at the top, the one shown (or the one it is inside) underlined. With
// `parentId`, a second row: the collections inside that one, after one tab
// for all of it. A click or a tap shows one; [ and ] step through
// them; a right click or a long press opens one to change. Where the window
// is too narrow for every tab, the row scrolls (by finger, by wheel, and by
// itself to keep the one shown in view), with a fade at the side that has
// more. At its end, always in view: a new collection, and the list of what
// can be done with collections.
Item {
    id: strip
    objectName: "collectionStrip"
    // The list of collections and what can be done with them (C).
    signal more()
    signal create(var parent)
    signal edit(var id)

    // The collection whose insides this row shows; 0 for the top.
    property var parentId: 0
    readonly property real tabHeight: win.touch ? (parentId ? 36 : 40) : (parentId ? 28 : 32)
    // The collection shown and those it is inside, from the top.
    readonly property var chain: {
        const byId = {}
        for (const collection of backend.collections)
            byId[collection.id] = collection
        const list = []
        for (let at = byId[backend.collection]; at; at = byId[at.parent])
            list.unshift(at.id)
        return list
    }
    readonly property var entries: {
        if (!parentId)
            return [({ id: 0, name: qsTr("All books") })].concat(backend.collections.filter(c => c.depth === 0))
        const parent = backend.collections.find(c => c.id === parentId)
        return [({ id: parentId, name: qsTr("All of %1").arg(parent ? parent.name : "") })]
               .concat(backend.collections.filter(c => c.parent === parentId))
    }
    // The tab that is marked: on the top row, the top of the chain.
    readonly property var marked: parentId ? backend.collection : (chain.length > 0 ? chain[0] : 0)
    readonly property int shownIndex: {
        for (let i = 0; i < entries.length; ++i)
            if (entries[i].id === marked)
                return i
        return 0
    }
    implicitHeight: tabHeight
    // The keys, for the tips.
    function keyOf(command) { return (custom.keyText[command] || "").split("  ")[0] }

    ListView {
        id: tabs
        objectName: "collectionTabs"
        anchors.left: parent.left
        anchors.right: actions.left
        anchors.rightMargin: 4
        height: parent.height
        orientation: ListView.Horizontal
        layoutDirection: win.rightToLeft ? Qt.RightToLeft : Qt.LeftToRight
        clip: true
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentWidth > width
        model: strip.entries
        currentIndex: strip.shownIndex
        // The one shown is kept in view, clear of the fades, however it was
        // chosen and whatever the width; the row can still be scrolled past it.
        highlightFollowsCurrentItem: true
        highlightRangeMode: ListView.ApplyRange
        preferredHighlightBegin: 24
        preferredHighlightEnd: width - 24
        highlightMoveDuration: 150
        highlight: Item {}

        delegate: Item {
            id: tab
            required property int index
            required property var modelData
            readonly property bool shown: modelData.id === strip.marked
            objectName: (strip.parentId ? "collectionSubTab-" : "collectionTab-") + modelData.id
            width: Math.min(label.implicitWidth, 200) + 24
            height: tabs.height
            Accessible.role: Accessible.PageTab
            Accessible.name: modelData.name
            Accessible.selected: shown
            Accessible.onPressAction: backend.collection = modelData.id

            Rectangle {
                anchors.fill: parent
                anchors.bottomMargin: 2
                radius: Math.min(4, theme.radius)
                color: theme.colors.selection
                visible: hover.hovered && !tab.shown
                opacity: 0.6
            }
            Label {
                id: label
                objectName: "collectionTabName"
                anchors.centerIn: parent
                width: Math.min(implicitWidth, 200)
                text: tab.modelData.name
                elide: Text.ElideRight
                color: tab.shown || hover.hovered ? theme.foreground : theme.colors.muted
                font.bold: tab.shown
            }
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - 12
                height: 2
                radius: 1
                color: theme.accent
                visible: tab.shown
            }
            HoverHandler { id: hover }
            TapHandler {
                onTapped: backend.collection = tab.modelData.id
                onLongPressed: if (tab.modelData.id !== 0 && tab.modelData.name !== undefined) strip.edit(tab.modelData.id)
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: if (tab.modelData.id !== 0) strip.edit(tab.modelData.id)
            }
            ToolTip.visible: hover.hovered
            ToolTip.delay: 600
            ToolTip.text: tab.modelData.id === 0 ? qsTr("Every book in the library")
                          : qsTr("%1: %Ln books", "", tab.modelData.count !== undefined ? tab.modelData.count : backend.books.total).arg(tab.modelData.name)
                            + "  " + strip.keyOf("library.collection_next")
        }
    }
    // A fade where the row goes on beyond what is in view.
    Rectangle {
        anchors.left: tabs.left
        width: 24
        height: tabs.height
        visible: tabs.interactive && !(win.rightToLeft ? tabs.atXEnd : tabs.atXBeginning)
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: theme.background }
            GradientStop { position: 1; color: Qt.alpha(theme.background, 0) }
        }
    }
    Rectangle {
        anchors.right: tabs.right
        width: 24
        height: tabs.height
        visible: tabs.interactive && !(win.rightToLeft ? tabs.atXBeginning : tabs.atXEnd)
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Qt.alpha(theme.background, 0) }
            GradientStop { position: 1; color: theme.background }
        }
    }
    Row {
        id: actions
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2
        HeaderChoice {
            objectName: strip.parentId ? "collectionNewInside" : "collectionNew"
            worded: false
            glyph: "plus"
            text: strip.parentId ? qsTr("New collection inside this one") : qsTr("New collection")
            onTapped: strip.create(strip.parentId)
        }
        HeaderChoice {
            objectName: "collectionMore"
            visible: !strip.parentId
            worded: false
            glyph: "more"
            text: qsTr("Collections")
            keys: strip.keyOf("library.collections")
            onTapped: strip.more()
        }
    }
}
