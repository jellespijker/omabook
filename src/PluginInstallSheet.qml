import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Installing a plugin someone else wrote: name its GitHub repository, or
// pick its .zip or its folder, see what it is and what it asks to be
// allowed, and only then say whether it goes in switched on. Nothing is put
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
        open()
        address.forceActiveFocus()
    }
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
