import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Installing a plugin someone else wrote: find it (in the registry Omabook
// keeps, or on GitHub by the topic omabook-plugin), name its GitHub
// repository, or pick its .zip or its folder; see what it is and what it
// asks to be allowed, and only then say whether it goes in switched on. Nothing is put
// in the plugin folder, and nothing of it runs, before that. The same sheet
// shows an update that asks for more than the version installed, to be
// agreed to or left.
AppPopup {
    id: sheet
    objectName: "pluginInstall"
    property var host
    readonly property var offer: host ? host.manager.offer : ({})
    // Something was picked and looked at; and it is a plugin that can go in.
    readonly property bool looked: offer.source !== undefined || offer.error !== undefined
    readonly property bool fits: offer.name !== undefined
    // It is a newer version of a plugin that is installed, shown for approval.
    readonly property bool updating: fits && offer.update === true
    readonly property bool fetching: host ? host.manager.fetching : false
    signal installed(string id)
    // What a script looks to add, as the words for it.
    readonly property var addNames: ({
        source: qsTr("a source of book details"), catalog: qsTr("a catalog format"), format: qsTr("a book format"),
        importer: qsTr("a say in books that are added"), command: qsTr("commands"), panel: qsTr("a panel"),
        hook: qsTr("hooks on what happens to books")
    })
    function channelText(channel) {
        if (!channel || channel.kind === "auto" || channel.kind === undefined) return qsTr("follows the latest release, or the main branch while there is none")
        return channel.kind === "tag" ? qsTr("stays at %1").arg(channel.name) : qsTr("follows the branch %1").arg(channel.name)
    }

    widest: 540

    function show() {
        host.manager.dismissOffer()
        address.text = ""
        findWords.text = ""
        host.manager.find("")
        open()
        findWords.forceActiveFocus()
    }
    // Installing what was found goes the way an address does: fetched,
    // checked, and shown with what it asks for, for a yes or a no.
    function take(found) {
        if (!fetching && found.installable && !found.installed)
            host.manager.lookUp(found.address)
    }
    readonly property var found: host ? host.manager.found : []
    readonly property bool finding: host ? host.manager.finding : false
    Timer { id: findSoon; interval: 300; onTriggered: sheet.host.manager.find(findWords.text) }
    // Opened by the manager for an update that waits to be agreed to; what
    // is on offer is left as it is.
    function review() {
        if (!opened)
            open()
    }
    function fetch() {
        if (address.text.trim() !== "" && !fetching)
            host.manager.lookUp(address.text)
    }
    function install(enable) {
        const answer = host.manager.install(enable, sheet.offer.installed === true)
        if (answer.error)
            return
        close()
        installed(answer.id)
    }
    onClosed: if (host) host.manager.dismissOffer()
    // Typing finds, once the sheet is up and has the keyboard.
    onOpened: if (!fits) findWords.forceActiveFocus()

    DialogBody {

        Label {
            objectName: "pluginInstallTitle"
            Layout.fillWidth: true
            // "Install Reading Log?"
            text: sheet.updating ? qsTr("Update %1 to version %2?").arg(sheet.offer.title).arg(sheet.offer.version)
                  : sheet.fits ? qsTr("Install %1?").arg(sheet.offer.title) : qsTr("Install a plugin")
            textFormat: Text.PlainText
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        // Finding one: the registry's first, then what GitHub has by its topic.
        TextField {
            id: findWords
            objectName: "pluginFindWords"
            visible: !sheet.fits
            enabled: !sheet.fetching
            Layout.fillWidth: true
            placeholderText: qsTr("Find plugins")
            selectByMouse: true
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            Material.roundedScale: theme.radius
            onTextEdited: findSoon.restart()
            // Enter takes the first that can be installed.
            onAccepted: {
                const first = sheet.found.find(one => one.installable && !one.installed)
                if (first) sheet.take(first)
            }
        }
        Thinking {
            objectName: "pluginFinding"
            active: sheet.finding && !sheet.fits
            text: qsTr("Looking for plugins…")
            room: parent ? parent.width : 400
        }
        Repeater {
            model: sheet.fits ? [] : sheet.found
            delegate: Rectangle {
                id: result
                required property var modelData
                required property int index
                objectName: "pluginFound_" + modelData.name
                Layout.fillWidth: true
                implicitHeight: lines.implicitHeight + 20
                radius: theme.radius
                color: "transparent"
                border.color: theme.colors.selection
                ColumnLayout {
                    id: lines
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: result.modelData.title
                            textFormat: Text.PlainText
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        // Listed in the registry, or only found by its topic.
                        Rectangle {
                            objectName: "pluginFoundMark"
                            implicitWidth: mark.implicitWidth + 12
                            implicitHeight: mark.implicitHeight + 4
                            radius: Math.min(4, theme.radius)
                            color: "transparent"
                            border.color: result.modelData.listed ? theme.accent : theme.colors.orange
                            Label {
                                id: mark
                                anchors.centerIn: parent
                                text: result.modelData.listed ? qsTr("Listed") : qsTr("Not reviewed")
                                color: result.modelData.listed ? theme.accent : theme.colors.orange
                                font.pixelSize: 11
                            }
                        }
                        AppButton {
                            objectName: "pluginFoundInstall_" + result.modelData.name
                            visible: !result.modelData.installed
                            enabled: result.modelData.installable && !sheet.fetching
                            text: qsTr("Install")
                            primary: result.index === 0
                            onClicked: sheet.take(result.modelData)
                        }
                        Label {
                            visible: result.modelData.installed === true
                            text: qsTr("Installed")
                            color: sheet.host ? sheet.host.quiet : theme.foreground
                        }
                    }
                    Label {
                        visible: text !== ""
                        Layout.fillWidth: true
                        text: result.modelData.description || ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        // "by Someone · 12 stars · updated 2026-11-01 · MIT · jellespijker/omabook-storyteller"
                        text: {
                            translator.language
                            const one = result.modelData
                            return [one.author ? qsTr("by %1").arg(one.author) : "",
                                    one.stars !== undefined ? qsTr("%Ln stars", "", one.stars) : "",
                                    one.updated ? qsTr("updated %1").arg(one.updated) : "",
                                    one.license || "", one.repository].filter(part => part !== "").join(" · ")
                        }
                        textFormat: Text.PlainText
                        color: sheet.host ? sheet.host.quiet : theme.foreground
                        font.pixelSize: 12
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                    Label {
                        Layout.fillWidth: true
                        text: result.modelData.program ? qsTr("It is a program of its own: it can do whatever you can on this computer.")
                              : qsTr("It may use: %1").arg(sheet.host ? sheet.host.permissionText(result.modelData.permissions || []) : "")
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Label {
                        visible: !result.modelData.listed
                        Layout.fillWidth: true
                        text: qsTr("Nobody has reviewed it for you. A plugin can do whatever its permissions allow.")
                        textFormat: Text.PlainText
                        color: theme.colors.orange
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Label {
                        visible: !result.modelData.installable && (result.modelData.problem || "") !== ""
                        Layout.fillWidth: true
                        text: result.modelData.problem || ""
                        textFormat: Text.PlainText
                        color: theme.colors.orange
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
                TapHandler { onDoubleTapped: sheet.take(result.modelData) }
            }
        }
        Label {
            objectName: "pluginFoundNone"
            visible: !sheet.fits && !sheet.finding && sheet.found.length === 0
            Layout.fillWidth: true
            text: findWords.text.trim() === "" ? qsTr("No plugins to try were found.") : qsTr("No plugin was found for “%1”.").arg(findWords.text.trim())
            textFormat: Text.PlainText
            color: sheet.host ? sheet.host.quiet : theme.foreground
            wrapMode: Text.Wrap
        }
        Label {
            visible: !sheet.fits && text !== ""
            Layout.fillWidth: true
            text: sheet.host ? sheet.host.manager.findProblem : ""
            textFormat: Text.PlainText
            color: sheet.host ? sheet.host.quiet : theme.foreground
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        Switch {
            objectName: "pluginDiscovery"
            visible: !sheet.fits
            Layout.fillWidth: true
            // A switch is dense in Material; under fingers it is a fingertip tall.
            Layout.preferredHeight: Math.max(implicitHeight, win.touch ? 40 : 0)
            text: qsTr("Look for plugins on GitHub")
            checked: sheet.host ? sheet.host.manager.discovery : true
            onToggled: {
                sheet.host.manager.discovery = checked
                sheet.host.manager.find(findWords.text)
            }
        }
        Rectangle {
            visible: !sheet.fits
            Layout.fillWidth: true
            Layout.topMargin: 4
            implicitHeight: 1
            color: theme.colors.selection
        }
        Label {
            visible: !sheet.fits
            Layout.fillWidth: true
            text: qsTr("A plugin comes from its GitHub repository, as a .zip, or as a folder with a plugin.json in it. It is checked before anything is installed, and you see what it asks for first. One from GitHub is kept up to date from there.")
            wrapMode: Text.Wrap
        }
        TextField {
            id: address
            objectName: "pluginInstallAddress"
            visible: !sheet.fits
            enabled: !sheet.fetching
            Layout.fillWidth: true
            // "GitHub repository: owner/name, or a link to it"
            placeholderText: qsTr("GitHub repository: owner/name, or a link to it")
            selectByMouse: true
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhUrlCharactersOnly
            Material.roundedScale: theme.radius
            onAccepted: sheet.fetch()
        }
        Label {
            visible: !sheet.fits && !sheet.fetching
            Layout.fillWidth: true
            text: qsTr("Public repositories; a private one needs a token, given with `omabook plugin token`.")
            textFormat: Text.PlainText
            color: sheet.host ? sheet.host.quiet : theme.foreground
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        Thinking {
            objectName: "pluginInstallFetching"
            active: sheet.fetching
            text: qsTr("Fetching it from GitHub…")
            room: parent ? parent.width : 400
        }

        // What was picked, as it describes itself.
        Label {
            visible: sheet.fits && text !== ""
            Layout.fillWidth: true
            text: !sheet.fits ? "" : [sheet.offer.version ? qsTr("version %1").arg(sheet.offer.version) : "",
                                      sheet.offer.author ? qsTr("by %1").arg(sheet.offer.author) : ""].filter(part => part !== "").join(" · ")
            textFormat: Text.PlainText
            color: sheet.host ? sheet.host.quiet : theme.foreground
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "pluginInstallOrigin"
            visible: sheet.fits && sheet.offer.from === "github"
            Layout.fillWidth: true
            // "From GitHub: someone/storyteller, main at ee850f7; it follows the latest release"
            text: !visible ? "" : qsTr("From GitHub: %1, %2 at %3; it %4").arg(sheet.offer.repository + (sheet.offer.subfolder ? "/" + sheet.offer.subfolder : ""))
                                      .arg(sheet.offer.ref).arg(String(sheet.offer.commit).substring(0, 7)).arg(sheet.channelText(sheet.offer.channel))
            textFormat: Text.PlainText
            color: sheet.host ? sheet.host.quiet : theme.foreground
            font.pixelSize: 12
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }
        Label {
            visible: sheet.fits && text !== ""
            Layout.fillWidth: true
            text: sheet.fits ? sheet.offer.description : ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "pluginInstallAdds"
            visible: sheet.fits && !sheet.updating && (sheet.offer.adds || []).length > 0
            Layout.fillWidth: true
            // Read from its script, which was not run to find out.
            text: !visible ? "" : qsTr("It looks to add: %1").arg(sheet.offer.adds.map(what => sheet.addNames[what] || what).join(", "))
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "pluginInstallNew"
            visible: sheet.updating
            Layout.fillWidth: true
            // What is different about this version, which is why it is asked.
            text: !sheet.updating ? ""
                  : [(sheet.offer.newPermissions || []).length > 0 ? qsTr("New in this version: it asks to use %1.").arg(sheet.host.permissionText(sheet.offer.newPermissions)) : "",
                     sheet.offer.renamedFrom ? qsTr("It was called %1 until now.").arg(sheet.offer.renamedFrom) : "",
                     qsTr("Version %1 is installed. It stays on or off as it is, keeps what it saved, and can be rolled back.").arg(sheet.offer.fromVersion)]
                        .filter(part => part !== "").join(" ")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        RowLayout {
            visible: sheet.fits
            Layout.fillWidth: true
            spacing: 8
            PluginMark {
                Layout.alignment: Qt.AlignTop
                // A program, or a script that starts programs, is trusted with everything.
                condition: sheet.fits && (sheet.offer.kind === "program" || sheet.offer.permissions.indexOf("run") >= 0) ? "needs" : "off"
            }
            Label {
                objectName: "pluginInstallPermissions"
                Layout.fillWidth: true
                text: sheet.fits ? qsTr("It may use: %1").arg(sheet.host.permissionText(sheet.offer.permissions)) : ""
                wrapMode: Text.Wrap
            }
        }
        Label {
            visible: sheet.fits && !sheet.updating && sheet.offer.installed === true
            Layout.fillWidth: true
            text: sheet.fits ? qsTr("A plugin called %1 is already installed. This one takes its place, and the one there goes to the trash.").arg(sheet.offer.name) : ""
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "pluginInstallError"
            visible: text !== ""
            Layout.fillWidth: true
            text: sheet.offer.error || ""
            textFormat: Text.PlainText
            color: theme.colors.orange
            wrapMode: Text.Wrap
        }

        // The one that acts comes last, at the end of the row.
        actions: [
            AppButton { text: sheet.updating ? qsTr("Not Now") : qsTr("Cancel"); onClicked: sheet.close() },
            AppButton {
                id: zip
                objectName: "pluginInstallZip"
                visible: !sheet.fits
                enabled: !sheet.fetching
                text: qsTr("From a .zip…")
                onClicked: sheet.host.manager.pick(false)
            },
            AppButton {
                objectName: "pluginInstallFolder"
                visible: !sheet.fits
                enabled: !sheet.fetching
                text: qsTr("From a Folder…")
                onClicked: sheet.host.manager.pick(true)
            },
            AppButton {
                objectName: "pluginInstallFetch"
                visible: !sheet.fits
                enabled: address.text.trim() !== "" && !sheet.fetching
                text: qsTr("Fetch")
                primary: true
                onClicked: sheet.fetch()
            },
            AppButton {
                objectName: "pluginInstallOff"
                visible: sheet.fits && !sheet.updating
                text: qsTr("Install, Leave Off")
                onClicked: sheet.install(false)
            },
            AppButton {
                objectName: "pluginInstallOn"
                visible: sheet.fits && !sheet.updating
                text: qsTr("Install and Switch On")
                primary: true
                onClicked: sheet.install(true)
            },
            AppButton {
                objectName: "pluginInstallUpdate"
                visible: sheet.updating
                text: qsTr("Update")
                primary: true
                onClicked: sheet.install(true)
            }
        ]
    }
}
