import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A tour of the real window: everything dims but the part a card is about,
// and the card sits beside it. The same overlay walks the first-use tour
// (tour.cpp) and what a release brought (whatsnew.cpp); both are lists of
// steps, each naming what to point at by its objectName.
//
// Next and Back are the keys that turn a page, Escape leaves at any time,
// and every key is taken while it runs, so nothing happens to the library
// behind it.
Item {
    id: tour
    objectName: "tour"
    visible: running
    z: 10

    property bool running: false
    // "tour" for the first-use tour, "news" for what a release brought.
    property string kind: ""
    // Where it was started: the tour ends if the window goes elsewhere.
    property string place: ""
    property var steps: []
    property int at: 0
    readonly property var step: running && at >= 0 && at < steps.length ? steps[at] : ({})
    // What the step points at, and a pane that was opened to show it.
    property Item target: null
    property string revealed: ""
    // Counted up whenever the target may have moved.
    property int tick: 0
    readonly property rect spot: {
        tick
        width
        height
        if (!target || !target.visible)
            return Qt.rect(0, 0, 0, 0)
        const corner = target.mapToItem(tour, 0, 0)
        const left = Math.max(0, corner.x - 6), top = Math.max(0, corner.y - 6)
        return Qt.rect(left, top, Math.min(width, corner.x + target.width + 6) - left,
                       Math.min(height, corner.y + target.height + 6) - top)
    }
    readonly property bool pointing: spot.width > 0 && spot.height > 0
    // The lit part, which glides from one step's to the next's. With
    // nothing to point at it closes to a point in the middle.
    property real holeX: pointing ? spot.x : width / 2
    property real holeY: pointing ? spot.y : height / 2
    property real holeWidth: pointing ? spot.width : 0
    property real holeHeight: pointing ? spot.height : 0
    Behavior on holeX { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    Behavior on holeY { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    Behavior on holeWidth { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    Behavior on holeHeight { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

    // The item a step names, if it is on screen. Several names separated
    // by | are tried in turn, and "book" is the selected book in whichever
    // view is showing.
    function locate(names) {
        for (const name of String(names).split("|")) {
            if (name === "")
                continue
            if (name !== "book") {
                const item = win.shown(name)
                if (item)
                    return item
                continue
            }
            const pulled = win.shown("pulledOut")
            if (pulled)
                return pulled
            for (const view of ["grid", "list", "carousel", "stacks"]) {
                const shelf = win.shown(view)
                if (shelf && shelf.currentItem)
                    return shelf.currentItem
            }
            return win.shown("shelfLoader")
        }
        return null
    }

    // "tour" walks the window as it is now; "news" shows what arrived since
    // the version last seen, and "allnews" everything the newest release
    // brought. False when there is nothing to show.
    function start(what) {
        if (running)
            return false
        kind = what === "tour" ? "tour" : "news"
        place = win.inReader ? "reader" : "library"
        const all = what === "tour" ? manual.tour(place, backend.books.total === 0, win.touch) : manual.news(what === "allnews")
        // The first-use tour never points at what isn't there (the details
        // of a book in a narrow window, a button only fingers get). What is
        // new is said whether or not it can be pointed at.
        steps = kind === "tour" ? all.filter(step => step.spot === "" || win.canReveal(step.spot) || locate(step.spot) !== null) : all
        if (steps.length === 0)
            return false
        at = 0
        running = true
        enter()
        forceActiveFocus()
        return true
    }
    function enter() {
        if (revealed !== "") {
            win.reveal(revealed, false)
            revealed = ""
        }
        target = null
        const spot = step.spot || ""
        if (spot !== "") {
            if (locate(spot) === null && win.canReveal(spot)) {
                win.reveal(spot, true)
                revealed = spot
            }
            target = locate(spot)
        }
        tick += 1
        settle.restart()
    }
    function go(by) {
        if (at + by < 0)
            return
        if (at + by >= steps.length) {
            finish(true)
            return
        }
        at += by
        enter()
    }
    // Finished or left, it is remembered either way, and the keyboard goes
    // back to where it was.
    function finish(completed) {
        if (!running)
            return
        if (revealed !== "") {
            win.reveal(revealed, false)
            revealed = ""
        }
        running = false
        target = null
        if (kind === "tour")
            manual.tourAnswered(place, completed)
        else
            manual.newsAnswered()
        win.restoreFocus()
    }

    // A pane that was just opened has its place only after the next layout.
    Timer {
        id: settle
        interval: 40
        onTriggered: {
            if (tour.running && tour.target === null && (tour.step.spot || "") !== "")
                tour.target = tour.locate(tour.step.spot)
            tour.tick += 1
        }
    }
    Connections {
        target: win
        function onInReaderChanged() { tour.finish(false) }
    }

    Keys.onPressed: (event) => {
        // The arrow that points onward is the other one in a right-to-left language.
        const onward = win.rightToLeft ? Qt.Key_Left : Qt.Key_Right
        const back = win.rightToLeft ? Qt.Key_Right : Qt.Key_Left
        const forward = [onward, Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space, Qt.Key_PageDown, Qt.Key_Down, Qt.Key_L, Qt.Key_J, Qt.Key_N]
        const backward = [back, Qt.Key_Backspace, Qt.Key_PageUp, Qt.Key_Up, Qt.Key_H, Qt.Key_K, Qt.Key_P]
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Q) tour.finish(false)
        else if (forward.includes(event.key)) tour.go(1)
        else if (backward.includes(event.key)) tour.go(-1)
        event.accepted = true
    }

    // Clicks, taps and the wheel stop here.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: (wheel) => { wheel.accepted = true }
    }

    // The window, washed out in its own background color so it works on a
    // light theme and a dark one, around the part that stays lit.
    Repeater {
        model: [
            Qt.rect(0, 0, tour.width, tour.holeY),
            Qt.rect(0, tour.holeY + tour.holeHeight, tour.width, tour.height - tour.holeY - tour.holeHeight),
            Qt.rect(0, tour.holeY, tour.holeX, tour.holeHeight),
            Qt.rect(tour.holeX + tour.holeWidth, tour.holeY, tour.width - tour.holeX - tour.holeWidth, tour.holeHeight)
        ]
        delegate: Rectangle {
            required property rect modelData
            x: modelData.x
            y: modelData.y
            width: Math.max(0, modelData.width)
            height: Math.max(0, modelData.height)
            color: Qt.alpha(theme.background, 0.82)
        }
    }
    Rectangle {
        objectName: "tourSpot"
        visible: tour.holeWidth > 1 && tour.holeHeight > 1
        x: tour.holeX
        y: tour.holeY
        width: tour.holeWidth
        height: tour.holeHeight
        color: "transparent"
        radius: theme.radius
        border.width: 2
        border.color: theme.accent
    }

    Rectangle {
        id: card
        objectName: "tourCard"
        width: Math.min(380, tour.width - 32)
        height: about.implicitHeight + 32
        radius: theme.radius
        color: theme.background
        border.color: theme.colors.muted

        // Below what it is about where there is room, else above it, else
        // beside it, and in the middle of the window when it is about
        // nothing in particular.
        readonly property point where: {
            const gap = 14, edge = 16
            const within = (value, low, high) => Math.max(low, Math.min(value, Math.max(low, high)))
            if (!tour.pointing)
                return Qt.point((tour.width - width) / 2, (tour.height - height) / 2)
            const spot = tour.spot
            const across = within(spot.x + spot.width / 2 - width / 2, edge, tour.width - width - edge)
            if (spot.y + spot.height + gap + height <= tour.height - edge)
                return Qt.point(across, spot.y + spot.height + gap)
            if (spot.y - gap - height >= edge)
                return Qt.point(across, spot.y - gap - height)
            const down = within(spot.y + spot.height / 2 - height / 2, edge, tour.height - height - edge)
            if (spot.x - gap - width >= edge)
                return Qt.point(spot.x - gap - width, down)
            if (spot.x + spot.width + gap + width <= tour.width - edge)
                return Qt.point(spot.x + spot.width + gap, down)
            return Qt.point(across, tour.height - height - edge)
        }
        x: where.x
        y: where.y
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        ColumnLayout {
            id: about
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 10

            Label {
                objectName: "tourTitle"
                Layout.fillWidth: true
                text: tour.step.title || ""
                font.pixelSize: 17
                font.bold: true
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "tourText"
                Layout.fillWidth: true
                text: tour.step.html || ""
                textFormat: Text.RichText
                wrapMode: Text.Wrap
                lineHeight: 1.3
            }
            // The key of the command a step is about, and what it does.
            RowLayout {
                visible: (tour.step.command || "") !== "" && tour.kind === "news"
                Layout.fillWidth: true
                spacing: 10
                Rectangle {
                    Layout.preferredWidth: keyName.implicitWidth + 16
                    Layout.preferredHeight: 24
                    radius: Math.min(4, theme.radius)
                    color: "transparent"
                    border.color: theme.colors.muted
                    Label {
                        id: keyName
                        anchors.centerIn: parent
                        text: tour.step.key || ""
                        color: theme.accent
                        font.bold: true
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: tour.step.what || ""
                    color: theme.colors.muted
                    elide: Text.ElideRight
                }
            }
            // Where in the tour this is, and under it the buttons: side by
            // side they are wider than the card in a language of long words.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 8

                // A dot a step, or the count when there are too many for dots.
                Row {
                    objectName: "tourDots"
                    visible: tour.steps.length <= 9
                    spacing: 5
                    Repeater {
                        model: tour.steps.length
                        delegate: Rectangle {
                            required property int index
                            width: 6
                            height: 6
                            radius: 3
                            color: index === tour.at ? theme.accent : theme.colors.selection
                            border.color: index === tour.at ? theme.accent : theme.colors.muted
                        }
                    }
                }
                Label {
                    visible: tour.steps.length > 9
                    text: qsTr("%1 of %2").arg(tour.at + 1).arg(tour.steps.length)
                    color: theme.colors.muted
                    font.pixelSize: 12
                }
                DialogActions {
                AppButton {
                    objectName: "tourSkip"
                    visible: tour.at < tour.steps.length - 1
                    text: qsTr("Skip")
                    focusPolicy: Qt.NoFocus
                    onClicked: tour.finish(false)
                }
                AppButton {
                    objectName: "tourBack"
                    visible: tour.at > 0
                    text: qsTr("Back")
                    focusPolicy: Qt.NoFocus
                    onClicked: tour.go(-1)
                }
                AppButton {
                    objectName: "tourNext"
                    primary: true
                    text: tour.at < tour.steps.length - 1 ? qsTr("Next") : qsTr("Done")
                    focusPolicy: Qt.NoFocus
                    onClicked: tour.go(1)
                }
                }
            }
        }
    }
}
