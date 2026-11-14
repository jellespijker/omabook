import QtQuick

// What the shelf calls things: the names of its views, orders, filters and
// reading states, in one place so every view says them the same way.
QtObject {
    function view(key) {
        return key === "spines" ? qsTr("Spines") : key === "carousel" ? qsTr("Carousel")
             : key === "list" ? qsTr("List") : key === "stacks" ? qsTr("Stacks") : qsTr("Covers")
    }
    function sort(key) {
        return key === "title" ? qsTr("Title") : key === "author" ? qsTr("Author") : key === "series" ? qsTr("Series")
             : key === "added" ? qsTr("Recently added") : key === "progress" ? qsTr("Progress")
             : key === "rating" ? qsTr("Rating") : key === "published" ? qsTr("Published")
             : key === "finished" ? qsTr("Finished") : qsTr("Recently read")
    }
    function group(kind) {
        return kind === "author" ? qsTr("By author") : kind === "tag" ? qsTr("By tag")
             : kind === "status" ? qsTr("By status") : qsTr("By series")
    }
    function status(status) {
        return status === "to-read" ? qsTr("To read") : status === "reading" ? qsTr("Reading")
             : status === "read" ? qsTr("Read") : status === "unread" ? qsTr("Unread") : qsTr("Not started")
    }
    // A book that is not a file says what it is instead; one that is says nothing.
    function held(held) {
        return held === "paper" || held === "both" ? qsTr("Paper")
             : held === "none" || held === "wishlist" ? qsTr("Wishlist")
             : held === "digital" ? qsTr("Digital") : ""
    }
    function kind(kind) {
        return kind === "status" ? qsTr("Status") : kind === "held" ? qsTr("Held as") : kind === "rating" ? qsTr("Rating")
             : kind === "format" ? qsTr("Format") : kind === "language" ? qsTr("Language") : kind === "tag" ? qsTr("Tag")
             : kind === "author" ? qsTr("Author") : qsTr("Series")
    }
    // A filter's value as it is shown; "-" stands for books that have none.
    function value(kind, value) {
        if (kind === "status") return status(value === "-" ? "" : value)
        if (kind === "held") return held(value)
        if (kind === "rating") return qsTr("%1 or more").arg(Number(value).toLocaleString(Qt.locale(), 'f', 0))
        if (kind === "format") return String(value).toUpperCase()
        if (value !== "-") return String(value)
        return kind === "series" ? qsTr("No series") : kind === "author" ? qsTr("No author")
             : kind === "tag" ? qsTr("No tags") : qsTr("No language")
    }
    // A day as the desktop's language writes it, from seconds since the epoch.
    function day(seconds) {
        return seconds > 0 ? new Date(seconds * 1000).toLocaleDateString(Qt.locale(), Locale.ShortFormat) : ""
    }
}
