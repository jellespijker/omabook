import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What just happened, said once at the bottom. It goes by itself after the
// time it takes to read (backend.messageLasts), held while the pointer rests
// on it, and a click or a newer message ends it sooner. What went wrong with
// work beside the window, or with bringing books in (#258), waits for the person instead, until it is clicked or
// a book is opened or closed (#233). A plugin's notice may carry a button that
// acts on it (omabook.ui.notify, #135), which dismisses it too.
Rectangle {
    id: toast
    objectName: "toast"

    visible: backend.message !== ""
    width: Math.min(parent.width - 2 * win.dialogEdge, row.implicitWidth + 40)
    height: row.implicitHeight + 24
    radius: theme.radius
    color: theme.colors.selection
    border.color: theme.colors.muted

    readonly property bool stays: visible && backend.messageLasts === 0

    // Counts again from the start: for each message, the same words included.
    function wait() {
        leaving.interval = backend.messageLasts
        if (leaving.interval > 0 && !resting.hovered) leaving.restart()
        else leaving.stop()
    }
    Component.onCompleted: wait()

    Timer {
        id: leaving
        objectName: "toastTimer"
        onTriggered: backend.dismissMessage()
    }
    Connections {
        target: backend
        function onMessageChanged() { toast.wait() }
    }
    Connections {
        target: win
        function onInReaderChanged() { if (toast.stays) backend.dismissMessage() }
    }
    HoverHandler {
        id: resting
        onHoveredChanged: toast.wait()
    }
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
                                 - (putAway.visible ? putAway.implicitWidth + 16 : 0)
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
        // Shows that this one waits to be put away.
        AppButton {
            id: putAway
            objectName: "toastClose"
            visible: toast.stays
            text: "✕"
            Accessible.name: qsTr("Close")
            focusPolicy: Qt.NoFocus
            onClicked: backend.dismissMessage()
        }
    }
}
