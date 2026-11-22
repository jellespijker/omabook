import QtQuick
import QtQuick.Window
import Omabook

// Commands said while reading (#150): "next chapter", "previous page",
// "highlight this", "read aloud", "go to chapter 5", "what does this word
// mean", in any of the window's languages. It listens only when the person
// starts it: its key (reader.voice, Ctrl+M) held while speaking, or pressed
// once to start and again to stop, or the bar's Speak, by mouse, finger or
// pen. While it listens the mark at the top of the window says so, with
// Stop, and Escape stops it with nothing done. What was said is written out
// by speech to text as #148's dictation is, and understood by the grammar of
// the command table, or by the model where the grammar doesn't know it
// (VoiceCommands); what neither understands is said plainly. With the AI Off
// nothing listens.
Item {
    id: voice
    // The reading view (ReaderView.qml), and the companion beside it.
    required property Item host
    required property Item aiPane

    readonly property var dictation: backend.dictation
    readonly property var commands: backend.voiceCommands
    // The number Dictation gave what is listened for; 0 when nothing is.
    property int field: 0
    readonly property bool listening: field !== 0 && dictation.field === field && dictation.listening
    readonly property bool writing: field !== 0 && dictation.field === field && dictation.writing
    // Listening, writing out what was said, or working out what it means.
    readonly property bool busy: listening || writing || commands.asking
    // A quick press keeps listening until the next; held longer, it is push to talk.
    readonly property int holdMs: 400
    property real pressedAt: 0
    property bool held: false

    function press() {
        // The key repeating while it is held down.
        if (held) return
        if (listening) {
            dictation.stop()
            return
        }
        if (writing || commands.asking || dictation.field !== 0) return
        const status = dictation.status(true, false)
        if (!status.available) {
            backend.say(status.message || qsTr("Nothing can be heard: speech to text isn't available."))
            return
        }
        pressedAt = Date.now()
        held = true
        field = dictation.listenForCommand()
    }
    function release() {
        if (!held) return
        held = false
        if (listening && Date.now() - pressedAt >= holdMs) dictation.stop()
    }
    // Escape: whatever was said is let go of, and nothing is done.
    function cancel() {
        held = false
        if (field !== 0) dictation.cancel()
        field = 0
        commands.cancel()
    }

    // What was understood, done as its key would do it, or as only said.
    function run(command, number, words, said) {
        if (command === "spoken.chapter") return chapter(number)
        if (command === "spoken.highlight") return highlight()
        if (command === "spoken.meaning") return meaning(words)
        if (!win.run(command)) backend.say(qsTr("Heard “%1”, but that can't be done here now.").arg(said))
    }
    function chapter(number) {
        const at = commands.chapter(reader.toc, number)
        if (at < 0) {
            backend.say(qsTr("This book has no chapter %1.").arg(number))
            return
        }
        const entry = reader.toc[at]
        reader.goTo(entry.section, entry.anchor || "")
        backend.say(entry.title)
    }
    function selected() { return host.content && host.content.selectedText ? host.content.selectedText.trim() : "" }
    function highlight() {
        if (selected() === "") {
            backend.say(qsTr("Select the words to highlight first: V, the mouse, a finger or the pen."))
            return
        }
        host.highlighting.highlight(reader.highlights.colors.length > 0 ? reader.highlights.colors[0].key : "yellow")
    }
    function meaning(words) {
        const word = words !== "" ? words : selected()
        if (word === "") {
            backend.say(qsTr("Select the word first, or say it: “what does serendipity mean”."))
            return
        }
        host.showAssistant = true
        if (assistant.busy) {
            backend.say(qsTr("The AI is still answering."))
            return
        }
        // Asked by voice, so its answer is read aloud too, as #149's is.
        aiPane.send(qsTr("What does “%1” mean here?").arg(word), "chapter", true)
    }

    Connections {
        target: voice.dictation
        function onHeard(field, text) {
            if (field !== voice.field) return
            voice.field = 0
            voice.commands.understand(text, translator.language)
        }
        function onFailed(field, why) {
            if (field !== voice.field) return
            voice.field = 0
            voice.held = false
            backend.say(why)
        }
    }
    Connections {
        target: voice.commands
        function onUnderstood(command, number, words, said) { voice.run(command, number, words, said) }
        function onNotUnderstood(said) {
            backend.say(said === "" ? qsTr("Nothing was heard.")
                        : qsTr("Not understood: “%1”. Say, for instance, “next page” or “go to chapter 5”.").arg(said))
        }
    }
    Component.onDestruction: if (field !== 0) dictation.forget(field)

    // The key's release, for push to talk: a Shortcut is told only that it was pressed.
    HeldKeys {
        window: voice.Window.window
        keys: (custom.chordKeys["reader.voice"] || []).concat(custom.plainKeys["reader.voice"] || [])
        onReleased: voice.release()
    }
}
