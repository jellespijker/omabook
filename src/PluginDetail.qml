import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Everything about one plugin: what it is and where it is from, what it
// adds and may use, what went wrong in it, and what can be done with it.
// The plugin manager (PluginsDialog) shows it beside its list, or in place
// of the list when the window is narrow.
Item {
    id: pane
    // The plugin, as PluginManager describes it, and the dialog it is in.
    property var plugin: null
    property var host
    // Whether it stands in place of the list, and so needs a way back.
    property bool alone: false
    readonly property bool has: plugin !== null && plugin !== undefined
    readonly property bool typing: tryField.activeFocus
    // The look-up being tried, when it is this plugin's.
    readonly property var trial: has && host.manager.trial.id === plugin.id ? host.manager.trial : ({})

    function focusTry() { if (tryRow.visible) tryField.forceActiveFocus() }
    function tryIt() {
        if (!has || !plugin.tryable)
            return
        if (trial.asking) host.manager.stopTrial()
        else host.manager.tryOut(plugin.id, tryField.text)
    }

    // What a script registered, one line each.
    function addedLine(added) {
        const places = ({ library: qsTr("in the library"), reader: qsTr("while reading"), global: qsTr("anywhere") })
        switch (added.what) {
        case "source": return qsTr("A source of book details: %1").arg(added.name)
        case "catalog": return qsTr("A catalog format: %1").arg(added.name)
        // "A book format: Text books (txtbook, txtz)"
        case "format": return added.detail ? qsTr("A book format: %1 (%2)").arg(added.name).arg(added.detail) : qsTr("A book format: %1").arg(added.name)
        case "importer": return qsTr("A say in every book that is added")
        case "command":
            // "A command: Shelve it (Ctrl+R, in the library)"
            return added.key ? qsTr("A command: %1 (%2, %3)").arg(added.name).arg(added.key).arg(places[added.place] || added.place)
                             : qsTr("A command: %1 (%2)").arg(added.name).arg(places[added.place] || added.place)
        case "panel": return qsTr("A panel: %1").arg(added.name || added.place)
        case "hook": return qsTr("Runs when: %1").arg(host.eventNames[added.name] || added.name)
        }
        return added.name
    }

    Label {
        visible: !pane.has
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 320)
        text: qsTr("No plugin goes by that. Clear the filter to see them all.")
        color: host.quiet
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
    }

    Flickable {
        id: flick
        objectName: "pluginDetailFlick"
        visible: pane.has
        anchors.fill: parent
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            // Room for the scroll bar, so it never lies over the text.
            width: flick.width - 14
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                AppButton {
                    objectName: "pluginBack"
                    visible: pane.alone
                    focusPolicy: Qt.TabFocus
                    text: qsTr("Back")
                    onClicked: host.detailShown = false
                }
                PluginMark {
                    condition: pane.has ? pane.plugin.state : "on"
                    size: 18
                }
                Label {
                    objectName: "pluginDetailName"
                    Layout.fillWidth: true
                    text: pane.has ? pane.plugin.name : ""
                    textFormat: Text.PlainText
                    font.pixelSize: 20
                    font.bold: true
                    wrapMode: Text.Wrap
                }
                Switch {
                    objectName: "pluginDetailSwitch"
                    // As tall as a fingertip under fingers.
                    Layout.preferredHeight: Math.max(implicitHeight, win.touch ? 40 : 0)
                    visible: pane.has && pane.plugin.switchable
                    checked: pane.has && pane.plugin.enabled
                    focusPolicy: Qt.TabFocus
                    Accessible.name: qsTr("Switched on")
                    onToggled: host.toggle(pane.plugin)
                }
            }

            Label {
                objectName: "pluginDetailOrigin"
                Layout.fillWidth: true
                // "Built in · version 1.2 · by Omabook"
                text: !pane.has ? "" : [host.origins[pane.plugin.origin] || pane.plugin.origin,
                                         pane.plugin.version ? qsTr("version %1").arg(pane.plugin.version) : "",
                                         pane.plugin.author ? qsTr("by %1").arg(pane.plugin.author) : "",
                                         pane.plugin.state === "on" ? "" : host.stateNames[pane.plugin.state]]
                                        .filter(part => part !== "").join(" · ")
                textFormat: Text.PlainText
                color: host.quiet
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            // What went wrong in a script, with where, and what to do about it.
            Rectangle {
                id: errorBox
                objectName: "pluginError"
                readonly property var error: pane.has && pane.plugin.error ? pane.plugin.error : null
                visible: error !== null
                Layout.fillWidth: true
                implicitHeight: errorColumn.implicitHeight + 24
                radius: theme.radius
                color: "transparent"
                border.color: theme.colors.red

                ColumnLayout {
                    id: errorColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6
                    Label {
                        objectName: "pluginErrorMessage"
                        Layout.fillWidth: true
                        text: errorBox.error ? errorBox.error.message : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Label {
                        objectName: "pluginErrorWhere"
                        Layout.fillWidth: true
                        visible: text !== ""
                        // "main.js, line 12"
                        text: !errorBox.error || !errorBox.error.file ? ""
                              : qsTr("%1, line %2").arg(errorBox.error.file).arg(errorBox.error.line)
                              + (errorBox.error.kept ? " · " + qsTr("The last version that worked is still running.") : "")
                        textFormat: Text.PlainText
                        color: host.quiet
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        AppButton {
                            objectName: "pluginRetry"
                            focusPolicy: Qt.TabFocus
                            text: qsTr("Try Again")
                            onClicked: host.manager.retry(pane.plugin.id)
                        }
                        AppButton {
                            objectName: "pluginDismiss"
                            focusPolicy: Qt.TabFocus
                            text: qsTr("Dismiss")
                            onClicked: host.manager.dismissError(pane.plugin.id)
                        }
                    }
                }
            }

            // Why it can't be used as it is, when that is not a script's error.
            RowLayout {
                visible: pane.has && !errorBox.visible && !!pane.plugin.note
                Layout.fillWidth: true
                spacing: 8
                PluginMark {
                    Layout.alignment: Qt.AlignTop
                    condition: pane.has && pane.plugin.state === "broken" ? "broken" : "needs"
                    visible: pane.has && pane.plugin.state !== "off"
                }
                Label {
                    objectName: "pluginNote"
                    Layout.fillWidth: true
                    text: pane.has ? pane.plugin.note || "" : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }

            Label {
                objectName: "pluginDetailDescription"
                Layout.fillWidth: true
                visible: text !== ""
                text: pane.has ? pane.plugin.description : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }

            // The facts, each under what it is: short labels, plain values.
            GridLayout {
                Layout.fillWidth: true
                columns: pane.width < 420 ? 1 : 2
                columnSpacing: 16
                rowSpacing: columns === 1 ? 2 : 10

                Label {
                    visible: brings.visible
                    Layout.alignment: Qt.AlignTop
                    text: qsTr("Brings")
                    color: host.quiet
                }
                Label {
                    id: brings
                    objectName: "pluginBrings"
                    visible: pane.has && pane.plugin.fields.length > 0
                    Layout.fillWidth: true
                    Layout.bottomMargin: parent.columns === 1 ? 8 : 0
                    text: pane.has ? host.fieldList(pane.plugin.fields) : ""
                    wrapMode: Text.Wrap
                }

                Label {
                    visible: order.visible
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Asked")
                    color: host.quiet
                }
                RowLayout {
                    id: order
                    visible: pane.has && pane.plugin.position !== undefined
                    Layout.fillWidth: true
                    Layout.bottomMargin: parent.columns === 1 ? 8 : 0
                    spacing: 8
                    Label {
                        objectName: "pluginOrder"
                        Layout.fillWidth: true
                        // "2 of 6": where it stands among the sources of details
                        text: order.visible ? qsTr("%1 of %2").arg(pane.plugin.position).arg(pane.plugin.of) : ""
                        elide: Text.ElideRight
                    }
                    AppButton {
                        objectName: "pluginEarlier"
                        focusPolicy: Qt.TabFocus
                        enabled: order.visible && pane.plugin.position > 1
                        text: qsTr("Earlier")
                        onClicked: host.manager.move(pane.plugin.id, -1)
                    }
                    AppButton {
                        objectName: "pluginLater"
                        focusPolicy: Qt.TabFocus
                        enabled: order.visible && pane.plugin.position < pane.plugin.of
                        text: qsTr("Later")
                        onClicked: host.manager.move(pane.plugin.id, 1)
                    }
                }

                Label {
                    visible: mayUse.visible
                    Layout.alignment: Qt.AlignTop
                    text: qsTr("May use")
                    color: host.quiet
                }
                Label {
                    id: mayUse
                    objectName: "pluginPermissions"
                    visible: pane.has && (pane.plugin.kind === "script" || pane.plugin.group === "program"
                                          || (pane.plugin.origin === "folder" && pane.plugin.kind === "metadata"))
                    Layout.fillWidth: true
                    Layout.bottomMargin: parent.columns === 1 ? 8 : 0
                    text: !visible ? "" : pane.plugin.kind === "script" ? host.permissionText(pane.plugin.permissions)
                                                                        : host.permissionText(["program"])
                    wrapMode: Text.Wrap
                }

                Label {
                    visible: adds.visible
                    Layout.alignment: Qt.AlignTop
                    text: qsTr("Adds")
                    color: host.quiet
                }
                Label {
                    id: adds
                    objectName: "pluginAdds"
                    visible: pane.has && pane.plugin.kind === "script" && pane.plugin.state !== "off"
                    Layout.fillWidth: true
                    Layout.bottomMargin: parent.columns === 1 ? 8 : 0
                    text: !visible ? "" : (pane.plugin.adds || []).length > 0 ? pane.plugin.adds.map(added => pane.addedLine(added)).join("\n")
                          : pane.plugin.running ? qsTr("Nothing yet: it registered no source, command, panel or hook.")
                                                : qsTr("Nothing, while it isn't running.")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    lineHeight: 1.15
                }

                Label {
                    visible: where.visible
                    Layout.alignment: Qt.AlignTop
                    text: qsTr("Folder")
                    color: host.quiet
                }
                Label {
                    id: where
                    objectName: "pluginFolder"
                    visible: pane.has && !!pane.plugin.folder
                    Layout.fillWidth: true
                    text: pane.has ? pane.plugin.folder : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
            }

            // One look-up, to see that a source answers and what it brings.
            ColumnLayout {
                id: tryRow
                visible: pane.has && pane.plugin.tryable
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        id: tryField
                        objectName: "pluginTryField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("A title to look up")
                        text: host.manager.sampleTitle
                        selectByMouse: true
                        focusPolicy: Qt.ClickFocus
                        Material.roundedScale: theme.radius
                        onAccepted: pane.tryIt()
                    }
                    AppButton {
                        objectName: "pluginTry"
                        focusPolicy: Qt.TabFocus
                        enabled: tryField.text.trim() !== ""
                        text: pane.trial.asking ? qsTr("Stop") : qsTr("Try It")
                        onClicked: pane.tryIt()
                    }
                }
                Label {
                    objectName: "pluginTryState"
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: pane.trial.asking ? qsTr("Asking…")
                          : !pane.trial.answered ? ""
                          : pane.trial.problem ? pane.trial.problem
                          : pane.trial.results.length === 0 ? qsTr("It answered, and found nothing for “%1”.").arg(pane.trial.title)
                          : qsTr("It answered with %Ln books:", "", pane.trial.results.length)
                    textFormat: Text.PlainText
                    color: pane.trial.problem && !pane.trial.asking ? theme.colors.orange : host.quiet
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: pane.trial.results || []
                    delegate: ColumnLayout {
                        id: found
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            objectName: "pluginTryResult"
                            Layout.fillWidth: true
                            // "Pride and Prejudice · Jane Austen · 1813"
                            text: [found.modelData.title, found.modelData.authors, found.modelData.published]
                                      .filter(part => part !== "").join(" · ")
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: found.modelData.brings.length > 0
                            text: qsTr("Brings: %1").arg(host.fieldList(found.modelData.brings))
                            color: host.quiet
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            // What can be done with one of the user's own.
            Flow {
                visible: pane.has && pane.plugin.removable
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                spacing: 8
                AppButton {
                    objectName: "pluginOpenFolder"
                    focusPolicy: Qt.TabFocus
                    text: qsTr("Open Folder")
                    onClicked: host.manager.openFolder(pane.plugin.id)
                }
                AppButton {
                    objectName: "pluginReloadOne"
                    visible: pane.has && pane.plugin.kind === "script" && !errorBox.visible
                    focusPolicy: Qt.TabFocus
                    text: qsTr("Load Again")
                    onClicked: host.manager.retry(pane.plugin.id)
                }
                AppButton {
                    objectName: "pluginRemove"
                    focusPolicy: Qt.TabFocus
                    text: qsTr("Remove…")
                    onClicked: host.askRemove(pane.plugin)
                }
            }
        }
    }
}
