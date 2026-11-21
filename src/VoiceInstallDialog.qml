import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Nothing on this computer reads aloud: on Omarchy, a voice is offered to
// install (#156), the way Omarchy's own apps install what they need, in a
// terminal that asks for the password there. A voice from Arch's own
// packages, or Piper's natural one from the AUR, said plainly to be from
// there; or neither. Nothing is installed unless one is chosen.
AppPopup {
    id: dialog
    objectName: "voiceInstall"
    readonly property var aloud: reader.readAloud

    widest: 560

    Connections {
        target: dialog.aloud
        function onOfferChanged() {
            if (dialog.aloud.offering)
                dialog.open()
            else
                dialog.close()
        }
    }
    // Escape, or a tap beside it, is Cancel.
    onClosed: dialog.aloud.declineInstall()
    onOpened: installVoice.forceActiveFocus()

    DialogBody {
        Label {
            Layout.fillWidth: true
            text: qsTr("Install a voice to read aloud?")
            font.pixelSize: 18
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Nothing on this computer reads aloud yet. A terminal opens to install the voice and asks for your password there; once it is closed, Omabook looks for the voice again.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "voiceInstallVoiceText"
            Layout.fillWidth: true
            Layout.topMargin: 4
            // "speech-dispatcher and espeak-ng"
            text: qsTr("%1 and %2, from Arch's own packages: small and quick, and it sounds like a machine.")
                  .arg(dialog.aloud.voicePackages[0] || "").arg(dialog.aloud.voicePackages[1] || "")
            wrapMode: Text.Wrap
        }
        AppButton {
            id: installVoice
            objectName: "voiceInstallVoice"
            Layout.fillWidth: win.compact
            primary: true
            text: qsTr("Install a voice")
            onClicked: dialog.aloud.install("voice")
        }
        Label {
            objectName: "voiceInstallPiperText"
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: dialog.aloud.piperInstallable
            // "piper-tts-bin and piper-voices-de-de"
            text: qsTr("%1 and %2, from the AUR, where Arch's users share packages that Arch itself doesn't check: larger, and it sounds like a person.")
                  .arg(dialog.aloud.piperPackages[0] || "").arg(dialog.aloud.piperPackages[1] || "")
            wrapMode: Text.Wrap
        }
        AppButton {
            objectName: "voiceInstallPiper"
            Layout.fillWidth: win.compact
            visible: dialog.aloud.piperInstallable
            text: qsTr("Install a natural voice (Piper)")
            onClicked: dialog.aloud.install("piper")
        }
        actions: [
            AppButton {
                objectName: "voiceInstallCancel"
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
        ]
    }
}
