import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A Calibre library found on this computer, offered once for import. It
// sits quietly at the bottom of the shelf and takes no focus: reading and
// browsing go on around it until it is answered. "Not now" puts it off for
// a week; "Never" is remembered for this library.
Rectangle {
    id: offer
    objectName: "calibreOffer"

    readonly property var library: backend.calibreOffer
    readonly property bool offered: library.path !== undefined

    visible: offered
    width: Math.min(parent.width - 2 * win.dialogEdge, 760)
    height: row.implicitHeight + 24
    radius: theme.radius
    color: theme.background
    border.color: theme.colors.muted

    // The words and, beside them, the answers; under them where the
    // window is too narrow for both.
    GridLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 12
        columns: win.compact ? 1 : 2
        columnSpacing: 8
        rowSpacing: 8

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                objectName: "calibreOfferText"
                Layout.fillWidth: true
                // "Calibre library “Calibre Library” holds 145 books. Import it?"
                text: offer.offered ? qsTr("Your Calibre library “%1” holds %Ln books. Import it?", "", offer.library.books).arg(offer.library.name) : ""
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: offer.offered ? qsTr("%1 is only read. Details, covers and what you have read come along.").arg(offer.library.path) : ""
                color: theme.colors.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
        DialogActions {
            Layout.fillWidth: win.compact
            AppButton {
                objectName: "calibreOfferNever"
                text: qsTr("Never")
                focusPolicy: Qt.TabFocus
                onClicked: backend.answerCalibreOffer("never")
            }
            AppButton {
                objectName: "calibreOfferLater"
                text: qsTr("Not now")
                focusPolicy: Qt.TabFocus
                onClicked: backend.answerCalibreOffer("later")
            }
            AppButton {
                objectName: "calibreOfferImport"
                text: qsTr("Import")
                primary: true
                focusPolicy: Qt.TabFocus
                onClicked: backend.answerCalibreOffer("import")
            }
        }
    }
}
