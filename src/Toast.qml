import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What just happened, said once at the bottom and dismissed by a click or by
// the next thing that happens; a plugin's notice may carry a button that acts
// on it (omabook.ui.notify, #135), which dismisses it too.
Rectangle {
    id: toast
    objectName: "toast"

    visible: backend.message !== ""
    width: Math.min(parent.width - 2 * win.dialogEdge, row.implicitWidth + 40)
    height: row.implicitHeight + 24
    radius: theme.radius
    color: theme.colors.selection
    border.color: theme.colors.muted

    TapHandler { onTapped: backend.dismissMessage() }

    RowLayout {
        id: row
        anchors.centerIn: parent
        width: Math.min(implicitWidth, toast.width - 40)
        spacing: 16
        Label {
            id: toastText
            objectName: "toastText"
            Layout.fillWidth: true
            Layout.maximumWidth: toast.parent.width - 2 * win.dialogEdge - 40 - (act.visible ? act.implicitWidth + 16 : 0)
            text: backend.message
            wrapMode: Text.Wrap
            horizontalAlignment: act.visible ? Text.AlignLeft : Text.AlignHCenter
        }
        AppButton {
            id: act
            objectName: "toastAction"
            visible: backend.messageAction !== ""
            text: backend.messageAction
            focusPolicy: Qt.NoFocus
            onClicked: backend.actOnMessage()
        }
    }
}
