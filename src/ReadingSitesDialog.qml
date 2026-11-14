import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The sites the reader keeps their reading on, one row each, all alike: not
// connected, connected as somebody, or in need of attention, with the same
// three things to do. A row opens to what connecting takes: whatever the
// reader has of themselves there, looked at the moment it is entered. Each
// site says plainly what it lets an app do; Goodreads lets it do the least,
// and gets a guide instead of a paragraph.
AppPopup {
    id: dialog
    objectName: "readingSites"
    widest: 640
    tallest: content.implicitHeight + 2 * padding

    readonly property var desk: librarian.sites
    // The row that is open, by its site; one at a time, so the dialog fits
    // a small window.
    property string current: ""
    // The sites there are, which doesn't change while Omabook runs.
    property var schemes: []
    Component.onCompleted: schemes = desk.rows.map(candidate => candidate.scheme)

    function show(scheme) {
        current = current === scheme ? "" : scheme
        settle.restart()
    }
    function focusRow(index) {
        const header = heads.itemAt(Math.max(0, Math.min(heads.count - 1, index)))
        if (header) header.head.forceActiveFocus()
    }

    onOpened: {
        // Whatever needs doing first is opened: a site that needs attention,
        // else the first that isn't connected.
        let first = ""
        for (const candidate of desk.rows) {
            if (candidate.state === "attention") { first = candidate.scheme; break }
            if (first === "" && candidate.state === "none") first = candidate.scheme
        }
        current = first
        focusRow(Math.max(0, desk.rows.findIndex(candidate => candidate.scheme === first)))
        desk.watchDownloads(current === "goodreads")
        settle.restart()
    }
    onClosed: desk.watchDownloads(false)
    // The Downloads folder is only watched while the guide that asks for a
    // file from it is in front of somebody.
    onCurrentChanged: desk.watchDownloads(opened && current === "goodreads")

    // Brings the open row into view once it has its size. A timer of the
    // dialog's own: the rows it would otherwise call back into can be gone.
    Timer {
        id: settle
        interval: 0
        onTriggered: {
            const index = dialog.desk.rows.findIndex(candidate => candidate.scheme === dialog.current)
            const header = index >= 0 ? heads.itemAt(index) : null
            if (!header) return
            const top = header.mapToItem(content, 0, 0).y
            if (top < flick.contentY || top + header.height > flick.contentY + flick.height)
                flick.contentY = Math.max(0, Math.min(top, flick.contentHeight - flick.height))
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded }

        ColumnLayout {
            id: content
            // Room for the scroll bar whether it shows or not: a width that
            // followed it would chase its own tail.
            width: flick.width - 12
            spacing: 10
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label { text: qsTr("Reading sites"); font.pixelSize: 18; font.bold: true }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: theme.colors.muted
                text: qsTr("Bring in what you have read, are reading and want to read. Nothing is ever removed, here or there, and what you changed here since the last sync stays.")
            }
            Label {
                objectName: "sitesNudge"
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                text: dialog.desk.nudge
            }
            Label {
                objectName: "keyringNote"
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                color: theme.colors.muted
                font.pixelSize: 12
                // Asked only once the dialog is in front of somebody: it
                // means a word with the keyring, which starting up can't wait for.
                text: dialog.opened ? dialog.desk.keyringNote : ""
            }

            Repeater {
                id: heads
                model: dialog.schemes

                ColumnLayout {
                    id: site
                    required property string modelData
                    required property int index
                    readonly property string scheme: modelData
                    // How things stand with this site, as the desk has it now.
                    // The row itself stays, so what is being typed into it
                    // survives the desk having news.
                    readonly property var info: dialog.desk.rows.find(candidate => candidate.scheme === scheme) || ({})
                    readonly property bool open: dialog.current === scheme
                    readonly property alias head: head
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: theme.colors.selection }

                    // The row itself: who the site is, how things stand, and
                    // the one thing most likely wanted.
                    ItemDelegate {
                        id: head
                        objectName: site.scheme + "Row"
                        Layout.fillWidth: true
                        implicitHeight: Math.max(52, summary.implicitHeight + 16)
                        padding: 0
                        Keys.onReturnPressed: dialog.show(site.scheme)
                        Keys.onEnterPressed: dialog.show(site.scheme)
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_Down || event.key === Qt.Key_J) { dialog.focusRow(site.index + 1); event.accepted = true }
                            else if (event.key === Qt.Key_Up || event.key === Qt.Key_K) { dialog.focusRow(site.index - 1); event.accepted = true }
                        }
                        onClicked: dialog.show(site.scheme)
                        background: Rectangle {
                            radius: theme.radius
                            color: head.visualFocus || head.hovered ? theme.colors.selection : "transparent"
                            border.width: head.visualFocus ? 1 : 0
                            border.color: theme.foreground
                        }
                        contentItem: RowLayout {
                            spacing: 10
                            Rectangle {
                                Layout.leftMargin: 8
                                Layout.rightMargin: 8
                                implicitWidth: 10
                                implicitHeight: 10
                                radius: 5
                                color: site.info.state === "connected" ? theme.accent
                                     : site.info.state === "attention" ? (theme.colors.red || theme.foreground)
                                     : "transparent"
                                border.width: 1
                                border.color: site.info.state === "connected" ? theme.accent
                                            : site.info.state === "attention" ? (theme.colors.red || theme.foreground)
                                            : theme.colors.muted
                            }
                            ColumnLayout {
                                id: summary
                                Layout.fillWidth: true
                                spacing: 1
                                Label { Layout.fillWidth: true; text: site.info.name; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight }
                                Label {
                                    objectName: site.scheme + "State"
                                    Layout.fillWidth: true
                                    text: [site.info.stateText, site.info.detail, site.info.syncedText].filter(part => part !== "").join(" · ")
                                    color: theme.colors.muted
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                                // What is being done with the site just now, in
                                // the desk's own words, shown open or closed.
                                Thinking {
                                    objectName: site.scheme + "Working"
                                    active: !!site.info.busy
                                    text: site.info.busy ? site.info.status : ""
                                }
                            }
                            AppButton {
                                objectName: site.scheme + "Sync"
                                Layout.rightMargin: 8
                                visible: site.info.state === "connected" && !site.info.busy
                                text: qsTr("Sync Now")
                                onClicked: dialog.desk.syncSite(site.scheme)
                            }
                        }
                    }

                    // What the row opens to.
                    ColumnLayout {
                        visible: site.open
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        spacing: 8

                        Label {
                            objectName: site.scheme + "Why"
                            Layout.fillWidth: true
                            visible: text !== ""
                            wrapMode: Text.Wrap
                            text: site.info.why
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: site.info.state !== "off"
                            TextField {
                                id: identity
                                objectName: site.scheme + "Identity"
                                Layout.fillWidth: true
                                placeholderText: site.scheme === "goodreads" ? qsTr("A link to your profile or a shelf, or your user id")
                                               : site.scheme === "hardcover" ? qsTr("Your user name (the token says it when this is empty)")
                                               : qsTr("A link to your profile, or your user name")
                                text: site.info.identity
                                selectByMouse: true
                                Material.roundedScale: theme.radius
                                onAccepted: connect.clicked()
                            }
                            AppButton {
                                objectName: site.scheme + "FindMe"
                                text: qsTr("Find Me")
                                onClicked: dialog.desk.openPage(site.scheme, "find")
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: identity.visible && identity.text.trim() !== "" && identity.text.trim() !== site.info.identity
                            wrapMode: Text.Wrap
                            color: theme.colors.muted
                            font.pixelSize: 12
                            // What was pasted, as the site will know it.
                            readonly property string understood: visible ? dialog.desk.identityIn(site.scheme, identity.text) : ""
                            text: understood !== "" ? qsTr("That is %1 on %2.").arg(understood).arg(site.info.name)
                                                    : qsTr("That names nobody on %1 yet: paste the address of your profile there.").arg(site.info.name)
                        }

                        // A token, or a pair of keys, where the site has them.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: site.scheme !== "goodreads" && site.info.secretUse > 0 && site.info.state !== "off"
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                color: theme.colors.muted
                                font.pixelSize: 12
                                text: site.scheme === "hardcover"
                                      ? qsTr("Hardcover is asked with a token you make in your own account there. It is kept in the keyring, never in a file.")
                                      : qsTr("With the two keys of your Internet Archive account, a private reading log can be read and changes made here can be sent. Without them a public log is read, and nothing is sent.")
                            }
                            // A pair of keys and the button that fetches them
                                // don't fit on one line of a small window.
                            GridLayout {
                                Layout.fillWidth: true
                                columnSpacing: 8
                                rowSpacing: 8
                                columns: site.info.secretParts === 2 && win.compact ? 2 : 3
                                TextField {
                                    id: secret
                                    objectName: site.scheme + "Secret"
                                    Layout.fillWidth: true
                                    echoMode: TextInput.Password
                                    placeholderText: site.info.hasSecret ? qsTr("Kept in the keyring; paste another to replace it")
                                                   : site.info.secretParts === 2 ? qsTr("Access key (optional)")
                                                   : qsTr("Your token")
                                    selectByMouse: true
                                    Material.roundedScale: theme.radius
                                    onAccepted: connect.clicked()
                                }
                                TextField {
                                    id: secondSecret
                                    objectName: site.scheme + "SecondSecret"
                                    Layout.fillWidth: true
                                    visible: site.info.secretParts === 2
                                    echoMode: TextInput.Password
                                    placeholderText: qsTr("Secret key (optional)")
                                    selectByMouse: true
                                    Material.roundedScale: theme.radius
                                    onAccepted: connect.clicked()
                                }
                                AppButton {
                                    objectName: site.scheme + "GetSecret"
                                    text: site.info.secretParts === 2 ? qsTr("Get Your Keys") : qsTr("Get a Token")
                                    onClicked: dialog.desk.openPage(site.scheme, "secret")
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: site.info.state !== "off"
                            AppButton {
                                id: connect
                                objectName: site.scheme + "Connect"
                                primary: site.info.state !== "connected"
                                enabled: !site.info.busy
                                text: site.info.state === "connected" ? qsTr("Look Again") : qsTr("Connect")
                                onClicked: {
                                    // The pair of keys travels as one secret.
                                    const typed = secondSecret.visible && secondSecret.text.trim() !== ""
                                                  ? secret.text.trim() + ":" + secondSecret.text.trim() : secret.text.trim()
                                    dialog.desk.connectSite(site.scheme, identity.text, typed)
                                    secret.clear()
                                    secondSecret.clear()
                                }
                            }
                            AppButton {
                                objectName: site.scheme + "Disconnect"
                                visible: site.info.state !== "none"
                                enabled: !site.info.busy
                                text: qsTr("Disconnect")
                                onClicked: {
                                    dialog.desk.disconnectSite(site.scheme)
                                    identity.clear()
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }

                        // Changes going the other way, for a site that takes them.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            visible: site.info.canWrite && site.info.state === "connected"
                            CheckBox {
                                objectName: site.scheme + "Write"
                                text: qsTr("Also update %1").arg(site.info.name)
                                checked: site.info.write
                                onToggled: dialog.desk.setWrite(site.scheme, checked)
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                color: theme.colors.muted
                                font.pixelSize: 12
                                text: qsTr("Status, stars and the day you finished go to %1 when you change them here. Nothing is ever removed there, and the first time you are shown what would be sent.").arg(site.info.name)
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                visible: site.info.pendingCount > 0
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    font.bold: true
                                    text: qsTr("%Ln changes would be sent:", "", site.info.pendingCount)
                                }
                                Label {
                                    objectName: site.scheme + "Pending"
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    font.pixelSize: 12
                                    text: site.info.pending.join("\n")
                                }
                                AppButton {
                                    objectName: site.scheme + "Send"
                                    primary: true
                                    enabled: !site.info.busy
                                    text: qsTr("Send Them")
                                    onClicked: dialog.desk.sendPending(site.scheme)
                                }
                            }
                        }

                        // Goodreads: three steps to the one way in that always works.
                        Rectangle {
                            id: guide
                            objectName: "goodreadsGuide"
                            visible: site.info.usesFile
                            Layout.fillWidth: true
                            implicitHeight: steps.implicitHeight + 24
                            radius: theme.radius
                            color: "transparent"
                            border.width: drop.containsDrag ? 2 : 1
                            border.color: drop.containsDrag ? theme.accent : theme.colors.selection

                            DropArea {
                                id: drop
                                anchors.fill: parent
                                onDropped: (dropped) => {
                                    if (dropped.hasUrls && dropped.urls.length > 0) {
                                        dialog.desk.importUrl(dropped.urls[0])
                                        dropped.accept()
                                    }
                                }
                            }
                            ColumnLayout {
                                id: steps
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    color: theme.colors.muted
                                    font.pixelSize: 12
                                    text: qsTr("Goodreads has no way in for apps and takes no changes from them. What it does have is a file of your shelves, which works for a private profile too.")
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label { text: "1"; font.bold: true }
                                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Open Goodreads' import and export page.") }
                                    AppButton {
                                        objectName: "goodreadsOpenExport"
                                        text: qsTr("Open the Page")
                                        onClicked: dialog.desk.openPage("goodreads", "export")
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label { text: "2"; font.bold: true }
                                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Press Export Library there, wait, and download the file it makes.") }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label { text: "3"; font.bold: true }
                                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Drop the file here, or pick it.") }
                                    AppButton {
                                        objectName: "goodreadsImport"
                                        enabled: !site.info.busy
                                        text: qsTr("Pick the File")
                                        onClicked: librarian.importSiteFile()
                                    }
                                }
                                // The download, seen arriving: offered, never taken unasked.
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    visible: dialog.desk.offeredFile !== ""
                                    Label {
                                        objectName: "goodreadsOffer"
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        font.bold: true
                                        text: qsTr("%1 is in your Downloads folder.").arg(dialog.desk.offeredFileName)
                                    }
                                    AppButton {
                                        objectName: "goodreadsTakeOffer"
                                        primary: true
                                        enabled: !site.info.busy
                                        text: qsTr("Bring It In")
                                        onClicked: dialog.desk.importFile(dialog.desk.offeredFile)
                                    }
                                    AppButton {
                                        objectName: "goodreadsLeaveOffer"
                                        text: qsTr("Not This One")
                                        onClicked: dialog.desk.dismissOffer()
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        color: theme.colors.muted
                                        font.pixelSize: 12
                                        text: qsTr("The way back is a file too: Omabook writes your reading as one, and you upload it on the same page.")
                                    }
                                    AppButton {
                                        objectName: "goodreadsExport"
                                        text: qsTr("Export for Goodreads")
                                        onClicked: librarian.exportSiteFile()
                                    }
                                }
                            }
                        }

                        Label {
                            objectName: site.scheme + "Status"
                            Layout.fillWidth: true
                            visible: text !== ""
                            wrapMode: Text.Wrap
                            // How the last thing went; what is going on now
                            // is said in the row, with the dots.
                            text: site.info.busy ? "" : (site.info.status || "")
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Item { Layout.fillWidth: true }
                AppButton { objectName: "readingSitesClose"; text: qsTr("Close"); onClicked: dialog.close() }
            }
        }
    }
}
