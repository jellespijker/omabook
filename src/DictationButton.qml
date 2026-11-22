import QtQuick
import QtQuick.Controls
import Omabook

// The microphone of a field that is dictated into (#148), beside it: held
// while speaking, or pressed once to start and again when done, with a mouse,
// a finger or a pen; Ctrl+M while the field has the keyboard does the same.
// It listens only when it is pressed, and while it listens it is filled red
// and swells with what it hears (and the mark at the top of the window says
// who listens); while what was said is written out it pulses. The words then
// go in the field where its cursor is, in place of what is selected, with a
// space between them and the words beside them. Escape, while it listens or
// writes, stops it, and nothing goes in.
//
// For Omabook's own note fields and, by `import Omabook`, a plugin's view:
// `target` is its TextArea or TextField, `plugin` its name, said while it
// listens, and `hosted` whether what is said may go to a hosted provider
// where the settings allow one for speech to text: Omabook's own fields
// may, a plugin's view must say so itself. With the AI Off it isn't there.
Item {
    id: mic
    property Item target: null
    property string plugin: ""
    property bool hosted: plugin === ""
    // Whether the field is in use (being written in): it is shown, and asks
    // what it may do, only then.
    property bool active: true
    // A pen's press is this button's, not the page's pen gestures' over it (PenGestures).
    readonly property bool takesPen: true
    // The words are about to go in: the field may need to be opened again first.
    signal hearing(string text)

    readonly property var dictation: backend.dictation
    // The field's number while it is listened or written for; 0 otherwise.
    property int field: 0
    readonly property bool listening: field !== 0 && dictation.field === field && dictation.listening
    readonly property bool writing: field !== 0 && dictation.field === field && dictation.writing
    readonly property var status: {
        // Asked again whenever the settings change.
        dictation.withVoxtype
        dictation.voxtypeFound
        return active || field !== 0 ? dictation.status(hosted) : ({})
    }
    readonly property bool available: !!status.available
    // A quick press keeps listening until the next; held longer, it is push to talk.
    readonly property int holdMs: 400
    property real pressedAt: 0
    property bool held: false

    visible: active && status.why !== undefined && status.why !== "ai-off"
    readonly property real side: typeof win !== "undefined" && win.touch ? 40 : 30
    implicitWidth: side
    implicitHeight: side

    Accessible.role: Accessible.Button
    Accessible.name: listening ? qsTr("Stop dictating") : writing ? qsTr("Writing out what you said") : qsTr("Dictate")
    Accessible.description: tip.text
    Accessible.onPressAction: listening ? dictation.stop() : press()

    function press() {
        if (listening) {
            held = false
            dictation.stop()
            return
        }
        if (writing) return
        if (!available) {
            backend.say(status.message || qsTr("Nothing can be dictated here."))
            return
        }
        pressedAt = Date.now()
        held = true
        field = dictation.listen(plugin, hosted)
    }
    function release() {
        if (!held) return
        held = false
        if (listening && Date.now() - pressedAt >= holdMs) dictation.stop()
    }
    function stopAll() {
        held = false
        dictation.cancel()
        field = 0
    }
    // The words, where the cursor is, in place of what is selected.
    function put(words) {
        if (!target) return
        hearing(words)
        let at = target.cursorPosition
        if (target.selectionStart !== target.selectionEnd) {
            at = target.selectionStart
            target.remove(target.selectionStart, target.selectionEnd)
        }
        const before = target.getText(0, at)
        const after = target.getText(at, target.length)
        const text = (before !== "" && !/\s$/.test(before) ? " " : "") + words + (after !== "" && !/^[\s.,;:!?)\]]/.test(after) ? " " : "")
        target.insert(at, text)
        target.cursorPosition = at + text.length
        target.forceActiveFocus()
    }

    Connections {
        target: mic.dictation
        function onHeard(field, text) {
            if (field !== mic.field) return
            mic.field = 0
            mic.put(text)
        }
        function onFailed(field, why) {
            if (field !== mic.field) return
            mic.field = 0
            mic.held = false
            backend.say(why)
        }
        function onHandedOver(field) {
            if (field !== mic.field) return
            mic.field = 0
            backend.say(qsTr("voxtype writes out what you said and types it here."))
        }
    }
    Component.onDestruction: if (field !== 0) dictation.forget(field)

    DictationKeys {
        target: mic.visible ? mic.target : null
        catchEscape: mic.listening || mic.writing
        onPressed: mic.press()
        onReleased: mic.release()
        onEscaped: mic.stopAll()
    }

    Rectangle {
        id: disc
        anchors.centerIn: parent
        width: mic.side - 4
        height: width
        radius: width / 2
        // It swells a little with what it hears, so it is plain that it does.
        scale: mic.listening ? 1 + 0.25 * Math.min(1, Math.sqrt(mic.dictation.level)) : 1
        Behavior on scale { NumberAnimation { duration: 90 } }
        color: mic.listening ? theme.colors.red
               : tap.pressed || hover.hovered ? theme.colors.selection : "transparent"
        border.width: mic.listening ? 0 : 1
        border.color: mic.writing ? theme.accent : theme.colors.selection
        opacity: mic.available || mic.listening ? 1 : 0.5
        SequentialAnimation on opacity {
            running: mic.writing
            loops: Animation.Infinite
            alwaysRunToEnd: true
            NumberAnimation { to: 0.35; duration: 500 }
            NumberAnimation { to: 1; duration: 500 }
        }
    }
    Glyph {
        anchors.centerIn: parent
        name: "mic"
        size: mic.side * 0.55
        color: mic.listening ? "#ffffff" : mic.writing ? theme.accent : theme.foreground
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    // A mouse, a finger and a pen alike: down starts it, up after a hold stops it.
    TapHandler {
        id: tap
        acceptedDevices: PointerDevice.AllDevices
        gesturePolicy: TapHandler.WithinBounds
        onPressedChanged: pressed ? mic.press() : mic.release()
        onCanceled: mic.release()
    }
    ToolTip {
        id: tip
        visible: hover.hovered && !mic.listening
        delay: 600
        text: !mic.available ? (mic.status.message || "")
              : qsTr("Dictate (%1): hold it while you speak, or press it once to start and again to stop.").arg("Ctrl+M")
    }
}
