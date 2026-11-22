import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A row's Test on the AI page (#170): one small real request through exactly
// what the row uses (its provider, server, key and model), and what came back
// with how long it took, or why not, worded here from its code. A test that
// would go to someone else's computer where the row doesn't allow it asks
// first; with the AI Off nothing is tested.
ColumnLayout {
    id: check
    property var info: ({})
    readonly property string capability: info.capability || ""
    readonly property var desk: aiAgent.capabilities
    readonly property var outcome: info.test || ({})
    readonly property bool running: outcome.state === "running"
    // Whether it can be tested now: the AI on, and none under way.
    readonly property bool ready: desk.aiOn && !running
    // Asking before this one test goes to someone else's computer.
    property bool asking: false
    spacing: 8

    function test() {
        if (!ready)
            return
        if (info.why === "hosted-not-allowed") {
            asking = true
            return
        }
        desk.test(capability, false)
    }
    function seconds(ms) { return (Number(ms || 0) / 1000).toLocaleString(Qt.locale(), 'f', 1) }
    function passed(outcome) {
        const time = seconds(outcome.ms)
        if (capability === "embeddings")
            return qsTr("It made a vector of %1 numbers in %2 s.").arg(outcome.said).arg(time)
        if (!outcome.said)
            return qsTr("It answered with nothing in %1 s.").arg(time)
        switch (capability) {
        case "handwriting": return qsTr("It read “%1” in %2 s.").arg(outcome.said).arg(time)
        case "speech-to-text": return qsTr("It heard “%1” in %2 s.").arg(outcome.said).arg(time)
        case "text-to-speech": return qsTr("It said “%1” in %2 s.").arg(outcome.said).arg(time)
        }
        return qsTr("It answered “%1” in %2 s.").arg(outcome.said).arg(time)
    }
    function why(code) {
        switch (code) {
        case "ai-off": return qsTr("Off, as the AI is: nothing was sent.")
        case "no-route": return qsTr("No route: it can't be reached, or isn't set up.")
        case "refused-key": return qsTr("Refused key: its key was refused, or it needs one.")
        case "no-model": return qsTr("Model not found: it hasn't the model asked for.")
        case "timeout": return qsTr("Timed out: it didn't answer in time.")
        case "hosted-not-allowed": return qsTr("Hosted not allowed: it is someone else's computer, and hosted isn't allowed for this.")
        }
        return qsTr("It failed.")
    }
    readonly property string said: {
        if (running)
            return qsTr("Testing…")
        if (outcome.state === "passed")
            return passed(outcome)
        if (outcome.state === "failed")
            return [why(outcome.why), outcome.message || "", qsTr("(%1 s)").arg(seconds(outcome.ms))].filter(part => !!part).join(" ")
        return ""
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        AppButton {
            objectName: check.capability + "Test"
            text: qsTr("Test")
            enabled: check.ready && !check.asking
            Accessible.description: qsTr("Sends one small request through this row's provider and model (T).")
            onClicked: check.test()
        }
        Label {
            objectName: check.capability + "TestResult"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: check.outcome.state === "failed" ? theme.colors.orange : theme.foreground
            text: check.said
        }
    }
    // Hosted isn't allowed for it: this one test goes there only on a yes.
    Label {
        objectName: check.capability + "TestAsk"
        Layout.fillWidth: true
        visible: check.asking
        wrapMode: Text.Wrap
        text: qsTr("Only a hosted service is set up for this, and hosted isn't allowed for it: the test would go to someone else's computer. Send this one test there?")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: check.asking
        AppButton {
            objectName: check.capability + "TestHosted"
            primary: true
            text: qsTr("Send the Test")
            onClicked: {
                check.asking = false
                check.desk.test(check.capability, true)
            }
        }
        AppButton {
            objectName: check.capability + "TestCancel"
            text: qsTr("Cancel")
            onClicked: check.asking = false
        }
        Item { Layout.fillWidth: true }
    }
}
