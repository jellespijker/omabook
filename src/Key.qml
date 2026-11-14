import QtQuick

// A command and the keys that run it, which are whatever keys.json says.
// Keys pressed alone are shortcuts only while nothing is being typed and no
// dialog is up; keys with a modifier work whenever no dialog is.
Item {
    id: key
    property string command
    // Whether this is the place the command belongs to right now.
    property bool when: true
    signal activated()

    Shortcut {
        sequences: custom.plainKeys[key.command] || []
        enabled: key.when && win.keys
        onActivated: key.activated()
    }
    Shortcut {
        sequences: custom.chordKeys[key.command] || []
        enabled: key.when && !win.dialogOpen
        onActivated: key.activated()
    }
}
