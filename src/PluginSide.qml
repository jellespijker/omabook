import QtQuick

// A plugin's view beside the page (#77): the book on one side, the
// plugin's view on the other, at the end the text runs to, where the map,
// the AI and the Aa panel are; it takes their place, and they take its. As
// wide as it is dragged, from the width the plugin gave (see SidePane), and
// lying over the page in a window without the room.
SidePane {
    id: side
    // The ReaderView it stands in.
    required property Item host
    property var entry: null
    property var args: ({})
    readonly property bool shown: entry !== null && reader.open
    readonly property string plugin: entry ? entry.plugin : ""
    readonly property bool live: frame.live
    readonly property bool focused: frame.activeFocus

    // One width a view of each plugin, kept as the others are.
    name: "view:" + plugin + ":" + (entry ? entry.view : "")
    standard: entry ? entry.width : 480
    least: 280
    most: 900

    function show(found, args, toggle) {
        if (toggle && shown && entry.plugin === found.plugin && entry.view === found.view) {
            close()
            return
        }
        side.args = args || {}
        entry = found
        host.showAssistant = false
        host.showGraph = false
        host.showFormat = false
        frame.takeFocus()
    }
    function close() {
        const had = frame.activeFocus
        entry = null
        if (had && host.content) host.content.forceActiveFocus()
    }
    function closeView(plugin, view) { if (entry && entry.plugin === plugin && entry.view === view) close() }
    function takeFocus() { frame.takeFocus() }

    // The entry anew whenever the plugins change: gone with its plugin, made anew when its file is edited.
    Connections {
        target: custom
        function onModsChanged() {
            if (!side.entry) return
            const now = custom.mods.find(mod => mod.place === "view" && mod.plugin === side.entry.plugin && mod.view === side.entry.view)
            side.entry = now || null
        }
    }

    PluginView {
        id: frame
        objectName: "pluginViewSide"
        anchors.fill: parent
        beside: true
        entry: side.entry
        args: side.args
        onDone: side.close()
    }
}
