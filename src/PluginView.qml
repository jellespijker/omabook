import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A plugin's own view (#77, omabook.ui.view): a QML file of the plugin's
// folder, as its panels are, under a header Omabook draws, in place of the
// shelf (PluginViewLayer.qml) or beside the page (PluginSide.qml).
//
// The header has the way back, the view's title and a ⋯ of what can be done
// in it, each the way a pointer or a finger gets to a key: Backspace goes back
// (view.back), Escape closes it (view.close), and the plugin's commands of the
// place "view" work while it has the keyboard. Omabook sets on the plugin's
// root item what it declares of `beside` and `args`, asks its back() before
// the view is closed, gives it the keyboard by its takeFocus(), and hands
// its logChanged(entities) what of the plugin's data came from elsewhere.
FocusScope {
    id: frame
    // As custom.mods has it: {plugin, pluginName, view, title, url, width}; null for none.
    property var entry: null
    // What omabook.ui.showView was given.
    property var args: ({})
    property bool beside: false
    // Whether its keys are the window's now: beside the page only while it
    // has the keyboard, in place of the shelf while it is shown.
    readonly property bool live: !!entry && visible && (!beside || activeFocus)
    readonly property Item content: loader.item
    readonly property bool compact: width < 560
    signal done()

    // Back within the view, if it has somewhere to go back to; else closed.
    function back() {
        if (content && typeof content.back === "function" && content.back()) return
        done()
    }
    function takeFocus() {
        if (content && typeof content.takeFocus === "function") content.takeFocus()
        else if (content) content.forceActiveFocus()
        else frame.forceActiveFocus()
    }

    Key { command: "view.back"; when: frame.live; onActivated: frame.back() }
    Key { command: "view.close"; when: frame.live; onActivated: frame.done() }

    Rectangle {
        anchors.fill: parent
        color: theme.background
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.margins: frame.compact ? 8 : 16
            Layout.bottomMargin: 8
            spacing: 12

            // Back to the shelf, for a pointer or a finger.
            BackButton {
                objectName: frame.beside ? "" : "pluginViewClose"
                runs: "view.close"
                visible: !frame.beside
                text: backend.libraryName
                compact: frame.compact
                onTapped: frame.done()
            }
            Label {
                objectName: frame.beside ? "pluginSideTitle" : "pluginViewTitle"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: frame.entry ? frame.entry.title : ""
                font.pixelSize: frame.beside ? 16 : 20
                font.bold: true
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
            HeaderChoice {
                objectName: frame.beside ? "pluginSideMore" : "pluginViewMore"
                worded: false
                glyph: "more"
                text: qsTr("More")
                onTapped: {
                    frame.takeFocus()
                    win.showMenu("view", undefined, frame.content || frame)
                }
            }
            // Beside the page, put away; as the map's ×.
            Rectangle {
                objectName: frame.beside ? "pluginViewHide" : ""
                readonly property string runs: "view.close"
                visible: frame.beside
                implicitWidth: win.touch ? 40 : 30
                implicitHeight: win.touch ? 40 : 30
                radius: theme.radius
                color: hideHover.hovered ? theme.colors.selection : "transparent"
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Close")
                Glyph {
                    anchors.centerIn: parent
                    name: "cross"
                    size: 14
                    color: theme.colors.muted
                }
                HoverHandler { id: hideHover }
                TapHandler { onTapped: frame.done() }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Loader {
                id: loader
                objectName: frame.entry ? "pluginView-" + frame.entry.plugin + "-" + frame.entry.view : "pluginView"
                anchors.fill: parent
                focus: true
                source: frame.entry ? frame.entry.url : ""
                onLoaded: frame.hand()
            }
            // A view that could not be made says so, rather than leaving a blank.
            Label {
                objectName: "pluginViewBroken"
                anchors.centerIn: parent
                width: Math.min(parent.width - 32, 420)
                visible: loader.status === Loader.Error
                text: qsTr("%1 could not show this view: its QML file has a mistake in it.").arg(frame.entry ? frame.entry.pluginName : "")
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                color: theme.colors.muted
            }
        }
    }

    // What the plugin's item declares of these, it is given.
    function hand() {
        const item = loader.item
        if (!item) return
        if (item.beside !== undefined) item.beside = frame.beside
        if (item.args !== undefined) item.args = frame.args
    }
    onBesideChanged: hand()
    onArgsChanged: hand()

    // What of the plugin's data came from elsewhere (log.changed), for a view that
    // declares logChanged(entities) to show it without being asked.
    Connections {
        target: backend.scripts
        function onLogChanged(plugin, entities) {
            const item = loader.item
            if (frame.entry && frame.entry.plugin === plugin && item && typeof item.logChanged === "function")
                item.logChanged(entities)
        }
    }
}
