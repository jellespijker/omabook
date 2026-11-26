import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Where else the snapshots are kept (#185, #182): each destination, how its
// last copy went, a test that opens it with its own password, and taking it
// off the list (what is kept there stays), asked first; then adding one, a
// folder on a second disk or a place restic reaches, with the credentials it
// needs, kept as secrets and never shown again. Its recovery key is shown once.
ColumnLayout {
    id: page
    required property SnapshotWords words
    // The destination asked to be taken off, until that is confirmed or left.
    property string removing: ""
    // The credentials being typed for a new one: [{name, value}].
    property var credentials: []
    spacing: 8

    // Added: its recovery key is shown, and the form is empty for another.
    Connections {
        target: snapshotDesk
        function onChanged() {
            if (!snapshotDesk.recoveryKey["for"] || snapshotDesk.recoveryKey["for"] !== name.text.trim())
                return
            name.text = ""
            location.text = ""
            page.credentials = []
        }
    }

    component Note: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
    }

    Note {
        visible: snapshotDesk.destinations.length === 0
        text: qsTr("The snapshots are only on this computer yet. Add a second disk, and a place away from home, for three copies on two kinds of medium with one away from home.")
    }
    Repeater {
        model: snapshotDesk.destinations
        ColumnLayout {
            id: row
            objectName: "snapshotDestination"
            required property var modelData
            readonly property bool removing: page.removing === modelData.name
            readonly property bool tested: snapshotDesk.tested.name === modelData.name
            Layout.fillWidth: true
            spacing: 4
            Label {
                Layout.fillWidth: true
                text: row.modelData.name
                font.bold: true
                elide: Text.ElideRight
            }
            SnapshotDestinationState {
                Layout.fillWidth: true
                destination: row.modelData
                words: page.words
            }
            Label {
                objectName: "snapshotDestinationTested"
                visible: row.tested && snapshotDesk.tested.reached === true
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: snapshotDesk.tested.latest ? qsTr("Reached. Its newest snapshot: %1.").arg(page.words.named(snapshotDesk.tested.latest))
                                                 : qsTr("Reached. It holds no snapshot yet.")
            }
            Label {
                visible: row.removing
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.orange
                text: qsTr("Stop copying the snapshots to %1? What is kept there stays, and its recovery key still opens it.").arg(row.modelData.name)
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                AppButton {
                    objectName: "snapshotDestinationTest"
                    visible: !row.removing
                    enabled: snapshotDesk.doing === ""
                    text: qsTr("Test")
                    onClicked: snapshotDesk.testDestination(row.modelData.name)
                }
                AppButton {
                    objectName: "snapshotDestinationRemove"
                    visible: !row.removing
                    text: qsTr("Remove…")
                    onClicked: page.removing = row.modelData.name
                }
                AppButton {
                    objectName: "snapshotDestinationRemoveYes"
                    visible: row.removing
                    enabled: snapshotDesk.doing === ""
                    text: qsTr("Remove")
                    onClicked: {
                        page.removing = ""
                        snapshotDesk.removeDestination(row.modelData.name)
                    }
                }
                AppButton {
                    visible: row.removing
                    text: qsTr("Keep")
                    onClicked: page.removing = ""
                }
            }
        }
    }

    Label { Layout.topMargin: 8; text: qsTr("Add a destination"); font.bold: true }
    TextField {
        id: name
        objectName: "snapshotDestinationName"
        Layout.fillWidth: true
        placeholderText: qsTr("A name: lower-case letters, digits and dashes")
        selectByMouse: true
        Material.roundedScale: theme.radius
    }
    TextField {
        id: location
        objectName: "snapshotDestinationLocation"
        Layout.fillWidth: true
        placeholderText: qsTr("Where: a folder on a second disk, another of your hubs (hub:<its host>), or sftp:host:/path, s3:…, b2:…, rest:…, rclone:…")
        selectByMouse: true
        Material.roundedScale: theme.radius
    }
    Repeater {
        model: page.credentials.length
        GridLayout {
            id: credential
            required property int index
            Layout.fillWidth: true
            columns: win.compact ? 1 : 2
            columnSpacing: 8
            TextField {
                objectName: "snapshotCredentialName"
                Layout.fillWidth: true
                placeholderText: qsTr("Its name, as restic takes it: AWS_SECRET_ACCESS_KEY…")
                selectByMouse: true
                Material.roundedScale: theme.radius
                onTextEdited: page.credentials[credential.index].name = text
            }
            TextField {
                objectName: "snapshotCredentialValue"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: qsTr("Its value, kept as a secret")
                Material.roundedScale: theme.radius
                onTextEdited: page.credentials[credential.index].value = text
            }
        }
    }
    Note {
        text: qsTr("A place away from home may need credentials: each is handed to restic by its name, and kept as a secret on this computer, never in the settings.")
    }
    DialogActions {
        AppButton {
            objectName: "snapshotCredentialAdd"
            text: qsTr("Add a Credential")
            onClicked: page.credentials = page.credentials.concat([{ name: "", value: "" }])
        }
        AppButton {
            objectName: "snapshotDestinationAdd"
            enabled: snapshotDesk.doing === "" && name.text.trim() !== "" && location.text.trim() !== ""
            primary: true
            text: qsTr("Add Destination")
            onClicked: {
                snapshotDesk.addDestination(name.text, location.text, page.credentials.map(each => each.name),
                                            page.credentials.map(each => each.value))
            }
        }
    }
}
