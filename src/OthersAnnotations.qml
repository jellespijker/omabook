import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omabook

// Whose annotations are shown beside one's own on the open book (#85): the
// people one shares a space with whose annotations part is on, each in their
// colour with how many of theirs there are as far as one has read, switched
// off one at a time or all at once. Kept in the settings of this computer.
// Shown only when someone has annotations on the book.
ColumnLayout {
    id: others
    objectName: "othersAnnotations"
    spacing: 4
    visible: othersLayer.people.length > 0

    readonly property QtObject windowReader: reader
    readonly property QtObject windowSettings: settings
    AnnotationLayerView { id: othersLayer; reader: others.windowReader; settings: others.windowSettings }
    Connections {
        target: backend
        function onRevisionChanged() { othersLayer.refresh() }
    }

    // Made only when someone has annotations on the book: most books have none.
    Loader {
        Layout.fillWidth: true
        active: othersLayer.people.length > 0
        sourceComponent: ColumnLayout {
            spacing: 4
            Switch {
                objectName: "othersShown"
                Layout.fillWidth: true
                // A fingertip high, to be tapped, and no more: a short pane still shows what is under them.
                Layout.minimumHeight: win.touch ? 36 : 0
                text: qsTr("Others' annotations")
                checked: othersLayer.shown
                onToggled: othersLayer.shown = checked
            }
            Repeater {
                model: othersLayer.people
                delegate: CheckBox {
                    id: person
                    required property var modelData
                    objectName: "othersPerson"
                    Layout.fillWidth: true
                    Layout.minimumHeight: win.touch ? 36 : 0
                    Layout.leftMargin: 12
                    enabled: othersLayer.shown
                    checked: modelData.shown
                    text: qsTr("%1, %Ln annotations", "", modelData.count).arg(modelData.called)
                    onToggled: othersLayer.setPersonShown(modelData.person, checked)
                    contentItem: RowLayout {
                        spacing: 6
                        Rectangle {
                            Layout.leftMargin: person.mirrored ? 0 : person.indicator.width + person.spacing
                            Layout.rightMargin: person.mirrored ? person.indicator.width + person.spacing : 0
                            width: 10
                            height: 10
                            radius: 5
                            color: person.modelData.color
                        }
                        Label {
                            Layout.fillWidth: true
                            text: person.text
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: theme.foreground
                            opacity: person.enabled ? 1 : 0.5
                        }
                    }
                }
            }
        }
    }
}
