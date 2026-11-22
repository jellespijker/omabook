import QtQuick
import QtQuick.Controls

// The on-screen bar of what the reading keys do, which a tap mid-page or the
// ☰ button brings up: one row where the window has room for it, more where
// it doesn't. Each button is the pointer's way to its command (`runs`).
Rectangle {
    id: barFrame
    objectName: "readerBar"
    // The reading view it works, and the width of the page it lies over.
    property Item host
    property real room: 0
    // What is said while reading (ReaderVoice.qml, #150).
    property Item voice
    width: bar.childrenRect.width + 24
    height: bar.childrenRect.height + 24
    radius: theme.radius
    color: theme.background
    border.color: theme.colors.selection

    function done() { host.showBar = false }

    Flow {
        id: bar
        // Mirrored, the buttons stand at the right of the room they may
        // take: the frame is round them, not round that room.
        x: 12 - bar.childrenRect.x
        y: 12
        width: Math.min(660, barFrame.room - 48)
        spacing: 8
        AppButton { objectName: "barBack"; runs: "reader.back"; text: qsTr("Library"); focusPolicy: Qt.NoFocus; onClicked: reader.close() }
        AppButton { objectName: "barContents"; runs: "reader.contents"; text: qsTr("Contents"); focusPolicy: Qt.NoFocus; onClicked: { barFrame.done(); barFrame.host.toggleIndex() } }
        AppButton { objectName: "barFind"; runs: "reader.find"; text: qsTr("Find"); focusPolicy: Qt.NoFocus; onClicked: { barFrame.done(); barFrame.host.openFinder() } }
        AppButton { objectName: "barBookmark"; runs: "reader.bookmark"; text: qsTr("Bookmark"); focusPolicy: Qt.NoFocus; onClicked: { barFrame.done(); barFrame.host.bookmark() } }
        AppButton { objectName: "barHighlights"; runs: "reader.annotations"; text: qsTr("Highlights"); focusPolicy: Qt.NoFocus; onClicked: { barFrame.done(); barFrame.host.showList("annotations") } }
        AppButton { objectName: "barInk"; runs: "reader.ink"; visible: reader.kind === "pages"; text: qsTr("Write"); focusPolicy: Qt.NoFocus; onClicked: { barFrame.done(); win.run("reader.ink") } }
        AppButton { objectName: "barReadAloud"; runs: "reader.read_aloud"; text: qsTr("Read aloud"); focusPolicy: Qt.NoFocus; onClicked: { barFrame.done(); reader.readAloud.toggle(barFrame.host.content ? barFrame.host.content.placeText() : "") } }
        // Held while speaking, or pressed once to start and again to stop; the bar stays while it listens.
        // In a short window the bar has no room for another row: More and the key have it there. It is
        // made only when shown: a hidden child of the mirrored Flow would widen the frame (childrenRect).
        Repeater {
            model: {
                backend.dictation.withVoxtype
                backend.dictation.voxtypeFound
                const why = backend.dictation.status(true, false).why
                return !!barFrame.voice && why !== undefined && why !== "ai-off" && win.height >= 420 ? 1 : 0
            }
            AppButton {
                objectName: "barVoice"
                runs: "reader.voice"
                readonly property bool takesPen: true
                text: barFrame.voice && barFrame.voice.listening ? qsTr("Stop") : qsTr("Speak")
                focusPolicy: Qt.NoFocus
                onPressed: barFrame.voice.press()
                onReleased: barFrame.voice.release()
                onCanceled: barFrame.voice.release()
                // A pen's press comes as a tablet's, which a button doesn't take by itself.
                TapHandler {
                    acceptedDevices: PointerDevice.Stylus
                    gesturePolicy: TapHandler.WithinBounds
                    onPressedChanged: pressed ? barFrame.voice.press() : barFrame.voice.release()
                    onCanceled: barFrame.voice.release()
                }
            }
        }
        AppButton { objectName: "barGraph"; runs: "reader.graph"; text: qsTr("Graph"); focusPolicy: Qt.NoFocus; onClicked: { barFrame.done(); barFrame.host.toggleGraph() } }
        AppButton { objectName: "barAsk"; runs: "reader.ask"; text: qsTr("Ask AI"); focusPolicy: Qt.NoFocus; onClicked: { barFrame.done(); barFrame.host.askAssistant() } }
        AppButton { objectName: "barSmaller"; runs: "reader.smaller"; text: qsTr("Smaller"); focusPolicy: Qt.NoFocus; onClicked: if (barFrame.host.content) barFrame.host.content.zoom(-1) }
        AppButton { objectName: "barLarger"; runs: "reader.larger"; text: qsTr("Larger"); focusPolicy: Qt.NoFocus; onClicked: if (barFrame.host.content) barFrame.host.content.zoom(1) }
        AppButton {
            objectName: "barFormat"
            runs: "reader.format"
            text: "Aa"
            Accessible.name: qsTr("Text")
            focusPolicy: Qt.NoFocus
            onClicked: { barFrame.done(); barFrame.host.toggleFormat() }
        }
        // The rest: the AI's catch-up and cast, the marks, the pages side
        // by side, chapters, the whole screen, help.
        AppButton {
            objectName: "barMore"
            text: qsTr("More")
            focusPolicy: Qt.NoFocus
            onClicked: {
                barFrame.done()
                win.showMenu("reader")
            }
        }
    }
}
