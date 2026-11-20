import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The chronology's header: the way back, what it is of, how many events
// it shows and how many contradictions they are in, and the filter and the
// list of everything else it does, for a pointer and a finger.
RowLayout {
    id: header
    property var browser
    property bool compact: false
    signal close()
    signal filter()
    signal more()
    spacing: 12

    function number(n) { return n.toLocaleString(Qt.locale(), 'f', 0) }

    Rectangle {
        objectName: "chronologyClose"
        readonly property string runs: "chronology.close"
        Layout.preferredWidth: Math.max(closeRow.implicitWidth + 16, height)
        Layout.preferredHeight: win.touch ? 40 : 30
        radius: theme.radius
        color: closeHover.hovered ? theme.colors.selection : "transparent"
        Row {
            id: closeRow
            anchors.centerIn: parent
            spacing: 4
            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                name: win.rightToLeft ? "forward" : "back"
                size: 14
                color: theme.colors.muted
            }
            Label {
                visible: !header.compact
                text: qsTr("Back")
                color: theme.colors.muted
            }
        }
        HoverHandler { id: closeHover }
        TapHandler { onTapped: header.close() }
    }
    Label {
        objectName: "chronologyTitle"
        Layout.fillWidth: true
        Layout.minimumWidth: Math.min(implicitWidth, 40)
        Layout.maximumWidth: Math.ceil(implicitWidth)
        text: header.browser.title !== "" ? qsTr("Chronology of %1").arg(header.browser.title) : qsTr("Chronology")
        font.pixelSize: 20
        font.bold: true
        elide: Text.ElideRight
        textFormat: Text.PlainText
    }
    Label {
        objectName: "chronologyCount"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: header.browser.eventCount > 0
        text: {
            translator.language
            const shown = header.browser.shownCount, all = header.browser.eventCount
            return shown < all ? qsTr("%1 of %Ln events", "", all).arg(header.number(shown)) : qsTr("%Ln events", "", all)
        }
        color: theme.colors.muted
        elide: Text.ElideRight
        // Whole or not at all: a count cut to "4…" says nothing.
        opacity: width >= implicitWidth - 1 ? 1 : 0
    }
    Label {
        objectName: "chronologyContradictions"
        visible: !header.compact && header.browser.contradictionCount > 0
        text: qsTr("%Ln contradictions", "", header.browser.contradictionCount)
        color: theme.colors.yellow || theme.accent
        elide: Text.ElideRight
        Layout.maximumWidth: 220
    }
    HeaderChoice {
        objectName: "chronologyFilter"
        runs: "chronology.filter"
        worded: !header.compact
        glyph: "filter"
        text: qsTr("Filter")
        lit: header.browser.filters.length > 0
        onTapped: header.filter()
    }
    // Everything else it does, each with its key.
    HeaderChoice {
        objectName: "chronologyMore"
        worded: false
        glyph: "more"
        text: qsTr("More")
        onTapped: header.more()
    }
}
