import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omabook

// Someone else's margin note (#85), as it stands in the margin column beside
// one's own: under their name, its edge in their colour, its words as plain
// text and never markup. It is theirs, so it is read and not edited; its words
// can be kept as a fact of a thing of the book's graph, learned where the note
// is, which is then one's own (AnnotationLayerView::makeFact).
FocusScope {
    id: card
    // {id, note, called, color, person, ink}, as AnnotationLayerView::notesIn gives it.
    property var note: null
    property var annotationLayer: null
    property int lines: 6
    property int fontSize: 13
    property bool keeping: false

    implicitHeight: frame.implicitHeight
    activeFocusOnTab: true
    Accessible.role: Accessible.StaticText
    Accessible.name: note ? qsTr("%1's note: %2").arg(note.called).arg(note.note) : ""

    function keep() {
        const thing = thingField.text.trim()
        if (!annotationLayer || !note || thing === "") return
        const why = annotationLayer.makeFact(note.id, thing, kindField.text.trim())
        if (why !== "") {
            backend.say(why)
            return
        }
        keeping = false
        backend.say(qsTr("Kept as a fact of %1").arg(thing))
    }

    Rectangle {
        id: frame
        width: parent.width
        implicitHeight: Math.max(win.touch ? 40 : 0, body.implicitHeight + 12)
        radius: Math.min(6, theme.radius)
        color: Qt.alpha(card.note ? card.note.color : theme.accent, card.activeFocus || hover.hovered ? 0.16 : 0.08)
        border.width: card.activeFocus ? 1 : 0
        border.color: theme.foreground
        // Their colour along the edge that faces the text, dotted as their passages are.
        Column {
            x: win.rightToLeft ? parent.width - 2 : 0
            y: 4
            spacing: 2
            Repeater {
                model: Math.max(1, Math.floor((frame.height - 8) / 4))
                delegate: Rectangle { width: 2; height: 2; radius: 1; color: card.note ? card.note.color : theme.accent }
            }
        }

        ColumnLayout {
            id: body
            x: 10
            y: 6
            width: parent.width - 16
            spacing: 4
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label {
                objectName: "othersNoteName"
                Layout.fillWidth: true
                text: card.note ? card.note.called : ""
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.pixelSize: Math.max(10, card.fontSize - 2)
                font.weight: Font.DemiBold
                color: theme.colors.muted
            }
            // Written by hand (#90): their ink, as wide as the card, in their colour (#124).
            InkSurface {
                id: theirInk
                objectName: "othersNoteInk"
                readonly property bool inked: !!card.note && !!card.note.ink
                readonly property real unit: width / (card.annotationLayer ? card.annotationLayer.noteInkUnits : 240)
                property real lowest: 0
                visible: inked
                Layout.fillWidth: true
                Layout.preferredHeight: inked ? Math.max(24, lowest + 6) * unit : 0
                inkScale: Math.max(0.01, unit)
                readOnly: true
                enabled: false
                Accessible.role: Accessible.Graphic
                Accessible.name: card.note && card.note.note !== "" ? qsTr("Handwritten margin note: %1").arg(card.note.note)
                                                                    : qsTr("Handwritten margin note")
                function draw() {
                    if (!card.annotationLayer) return
                    if (inked) card.annotationLayer.showNoteInk(theirInk, card.note.id)
                    else card.annotationLayer.forgetInk(theirInk)
                }
                onInkedChanged: draw()
                onStrokesChanged: lowest = card.annotationLayer ? card.annotationLayer.inkBottom(theirInk) : 0
                Component.onCompleted: draw()
                Component.onDestruction: if (card.annotationLayer) card.annotationLayer.forgetInk(theirInk)
                Connections {
                    target: card
                    function onNoteChanged() { theirInk.draw() }
                    function onAnnotationLayerChanged() { theirInk.draw() }
                }
            }
            Label {
                objectName: "othersNoteWords"
                Layout.fillWidth: true
                visible: !!card.note && (card.note.note !== "" || !card.note.ink)
                text: !card.note ? "" : card.note.note !== "" ? card.note.note : qsTr("A handwritten note")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                maximumLineCount: card.activeFocus || card.lines <= 0 ? 1000 : card.lines
                elide: Text.ElideRight
                font.pixelSize: card.fontSize
                font.italic: !!card.note && card.note.note === ""
                lineHeight: 1.15
                color: theme.foreground
            }
            AppButton {
                objectName: "othersNoteToFact"
                visible: !card.keeping && !!card.note && card.note.note !== "" && (hover.hovered || card.activeFocus || win.touch)
                focusPolicy: Qt.NoFocus
                leftPadding: 10
                rightPadding: 10
                text: qsTr("Keep as a fact")
                onClicked: {
                    card.keeping = true
                    thingField.forceActiveFocus()
                }
            }
            TextField {
                id: thingField
                objectName: "othersNoteThing"
                visible: card.keeping
                Layout.fillWidth: true
                placeholderText: qsTr("Of which thing: a name in the graph")
                selectByMouse: true
                Material.roundedScale: theme.radius
                onAccepted: card.keep()
                Keys.onEscapePressed: card.keeping = false
            }
            TextField {
                id: kindField
                objectName: "othersNoteKind"
                visible: card.keeping
                Layout.fillWidth: true
                placeholderText: qsTr("Its kind, to make it when there is none")
                selectByMouse: true
                Material.roundedScale: theme.radius
                onAccepted: card.keep()
                Keys.onEscapePressed: card.keeping = false
            }
            RowLayout {
                visible: card.keeping
                Layout.fillWidth: true
                spacing: 6
                Item { Layout.fillWidth: true }
                AppButton {
                    focusPolicy: Qt.NoFocus
                    text: qsTr("Cancel")
                    onClicked: card.keeping = false
                }
                AppButton {
                    objectName: "othersNoteKeep"
                    focusPolicy: Qt.NoFocus
                    primary: true
                    text: qsTr("Keep")
                    onClicked: card.keep()
                }
            }
        }
        HoverHandler { id: hover }
        TapHandler { onTapped: card.forceActiveFocus() }
    }
    Keys.onReturnPressed: if (!keeping && note && note.note !== "") {
        keeping = true
        thingField.forceActiveFocus()
    }
}
