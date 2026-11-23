import QtQml

// The words of the snapshots' panel (#185): a snapshot by when it was taken
// and why (a moment snapshot by what it came before), a book's details by
// their names, and counts of what came, went or changed.
QtObject {
    function when(iso) {
        if (!iso)
            return ""
        return new Date(iso).toLocaleString(Qt.locale(), Locale.ShortFormat)
    }
    function why(snapshot) {
        if (!snapshot || !snapshot.id)
            return ""
        if (snapshot.kind === "nightly")
            return qsTr("Nightly")
        switch (snapshot.reason) {
        case "asked": return qsTr("Taken when asked")
        case "sync": return qsTr("Before a sync")
        case "import": return qsTr("Before an import")
        case "tidy": return qsTr("Before a tidy")
        case "merge": return qsTr("Before a merge")
        case "restore": return qsTr("Before a restore")
        case "upgrade": return qsTr("Before an upgrade")
        case "bulk-change": return qsTr("Before a change to many books")
        default: return qsTr("A moment snapshot")
        }
    }
    // "20 Nov 2026 14:02 · Before a sync"
    function named(snapshot) {
        return snapshot && snapshot.id ? "%1 · %2".arg(when(snapshot.time)).arg(why(snapshot)) : ""
    }
    function field(name) {
        switch (name) {
        case "title": return qsTr("Title")
        case "authors": return qsTr("Authors")
        case "series": return qsTr("Series")
        case "series_index": return qsTr("Number in the series")
        case "publisher": return qsTr("Publisher")
        case "language": return qsTr("Language")
        case "isbn": return qsTr("ISBN")
        case "published": return qsTr("Published")
        case "description": return qsTr("Description")
        case "tags": return qsTr("Tags")
        }
        return name.startsWith("identifiers.") ? qsTr("Identifier %1").arg(name.slice(12)) : name
    }
    function kind(name) {
        switch (name) {
        case "highlight": return qsTr("Highlights")
        case "bookmark": return qsTr("Bookmarks")
        case "margin-note": return qsTr("Margin notes")
        case "ink": return qsTr("Ink pages")
        case "things": return qsTr("Things")
        case "relations": return qsTr("Relations")
        case "facts": return qsTr("Facts")
        }
        return name
    }
    // A detail's value as a line shows it.
    function value(text) {
        if (!text)
            return qsTr("(none)")
        const line = String(text).replace(/\s+/g, " ").trim()
        return line.length > 70 ? line.slice(0, 69) + "…" : line
    }
    // "Highlights: 2 added, 1 removed; Ink pages: 1 changed", of {kind: {added, removed, changed}}.
    function counts(byKind) {
        const kinds = []
        for (const name in byKind || {}) {
            const count = byKind[name]
            const said = []
            if (count.added > 0) said.push(qsTr("%n added", "", count.added))
            if (count.removed > 0) said.push(qsTr("%n removed", "", count.removed))
            if (count.changed > 0) said.push(qsTr("%n changed", "", count.changed))
            if (said.length > 0)
                kinds.push(qsTr("%1: %2").arg(kind(name)).arg(said.join(", ")))
        }
        return kinds.join("; ")
    }
    // The same, of what putting a book back does: from now to the snapshot.
    function putBack(byKind) {
        const kinds = []
        for (const name in byKind || {}) {
            const count = byKind[name]
            const said = []
            if (count.added > 0) said.push(qsTr("%n to come back", "", count.added))
            if (count.changed > 0) said.push(qsTr("%n to change back", "", count.changed))
            if (count.removed > 0) said.push(qsTr("%n made since, which stay", "", count.removed))
            if (said.length > 0)
                kinds.push(qsTr("%1: %2").arg(kind(name)).arg(said.join(", ")))
        }
        return kinds.join("; ")
    }
}
