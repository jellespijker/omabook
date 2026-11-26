import QtQuick

// The keys of the open book (ReaderView.qml, its `host`): each command of the commands
// table's place "reader", run while it is the page's, not the map's, a
// plugin's view's beside the page or the Aa panel's.
Item {
    id: keys
    required property Item host
    // The notes beside the text, and the AI beside the page.
    required property Item notes
    required property Item aiPane

    // Commands said (#150); Escape while one is listened for lets it go, rather than closing the book.
    readonly property alias voice: voice
    ReaderVoice { id: voice; host: keys.host; aiPane: keys.aiPane }
    Key {
        command: "reader.voice"
        when: win.inReader && !!keys.host.content && !keys.host.mapping && !keys.host.formatting && (!keys.host.popupOpen || keys.host.selecting)
        onActivated: voice.press()
    }
    Key { command: "reader.back"; when: win.inReader && !keys.host.mapping; onActivated: voice.busy ? voice.cancel() : keys.host.back() }
    // Left and Right follow the reading direction: forward is leftwards in a
    // right-to-left book, as a finger's swipe is.
    Key { command: "reader.page_down"; mirrored: win.pagesRightToLeft; when: keys.host.paging; onActivated: keys.host.content.pageDown() }
    Key { command: "reader.page_up"; mirrored: win.pagesRightToLeft; when: keys.host.paging; onActivated: keys.host.content.pageUp() }
    Key { command: "reader.line_down"; when: keys.host.paging; onActivated: keys.host.content.lineDown() }
    Key { command: "reader.line_up"; when: keys.host.paging; onActivated: keys.host.content.lineUp() }
    Key { command: "reader.next"; when: keys.host.paging; onActivated: keys.host.content.next() }
    Key { command: "reader.previous"; when: keys.host.paging; onActivated: keys.host.content.previous() }
    Key { command: "reader.start"; when: keys.host.paging; onActivated: keys.host.content.toStart() }
    Key { command: "reader.end"; when: keys.host.paging; onActivated: keys.host.content.toEnd() }
    Key { command: "reader.larger"; when: win.inReader && !!keys.host.content && !keys.host.mapping && !keys.host.formatting; onActivated: keys.host.content.zoom(1) }
    Key { command: "reader.smaller"; when: win.inReader && !!keys.host.content && !keys.host.mapping && !keys.host.formatting; onActivated: keys.host.content.zoom(-1) }
    Key { command: "reader.format"; when: reader.open && !keys.host.mapping; onActivated: keys.host.toggleFormat() }
    Key { command: "reader.contents"; when: reader.open; onActivated: keys.host.showList("contents") }
    Key { command: "reader.annotations"; when: reader.open; onActivated: keys.host.showList("annotations") }
    Key { command: "reader.catch_up"; when: reader.open; onActivated: keys.host.catchUp() }
    Key { command: "reader.cast"; when: reader.open; onActivated: { keys.host.showAssistant = true; assistant.whoIsWho(); keys.aiPane.takeFocus() } }
    Key { command: "reader.find"; when: reader.open && !keys.host.mapping; onActivated: keys.host.openFinder() }
    Key { command: "reader.ask"; when: reader.open; onActivated: keys.host.askAssistant() }
    Key { command: "reader.bookmark"; when: reader.open; onActivated: keys.host.bookmark() }
    Key { command: "reader.expand"; when: win.inReader && !keys.host.mapping; onActivated: keys.host.expand(!keys.host.immersive) }
    Key { command: "reader.graph"; when: reader.open; onActivated: keys.host.toggleGraph() }
    Key { command: "reader.marks"; when: reader.open; onActivated: keys.host.cycles.marks() }
    Key { command: "reader.pages"; when: reader.open && !keys.host.mapping; onActivated: keys.host.cycles.pages() }
    Key { command: "reader.layout"; when: reader.open && !keys.host.mapping; onActivated: keys.host.cycles.layout() }
    // Read aloud (#73) from the words at the top of the page, pause and go on; stop; a sentence back or on.
    Key { command: "reader.read_aloud"; when: reader.open && !keys.host.mapping; onActivated: reader.readAloud.toggle(keys.host.content ? keys.host.content.placeText() : "") }
    // A book read aloud, and a plugin reading aloud (#147), both stop.
    Key { command: "reader.read_aloud_stop"; when: reader.readAloud.active || (reader.open && backend.audio.speaking); onActivated: { reader.readAloud.stop(); backend.audio.stopSpeaking() } }
    Key { command: "reader.sentence_back"; when: reader.readAloud.active && !keys.host.mapping; onActivated: reader.readAloud.back() }
    Key { command: "reader.sentence_next"; when: reader.readAloud.active && !keys.host.mapping; onActivated: reader.readAloud.next() }
    Key { command: "reader.select"; when: keys.host.paging; onActivated: keys.host.startSelecting("menu") }
    Key { command: "reader.note"; when: keys.host.paging; onActivated: keys.notes.write() }
    Key { command: "reader.notes"; when: keys.host.paging; onActivated: keys.notes.go() }
    Key { command: "reader.fold_margin"; when: keys.host.paging && reader.kind === "flow"; onActivated: keys.notes.fold() }
}
