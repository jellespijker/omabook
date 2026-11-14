import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The tour, offered: once on a first start, once more in the first book
// opened by someone who took it, and when a new version has something to
// show. Like the Calibre offer it sits quietly at the bottom and takes no
// focus; it is never a dialog. Either answer is remembered, so it doesn't
// come back.
Rectangle {
    id: note
    objectName: "tourNote"

    // Which offer this is, if any: "tour", "reader" or "news".
    readonly property string offer: win.touring ? ""
                                  : manual.tourOffered && !win.inReader ? "tour"
                                  : manual.readerTourOffered && reader.open ? "reader"
                                  : manual.newsOffered ? "news" : ""
    readonly property bool offered: offer !== ""
    readonly property string helpKey: (custom.keyText["global.help"] || "").split("  ")[0]

    visible: offered
    width: Math.min(parent.width - 2 * win.dialogEdge, 640)
    height: row.implicitHeight + 24
    radius: theme.radius
    color: theme.background
    border.color: theme.colors.muted

    function decline() {
        if (offer === "news") manual.newsAnswered()
        else manual.tourAnswered(win.inReader ? "reader" : "library", false)
    }

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
                objectName: "tourNoteText"
                Layout.fillWidth: true
                text: note.offer === "tour" ? qsTr("New here? A one-minute tour shows where everything is.")
                    : note.offer === "reader" ? qsTr("Your first book is open. Want a quick look around the reader?")
                    : note.offer === "news" ? qsTr("Omabook %1 brought a few new things.").arg(manual.version) : ""
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: note.helpKey !== ""
                text: qsTr("It is always there under %1.").arg(note.helpKey)
                color: theme.colors.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
        DialogActions {
            Layout.fillWidth: win.compact
            AppButton {
                objectName: "tourNoteDecline"
                text: qsTr("No Thanks")
                focusPolicy: Qt.TabFocus
                onClicked: note.decline()
            }
            AppButton {
                objectName: "tourNoteAccept"
                text: note.offer === "news" ? qsTr("Show Me") : qsTr("Take the Tour")
                primary: true
                focusPolicy: Qt.TabFocus
                onClicked: win.startTour(note.offer === "news" ? "news" : "tour")
            }
        }
    }
}
