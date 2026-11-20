import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Which part of the graph a view shows (ADR 0003): one of the library's
// top-level collections, or Unfiled, the books in none, each a graph of its
// own with nothing of the others in it. `C` lists them to choose from,
// typing narrows the list as in the other lists, and PgDown and PgUp go to
// the next and the previous; the header's TenantSwitch is the same for a
// pointer and a finger. The choice is the window's (graphTenants, in
// Main.qml), so the graph and the chronology show the same one, and it is
// remembered. With one graph for the whole library there is nothing to
// choose, and none of this is there.
Item {
    id: choice
    // Whose keys these are: "graph" or "chronology".
    property string place: "graph"
    property bool keysOn: false
    readonly property bool opened: popup.opened
    signal closed()

    function nameOf(tenant) { return !tenant ? "" : tenant.unfiled ? qsTr("Unfiled") : tenant.name }
    function open() {
        field.text = ""
        list.currentIndex = Math.max(0, graphTenants.tenants.findIndex(tenant => tenant.id === graphTenants.current))
        popup.open()
    }
    function take(tenant) {
        if (tenant === undefined) return
        popup.close()
        graphTenants.current = tenant.id
    }

    Key { command: choice.place + ".tenant"; when: choice.keysOn && graphTenants.switchable; onActivated: choice.open() }
    Key { command: choice.place + ".tenant_next"; when: choice.keysOn && graphTenants.switchable; onActivated: graphTenants.step(1) }
    Key { command: choice.place + ".tenant_previous"; when: choice.keysOn && graphTenants.switchable; onActivated: graphTenants.step(-1) }

    AppPopup {
        id: popup
        objectName: choice.place + "TenantChoice"
        padding: 16
        // Once it is up, when the popup has taken the keyboard for itself.
        onOpened: field.forceActiveFocus()
        onClosed: choice.closed()

        readonly property var matching: {
            const wanted = field.text.trim().toLowerCase().split(" ").filter(word => word !== "")
            return graphTenants.tenants.filter(tenant => wanted.every(word => choice.nameOf(tenant).toLowerCase().includes(word)))
        }

        ColumnLayout {
            spacing: 8
            // A popup is not an Item and doesn't inherit the window's mirroring.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label {
                Layout.fillWidth: true
                Layout.maximumWidth: field.Layout.preferredWidth
                text: choice.place === "chronology" ? qsTr("Show the chronology of") : qsTr("Show the graph of")
                font.bold: true
                wrapMode: Text.Wrap
            }
            TextField {
                id: field
                objectName: choice.place + "TenantField"
                Layout.preferredWidth: Math.min(420, win.width - 2 * win.dialogEdge - 2 * popup.padding)
                placeholderText: qsTr("Collection")
                selectByMouse: true
                Material.roundedScale: theme.radius
                onTextEdited: list.currentIndex = 0
                Keys.onDownPressed: list.incrementCurrentIndex()
                Keys.onUpPressed: list.decrementCurrentIndex()
                Keys.onReturnPressed: choice.take(popup.matching[list.currentIndex])
                Keys.onEnterPressed: choice.take(popup.matching[list.currentIndex])
            }
            ListView {
                id: list
                objectName: choice.place + "TenantList"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, Math.max(120, win.height - 260), 360)
                clip: true
                model: popup.matching
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: entry
                    required property int index
                    required property var modelData
                    width: list.width
                    height: win.touch ? 44 : 34

                    Rectangle {
                        anchors.fill: parent
                        visible: entry.ListView.isCurrentItem
                        radius: Math.min(4, theme.radius)
                        color: theme.colors.selection
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10
                        Label {
                            Layout.fillWidth: true
                            text: choice.nameOf(entry.modelData)
                            font.italic: entry.modelData.unfiled
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                        Label {
                            text: qsTr("%Ln books", "", entry.modelData.books)
                            color: Qt.alpha(theme.foreground, 0.6)
                            font.pixelSize: 12
                        }
                        Glyph {
                            visible: entry.modelData.id === graphTenants.current
                            name: "check"
                            size: 14
                        }
                    }
                    TapHandler { onTapped: choice.take(entry.modelData) }
                }
            }
            Label {
                visible: list.count === 0
                Layout.fillWidth: true
                text: qsTr("No collection goes by that")
                color: theme.colors.muted
            }
        }
    }
}
