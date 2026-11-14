import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Installing a plugin someone else wrote: pick its .zip or its folder, see
// what it is and what it asks to be allowed, and only then say whether it
// goes in switched on. Nothing is put in the plugin folder before that.
AppPopup {
    id: sheet
    objectName: "pluginInstall"
    property var host
    readonly property var offer: host ? host.manager.offer : ({})
    // Something was picked and looked at; and it is a plugin that can go in.
    readonly property bool looked: offer.source !== undefined || offer.error !== undefined
    readonly property bool fits: offer.name !== undefined
    signal installed(string id)

    widest: 540

    function show() {
        host.manager.dismissOffer()
        open()
        zip.forceActiveFocus()
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
            text: sheet.fits ? qsTr("Install %1?").arg(sheet.offer.title) : qsTr("Install a plugin")
            textFormat: Text.PlainText
            font.pixelSize: 18
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            visible: !sheet.looked
            Layout.fillWidth: true
            text: qsTr("A plugin comes as a .zip, or as a folder with a plugin.json in it. It is checked before anything is installed, and you see what it asks for first.")
            wrapMode: Text.Wrap
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
            visible: sheet.fits && text !== ""
            Layout.fillWidth: true
            text: sheet.fits ? sheet.offer.description : ""
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
            visible: sheet.fits && sheet.offer.installed === true
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
            AppButton { text: qsTr("Cancel"); onClicked: sheet.close() },
            AppButton {
                id: zip
                objectName: "pluginInstallZip"
                visible: !sheet.fits
                text: qsTr("From a .zip…")
                primary: !sheet.looked
                onClicked: sheet.host.manager.pick(false)
            },
            AppButton {
                objectName: "pluginInstallFolder"
                visible: !sheet.fits
                text: qsTr("From a Folder…")
                onClicked: sheet.host.manager.pick(true)
            },
            AppButton {
                objectName: "pluginInstallOff"
                visible: sheet.fits
                text: qsTr("Install, Leave Off")
                onClicked: sheet.install(false)
            },
            AppButton {
                objectName: "pluginInstallOn"
                visible: sheet.fits
                text: qsTr("Install and Switch On")
                primary: true
                onClicked: sheet.install(true)
            }
        ]
    }
}
