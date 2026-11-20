import QtQuick

// A plugin's view in place of the shelf (#77), and where a plugin's asking
// for one goes: beside the page while a book is open (ReaderView's
// pluginSide), here otherwise, as the graph a plugin asks for does. Opened
// from the shelf, a book is read over it, and closing the book comes back to
// it; the graph and the chronology lie over it the same way.
Item {
    id: layer
    // The view shown here, by its plugin and id; "" for none.
    property string plugin: ""
    property string view: ""
    property var args: ({})
    // As custom.mods has it, while the plugin has it: a plugin switched off
    // or reloaded without it takes it away, an edited file is made anew.
    readonly property var entry: find(plugin, view)
    readonly property bool shown: entry !== null
    visible: shown && !win.inReader && !win.chronologyShown && !win.graphShown
    // The plugin whose view has the keyboard, here or beside the page: its
    // commands of the place "view" are the ones that work.
    readonly property string keysOf: visible ? plugin
                                    : win.inReader && readerLoader.item ? readerLoader.item.pluginKeys : ""

    function find(plugin, view) {
        if (plugin === "") return null
        return custom.mods.find(mod => mod.place === "view" && mod.plugin === plugin && mod.view === view) || null
    }
    function show(plugin, view, args, toggle) {
        if (!find(plugin, view)) return
        if (win.inReader && readerLoader.item) {
            readerLoader.item.showPluginView(find(plugin, view), args, toggle)
            return
        }
        if (toggle && visible && layer.plugin === plugin && layer.view === view) {
            close()
            return
        }
        // What it is given first, so the view made anew has it at once.
        layer.args = args || {}
        layer.plugin = plugin
        layer.view = view
        win.graphShown = false
        frame.takeFocus()
    }
    function close() {
        plugin = ""
        view = ""
        win.restoreFocus()
    }
    function closeView(plugin, view) {
        if (layer.plugin === plugin && layer.view === view) close()
        if (readerLoader.item) readerLoader.item.closePluginView(plugin, view)
    }
    function takeFocus() { frame.takeFocus() }
    // Once the plugins have changed, not while the view is being worked out.
    onShownChanged: if (!shown && plugin !== "") Qt.callLater(close)
    onVisibleChanged: if (visible) Qt.callLater(frame.takeFocus)

    Connections {
        target: backend.scripts
        function onViewRequested(plugin, view, args, toggle) { layer.show(plugin, view, args, toggle) }
        function onViewClosed(plugin, view) { layer.closeView(plugin, view) }
    }

    PluginView {
        id: frame
        objectName: "pluginViewFull"
        anchors.fill: parent
        entry: layer.entry
        args: layer.args
        onDone: layer.close()
    }
}
