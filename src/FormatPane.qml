import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The "Aa" panel beside the page: how the text is set, changed as you look
// at it. Changes go to the defaults, or to this book alone while "Only this
// book" is on. Reset puts back what there was when the panel opened, and
// Escape or Done closes it. Pages that are pictures can't be set: the panel
// says so.
FocusScope {
    id: pane
    // The reading view, which keeps what there was when the panel opened,
    // so the panel can move from beside the page to under it and back.
    required property Item host
    readonly property bool focused: activeFocus
    // How tall it would like to be, for a host that isn't the window's height.
    readonly property real wanted: column.implicitHeight + 32
    readonly property bool applies: reader.kind !== "pages"
    readonly property bool hasOwn: Object.keys(reader.format.own).length > 0
    readonly property bool collectionHasOwn: Object.keys(reader.format.collectionOwn).length > 0
    signal done()

    function takeFocus() { controls.first.forceActiveFocus() }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height + 1
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            width: flick.width
            spacing: 10
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Text")
                    font.pixelSize: 18
                    font.bold: true
                    elide: Text.ElideRight
                }
                AppButton {
                    objectName: "formatClose"
                    implicitWidth: 36
                    implicitHeight: 36
                    leftPadding: 0
                    rightPadding: 0
                    focusPolicy: Qt.NoFocus
                    text: "✕"
                    Accessible.name: qsTr("Close")
                    onClicked: pane.done()
                }
            }
            Label {
                objectName: "formatPictures"
                Layout.fillWidth: true
                visible: !pane.applies
                wrapMode: Text.Wrap
                color: theme.colors.muted
                text: qsTr("These pages are pictures, so their type can't be changed. What is set here is for books that flow.")
            }
            Switch {
                objectName: "formatThisBook"
                Layout.fillWidth: true
                // Tall enough for a fingertip.
                Layout.minimumHeight: 40
                visible: reader.format.hasBook
                text: qsTr("Only this book")
                checked: pane.host.formatBookOnly
                Accessible.name: text
                // Off, the book follows the defaults again.
                onToggled: {
                    if (!checked) reader.format.resetBook()
                    pane.host.formatBookOnly = checked
                    if (checked) pane.host.formatForCollection = false
                }
            }
            // The book's collection: novels justified, technical books with
            // longer lines. It lies between the defaults and a book's own.
            Switch {
                objectName: "formatThisCollection"
                Layout.fillWidth: true
                Layout.minimumHeight: 40
                visible: reader.format.hasBook && reader.format.collectionId > 0
                text: qsTr("For this collection: %1").arg(reader.format.collectionName)
                checked: pane.host.formatForCollection && !pane.host.formatBookOnly
                Accessible.name: text
                onToggled: {
                    pane.host.formatForCollection = checked
                    if (checked) pane.host.formatBookOnly = false
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.muted
                font.pixelSize: 12
                text: pane.host.formatBookOnly ? qsTr("Changes are for this book alone; the others keep the defaults.")
                      : pane.host.formatForCollection && reader.format.collectionId > 0
                        ? qsTr("Changes are for the books of %1 without a way of their own.").arg(reader.format.collectionName)
                      : qsTr("Changes are the defaults, for every book without a way of its own.")
            }
            FormatControls {
                id: controls
                Layout.fillWidth: true
                scoped: true
                bookOnly: pane.host.formatBookOnly
                forCollection: pane.host.formatForCollection
                enabled: pane.applies
            }
            AppButton {
                objectName: "formatResetCollection"
                Layout.fillWidth: true
                visible: pane.collectionHasOwn && reader.format.collectionId > 0
                text: qsTr("Reset %1 to the defaults").arg(reader.format.collectionName)
                onClicked: {
                    reader.format.resetCollection()
                    pane.host.formatForCollection = false
                }
            }
            AppButton {
                objectName: "formatResetBook"
                Layout.fillWidth: true
                visible: pane.hasOwn
                text: qsTr("Reset this book to the defaults")
                onClicked: {
                    reader.format.resetBook()
                    pane.host.formatBookOnly = false
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 8
                AppButton {
                    objectName: "formatRestore"
                    Layout.fillWidth: true
                    text: qsTr("Reset")
                    Accessible.description: qsTr("Put back what there was when this was opened")
                    onClicked: pane.host.restoreFormat()
                }
                AppButton {
                    objectName: "formatDone"
                    Layout.fillWidth: true
                    primary: true
                    text: qsTr("Done")
                    onClicked: pane.done()
                }
            }
        }
    }
}
