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

    Key { command: "reader.back"; when: win.inReader && !keys.host.mapping; onActivated: keys.host.back() }
    // Left and Right follow the reading direction: forward is leftwards in
    // a right-to-left language, as a finger's swipe is.
    Key { command: "reader.page_down"; mirrored: win.rightToLeft; when: keys.host.paging; onActivated: keys.host.content.pageDown() }
    Key { command: "reader.page_up"; mirrored: win.rightToLeft; when: keys.host.paging; onActivated: keys.host.content.pageUp() }
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
    Key { command: "reader.marks"; when: reader.open; onActivated: keys.host.cycleMarks() }
    Key { command: "reader.pages"; when: reader.open && !keys.host.mapping; onActivated: keys.host.cyclePages() }
    Key { command: "reader.layout"; when: reader.open && !keys.host.mapping; onActivated: keys.host.cycleLayout() }
    Key { command: "reader.select"; when: keys.host.paging; onActivated: keys.host.startSelecting("menu") }
    Key { command: "reader.note"; when: keys.host.paging; onActivated: keys.notes.write() }
    Key { command: "reader.notes"; when: keys.host.paging; onActivated: keys.notes.go() }
}
