import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The libraries this computer knows: pick one to open it, add another
// folder, or sync the open one with another computer.
AppPopup {
    id: popup
    objectName: "libraries"
    widest: 640

    function pick(path) {
        backend.useLibrary(path)
        close()
    }
    // The library asked to be thrown away, until that is confirmed or left.
    property string trashing: ""
    onClosed: trashing = ""
    onOpened: {
        list.currentIndex = Math.max(0, settings.libraries.indexOf(backend.libraryPath))
        list.forceActiveFocus()
    }

    DialogBody {

        Label {
            text: qsTr("Libraries")
            font.pixelSize: 18
            font.bold: true
        }
        ListView {
            id: list
            objectName: "librariesList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 320)
            clip: true
            model: settings.libraries
            Keys.onReturnPressed: if (currentIndex >= 0) popup.pick(settings.libraries[currentIndex])
            Keys.onEnterPressed: if (currentIndex >= 0) popup.pick(settings.libraries[currentIndex])
            // Forgetting a library takes it off this list; its folder stays.
            // With Shift the folder goes to the trash too, once confirmed.
            Keys.onDeletePressed: event => {
                if (currentIndex < 0)
                    return
                if (event.modifiers & Qt.ShiftModifier)
                    popup.trashing = settings.libraries[currentIndex]
                else
                    backend.forgetLibrary(settings.libraries[currentIndex])
            }

            delegate: Rectangle {
                id: row
                required property int index
                required property string modelData
                readonly property bool open: modelData === backend.libraryPath
                readonly property bool trashing: popup.trashing === modelData
                width: list.width
                height: rowParts.implicitHeight + 16
                radius: Math.min(4, theme.radius)
                color: list.currentIndex === index ? theme.colors.selection : "transparent"

                // The name and the folder, and beside them what can be done
                // with the library: under them where the window is narrow.
                GridLayout {
                    id: rowParts
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10
                    anchors.rightMargin: 6
                    columns: win.compact ? 1 : 2
                    columnSpacing: 10
                    rowSpacing: 6
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: !row.trashing
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.split("/").pop()
                            font.bold: row.open
                            color: row.open ? theme.accent : theme.foreground
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.modelData
                            color: theme.colors.muted
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }
                    }
                    Label {
                        visible: row.trashing
                        Layout.fillWidth: true
                        text: qsTr("Move the folder and its books to the trash?")
                        color: theme.colors.orange
                        wrapMode: Text.Wrap
                    }
                    Row {
                        id: rowButtons
                        spacing: 6
                        z: 1
                        AppButton {
                            objectName: "forgetLibrary"
                            visible: !row.trashing && settings.libraries.length > 1
                            text: qsTr("Forget")
                            onClicked: backend.forgetLibrary(row.modelData)
                        }
                        AppButton {
                            objectName: "trashLibrary"
                            visible: !row.trashing && settings.libraries.length > 1
                            text: qsTr("Delete…")
                            onClicked: popup.trashing = row.modelData
                        }
                        AppButton {
                            objectName: "trashLibraryYes"
                            visible: row.trashing
                            text: qsTr("Move to Trash")
                            onClicked: {
                                popup.trashing = ""
                                backend.forgetLibrary(row.modelData, true)
                            }
                        }
                        AppButton {
                            visible: row.trashing
                            text: qsTr("Keep")
                            onClicked: popup.trashing = ""
                        }
                    }
                }
                TapHandler { onTapped: popup.pick(row.modelData) }
            }
        }
        // Calibre's libraries found on this computer, each of which can be
        // imported, or imported again to bring the reading up to date.
        Label {
            objectName: "calibreOff"
            Layout.fillWidth: true
            visible: backend.calibreSwitchedOff
            text: qsTr("The Calibre plugin is switched off, so Calibre libraries aren't looked for.")
            color: theme.colors.muted
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        Repeater {
            model: backend.calibreLibraries
            RowLayout {
                id: found
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: 10
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Calibre: %1").arg(found.modelData.name)
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        // "145 books · imported"
                        text: (found.modelData.books < 0 ? qsTr("It can't be read")
                                                         : qsTr("%Ln books", "", found.modelData.books))
                              + (found.modelData.answer === "imported" ? " · " + qsTr("imported")
                                 : found.modelData.answer === "never" ? " · " + qsTr("set aside") : "")
                        color: theme.colors.muted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
                AppButton {
                    objectName: "calibreImport"
                    enabled: found.modelData.books > 0
                    text: found.modelData.answer === "imported" ? qsTr("Import Again") : qsTr("Import")
                    onClicked: {
                        backend.importCalibreLibrary(found.modelData.path)
                        popup.close()
                    }
                }
            }
        }
        actions: [
            AppButton {
                objectName: "findCalibre"
                visible: !backend.calibreSwitchedOff
                text: qsTr("Find Calibre Libraries")
                onClicked: {
                    backend.findCalibreLibraries()
                    popup.close()
                }
            },
            AppButton {
                objectName: "otherFolder"
                text: qsTr("Other Folder")
                onClicked: {
                    backend.chooseLibrary()
                    popup.close()
                }
            },
            AppButton {
                objectName: "syncNow"
                visible: settings.syncTarget !== ""
                enabled: !backend.busy
                text: qsTr("Sync with %1").arg(settings.syncTarget.split(":")[0])
                onClicked: {
                    backend.sync()
                    popup.close()
                }
            },
            AppButton { text: qsTr("Close"); onClicked: popup.close() }
        ]
    }
}
