import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A recovery key just made (#185), this computer's or a destination's: shown
// this once, to copy and keep away from this computer, since without it
// nothing kept there can be restored.
ColumnLayout {
    id: shown
    objectName: "snapshotKey"
    readonly property string key: snapshotDesk.recoveryKey.key || ""
    readonly property string owner: snapshotDesk.recoveryKey["for"] || ""
    visible: key !== ""
    spacing: 8

    PlainCopy { id: copier }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.bold: true
        text: shown.owner === "" ? qsTr("The recovery key, shown this once")
                                 : qsTr("The recovery key of %1, shown this once").arg(shown.owner)
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        TextField {
            objectName: "snapshotKeyText"
            Layout.fillWidth: true
            readOnly: true
            selectByMouse: true
            font.family: "monospace"
            text: shown.key
            Material.roundedScale: theme.radius
        }
        AppButton {
            objectName: "snapshotKeyCopy"
            text: qsTr("Copy")
            onClicked: copier.copyText(shown.key)
        }
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.orange
        text: qsTr("Keep it away from this computer: printed, or in a password manager. Without it nothing kept there can be restored, on this computer or any other.")
    }
    AppButton {
        objectName: "snapshotKeyKept"
        text: qsTr("I Have Kept It")
        onClicked: snapshotDesk.forget()
    }
}
