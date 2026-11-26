import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// In the shelf's place when the library can't be opened: why, and what can be
// done. One that isn't there (#221) can be looked for again or, only when
// asked here, made anew. Scrolls where a low window has less room than the
// words and buttons take.
Flickable {
    id: scroller
    objectName: "libraryMissing"
    visible: backend.libraryError !== ""
    contentHeight: parts.height + 32
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    interactive: contentHeight > height
    ScrollBar.vertical: ScrollBar {}
    // From the keyboard: Enter tries again, Tab reaches the others. Not from
    // under a dialog that is up.
    onVisibleChanged: if (visible && !win.dialogOpen) retry.forceActiveFocus()

    ColumnLayout {
        id: parts
        x: (scroller.width - width) / 2
        y: Math.max(16, (scroller.height - height) / 2)
        width: Math.min(scroller.width - 32, 460)
        spacing: 12

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.pixelSize: 22
            text: backend.libraryMissing ? qsTr("The library isn't there") : qsTr("The library can't be opened")
        }
        Label {
            objectName: "libraryProblem"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: theme.colors.muted
            text: backend.libraryError
        }
        Flow {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: parent.width
            Layout.topMargin: 4
            spacing: 8
            AppButton {
                id: retry
                objectName: "libraryRetry"
                primary: true
                text: qsTr("Try Again")
                onClicked: backend.retryLibrary()
            }
            AppButton {
                objectName: "libraryOther"
                text: qsTr("Choose Another")
                onClicked: win.showLibraries()
            }
            AppButton {
                objectName: "libraryCreate"
                visible: backend.libraryMissing
                text: qsTr("Start an Empty Library Here")
                onClicked: backend.createLibrary()
            }
        }
    }
}
