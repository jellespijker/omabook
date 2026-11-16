import QtQuick

// A command and the keys that run it, which are whatever keys.json says.
// Keys pressed alone are shortcuts only while nothing is being typed and no
// dialog is up; keys with a modifier work whenever no dialog is.
Item {
    id: key
    property string command
    // Whether this is the place the command belongs to right now.
    property bool when: true
    // Left and Right the other way round, for a command that goes with the
    // reading direction in a right-to-left language.
    property bool mirrored: false
    signal activated()
    function turned(keys) {
        if (!mirrored || !keys) return keys || []
        return keys.map(k => k.replace(/\b(Left|Right)\b/g, (side) => side === "Left" ? "Right" : "Left"))
    }

    Shortcut {
        sequences: key.turned(custom.plainKeys[key.command])
        enabled: key.when && win.keys
        onActivated: key.activated()
    }
    Shortcut {
        sequences: key.turned(custom.chordKeys[key.command])
        enabled: key.when && !win.dialogOpen
        onActivated: key.activated()
    }
}
