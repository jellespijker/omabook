import QtQuick

// The plain words for what a plugin brings (fields of book details), asks to
// be allowed (its permissions) and listens to (events), as the plugin manager
// and the install sheet say them (PluginsDialog).
QtObject {
    readonly property var fieldNames: ({
        title: qsTr("title"), authors: qsTr("authors"), series: qsTr("series"), publisher: qsTr("publisher"),
        published: qsTr("date"), language: qsTr("language"), isbn: qsTr("ISBN"), description: qsTr("description"),
        tags: qsTr("tags"), cover: qsTr("cover")
    })
    // What a script plugin asked to be allowed in plugin.json, in plain
    // words; a program plugin is not held to any of it.
    readonly property var permissionNames: ({
        http: qsTr("the network"), files: qsTr("files in its own folder"), run: qsTr("programs on this computer"),
        microphone: qsTr("the microphone"), clipboard: qsTr("the clipboard, right after you use it"),
        program: qsTr("everything you can: it is a program of its own, not held to permissions")
    })
    readonly property var eventNames: ({
        "app.started": qsTr("Omabook starts"), "library.switched": qsTr("another library is opened"),
        "book.added": qsTr("a book is added"), "book.removed": qsTr("a book is removed"),
        "book.changed": qsTr("a book's details change"), "book.opened": qsTr("a book is opened"),
        "book.closed": qsTr("a book is closed"), "page.turned": qsTr("a page is turned"),
        "lookup.finished": qsTr("a look-up has finished"), "log.changed": qsTr("its data comes from another computer")
    })
    function fieldList(fields) { return (fields || []).map(field => fieldNames[field] || field).join(", ") }
    function permissionText(permissions) {
        return (permissions || []).length === 0 ? qsTr("nothing beyond the library")
               : permissions.map(permission => permissionNames[permission] || permission).join(", ")
    }
}
