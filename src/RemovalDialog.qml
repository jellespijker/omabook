import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Removing a book, asked first: its files move to the trash.
AppPopup {
    id: dialog
    objectName: "removal"
    property var bookId: 0
    property string bookTitle: ""
    // A plugin's launcher is only taken off the shelf; its plugin hears of it.
    property bool launcher: false
    property string pluginName: ""
    function show(id) {
        const book = backend.book(id)
        bookId = id
        bookTitle = book.title || ""
        launcher = book.launcher === true
        pluginName = book.pluginName || ""
        open()
    }

    widest: 468

    DialogBody {
        Label {
            Layout.fillWidth: true
            text: dialog.launcher ? qsTr("Take “%1” off the shelf?").arg(dialog.bookTitle)
                  : qsTr("Remove “%1” from the library?").arg(dialog.bookTitle)
            font.pixelSize: 18
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: dialog.launcher ? qsTr("It is not a book: %1 put it there to start something new. %1 can put it back.").arg(dialog.pluginName)
                  : qsTr("Its files in the library folder move to the trash. The file you added it from is not touched.")
            color: theme.colors.muted
            wrapMode: Text.Wrap
        }
        actions: [
            AppButton { text: qsTr("Keep"); onClicked: dialog.close() },
            AppButton {
                objectName: "confirmRemove"
                text: qsTr("Remove")
                primary: true
                focus: true
                onClicked: {
                    if (dialog.launcher) backend.scripts.removeLauncher(dialog.bookId)
                    else backend.remove(dialog.bookId)
                    dialog.close()
                }
            }
        ]
    }
}
