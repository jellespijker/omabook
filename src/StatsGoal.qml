import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The year's reading goal in the statistics (#68): how far it is, as words
// and a bar, and setting it, changing it or taking it away. A goal of the
// whole library, so it is shown only there, not of one collection.
ColumnLayout {
    id: goalPart
    objectName: "statsGoal"
    property var view
    property bool shown: true
    readonly property var goal: (view.summary || {}).goal || null
    property bool editing: false
    visible: shown
    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        visible: !goalPart.editing
        spacing: 12
        Label {
            objectName: "statsGoalText"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: !goalPart.goal ? qsTr("No goal for %1.").arg(goalPart.view.year)
                  : goalPart.goal.kind === "hours"
                    ? qsTr("Goal: %1 of %Ln hours", "", goalPart.goal.target).arg(Number(Math.floor(goalPart.goal.done)).toLocaleString(Qt.locale(), "f", 0))
                    : qsTr("Goal: %1 of %Ln books", "", goalPart.goal.target).arg(Number(goalPart.goal.done).toLocaleString(Qt.locale(), "f", 0))
        }
        AppButton {
            objectName: "statsGoalEdit"
            text: goalPart.goal ? qsTr("Change the goal") : qsTr("Set a goal")
            onClicked: {
                target.value = goalPart.goal ? goalPart.goal.target : 24
                kind.currentIndex = goalPart.goal && goalPart.goal.kind === "hours" ? 1 : 0
                goalPart.editing = true
            }
        }
    }
    // How far: the fill in the chart's colour, the track a lighter step of it.
    Rectangle {
        objectName: "statsGoalBar"
        visible: !!goalPart.goal && !goalPart.editing
        Layout.fillWidth: true
        implicitHeight: 8
        radius: 4
        readonly property color fill: goalPart.view.mark(theme.accent, theme.background)
        color: Qt.rgba(fill.r, fill.g, fill.b, 0.22)
        Rectangle {
            height: parent.height
            radius: 4
            width: !goalPart.goal ? 0 : parent.width * Math.min(1, goalPart.goal.done / Math.max(1, goalPart.goal.target))
            color: parent.fill
        }
    }
    Flow {
        Layout.fillWidth: true
        visible: goalPart.editing
        spacing: 8
        SpinBox {
            id: target
            objectName: "statsGoalTarget"
            from: 1
            to: 1000
            editable: true
            Accessible.name: qsTr("How many")
        }
        ComboBox {
            id: kind
            objectName: "statsGoalKind"
            model: [qsTr("books"), qsTr("hours")]
        }
        AppButton {
            objectName: "statsGoalSave"
            primary: true
            text: qsTr("Save")
            onClicked: {
                goalPart.view.setGoal(kind.currentIndex === 1 ? "hours" : "books", target.value)
                goalPart.editing = false
            }
        }
        AppButton {
            objectName: "statsGoalRemove"
            visible: !!goalPart.goal
            text: qsTr("No goal")
            onClicked: {
                goalPart.view.setGoal("books", 0)
                goalPart.editing = false
            }
        }
        AppButton { text: qsTr("Cancel"); onClicked: goalPart.editing = false }
    }
}
