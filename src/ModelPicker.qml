import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One row of the model picker: the model one kind of work goes to, chosen
// from those known for the AI in use, or typed, since any name it takes will
// do. The first choice is always the default.
GridLayout {
    id: row

    // "main" (answers and writing) or "quick" (reading and parsing).
    property string tier: "main"
    property string label

    readonly property string chosen: tier === "quick" ? aiAgent.quickChoice : aiAgent.mainChoice
    // What the default is called: the model's name where one is known.
    readonly property string defaultText: {
        const model = tier === "quick" ? aiAgent.quickDefault : aiAgent.mainDefault
        if (model !== "")
            return qsTr("The default (%1)").arg(model)
        return tier === "quick" ? qsTr("The same as for answers") : qsTr("The default")
    }

    // What it is for beside the model, and over it in a compact window,
    // where the two would leave the model's name no room.
    columns: win.compact ? 1 : 2
    columnSpacing: 12
    rowSpacing: 4

    Label {
        text: row.label
        // A width of its own, not one the layout works out from its text:
        // a wrapping text whose width follows its lines never settles.
        Layout.fillWidth: win.compact
        Layout.minimumWidth: win.compact ? 0 : 190
        Layout.preferredWidth: 190
        Layout.maximumWidth: win.compact ? Number.POSITIVE_INFINITY : 190
        wrapMode: Text.Wrap
    }
    ComboBox {
        id: box
        objectName: row.tier + "Model"
        Layout.fillWidth: true
        editable: true
        model: [row.defaultText].concat(aiAgent.knownModels)
        Material.roundedScale: theme.radius

        // Shows what the settings hold, also a name that isn't in the list.
        function show() {
            const at = row.chosen === "" ? 0 : aiAgent.knownModels.indexOf(row.chosen) + 1
            currentIndex = row.chosen === "" || at > 0 ? at : -1
            editText = row.chosen === "" ? row.defaultText : row.chosen
        }
        function take(text) {
            const name = text.trim()
            aiAgent.setModel(row.tier, name === row.defaultText || name === "" ? "" : name)
            show()
        }

        // A model that sends the text on to another service says so.
        delegate: ItemDelegate {
            required property int index
            required property string modelData
            width: ListView.view ? ListView.view.width : 200
            text: index > 0 && aiAgent.elsewhereModels.includes(modelData) ? qsTr("%1 (runs elsewhere)").arg(modelData) : modelData
            highlighted: box.highlightedIndex === index
        }

        onActivated: (index) => take(index === 0 ? "" : aiAgent.knownModels[index - 1])
        onAccepted: take(editText)
        onModelChanged: show()
        Component.onCompleted: show()

        Connections {
            target: aiAgent
            function onChanged() { box.show() }
        }
        // A typed name counts once the field is left, as well as on Enter.
        Connections {
            target: box.contentItem
            ignoreUnknownSignals: true
            function onEditingFinished() { box.take(box.editText) }
        }
    }
}
