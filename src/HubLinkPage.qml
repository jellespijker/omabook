import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Linking this computer to a hub (#189, #180): first where the hub is, as
// ssh reaches it, and what it is to call this computer; then what linking
// does there, to say yes to. Omabook never asks for a password, nor sends
// one: the person's own ssh must already let them in.
ColumnLayout {
    id: page
    property bool busy: false
    // 0: where the hub is; 1: what linking does, for a yes.
    property int step: 0
    // Linking was asked, and its answer hasn't come.
    property bool waiting: false
    readonly property string host: hostField.text.trim()
    readonly property string computer: nameField.text.trim() !== "" ? nameField.text.trim() : hubsDesk.thisComputer
    signal linked()
    spacing: 8

    function begin() {
        step = 0
        waiting = false
        hostField.forceActiveFocus()
    }
    function next() {
        if (host === "")
            return
        step = 1
        hubsDesk.clear()
        linkButton.forceActiveFocus()
    }
    function link() {
        waiting = true
        hubsDesk.link(host, computer)
    }

    Connections {
        target: hubsDesk
        function onChanged() {
            if (!page.waiting || hubsDesk.doing !== "")
                return
            page.waiting = false
            if (!hubsDesk.failed) {
                hostField.text = ""
                nameField.text = ""
                page.linked()
            }
        }
    }
    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
    }

    // Where the hub is.
    Label { visible: page.step === 0; text: qsTr("Where the hub is"); font.bold: true }
    TextField {
        id: hostField
        objectName: "hubHost"
        visible: page.step === 0
        Layout.fillWidth: true
        placeholderText: qsTr("hub, omabook@hub, or hub.example.ts.net")
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        selectByMouse: true
        Material.roundedScale: theme.radius
        onAccepted: page.next()
    }
    Note {
        visible: page.step === 0
        text: qsTr("Its name on your network or tailnet, as ssh reaches it; with the hub's user in front when it isn't yours.")
    }
    Label { visible: page.step === 0; text: qsTr("What the hub calls this computer"); font.bold: true }
    TextField {
        id: nameField
        objectName: "hubComputerName"
        visible: page.step === 0
        Layout.fillWidth: true
        placeholderText: hubsDesk.thisComputer
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        selectByMouse: true
        Material.roundedScale: theme.radius
        onAccepted: page.next()
    }
    AppButton {
        objectName: "hubNext"
        visible: page.step === 0
        enabled: page.host !== ""
        primary: true
        text: qsTr("Next")
        onClicked: page.next()
    }

    // What linking does there, before the yes.
    Label {
        objectName: "hubConsent"
        visible: page.step === 1
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: qsTr("Linking signs in to %1 once, over ssh, as you do yourself: with your own ssh key or agent. Omabook never asks for a password, nor sends one, so ssh must already let you in there.").arg(page.host)
    }
    Note {
        visible: page.step === 1
        color: theme.foreground
        textFormat: Text.PlainText
        text: qsTr("There it installs a key of this computer's own, as %1, signed by your identity. That key opens only the hub's session, never a shell, and the hub takes only your own computers. Unlink takes it off again.").arg(page.computer)
    }
    RowLayout {
        visible: page.step === 1
        Layout.fillWidth: true
        spacing: 8
        AppButton {
            id: linkButton
            objectName: "hubLinkYes"
            enabled: !page.busy
            primary: true
            text: qsTr("Link This Computer")
            onClicked: page.link()
        }
        AppButton {
            objectName: "hubLinkBack"
            enabled: !page.busy
            text: qsTr("Change")
            onClicked: page.begin()
        }
    }
}
