import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The reading format as controls: the layout (pages or a scrolling column),
// the font, its size, the measure, the pages side by side, the spacing, the
// indent, the margins and the alignment (readingformat.h, pagelayout.h). Each
// change shows on the page at once. The same controls are in the reader's
// "Aa" panel, for the open book or the defaults (`scoped`), and in the
// settings, for the defaults.
ColumnLayout {
    id: controls
    // In the reader: the values in force for the open book, which go to the
    // book alone while `bookOnly` is on. Elsewhere: the defaults.
    property bool scoped: false
    property bool bookOnly: false
    // Or to the book's collection (pageformat.h), while `forCollection` is on.
    property bool forCollection: false
    // What the controls' objectNames begin with, so the panel's and the
    // settings' are told apart.
    property string prefix: "format"
    readonly property var values: scoped ? reader.format.effective : reader.format.defaults
    readonly property var own: !scoped ? ({}) : forCollection && !bookOnly ? reader.format.collectionOwn : reader.format.own
    // Pages that are pictures: only the layout and the first page apply.
    property bool picturesOnly: false
    // The first control, for the keyboard to start at.
    readonly property Item first: layoutAuto

    spacing: 6

    function change(key, value) {
        const where = !scoped || !reader.format.hasBook ? "defaults" : bookOnly ? "book"
                      : forCollection && reader.format.collectionId > 0 ? "collection" : "defaults"
        const why = reader.format.setFor(key, value, where)
        if (why !== "") backend.say(why)
    }
    function shown(key, value) {
        const unit = reader.format.bounds[key] ? reader.format.bounds[key].unit : "px"
        const number = Number(value).toLocaleString(Qt.locale(), 'f', 0)
        return unit === "%" ? qsTr("%1%").arg(number) : unit === "characters" ? qsTr("%1 characters").arg(number) : qsTr("%1 px").arg(number)
    }

    // A number, a step at a time: the buttons, or the arrow keys, + and -,
    // Home and End while it has the keyboard.
    component Stepper: FocusScope {
        id: stepper
        required property string key
        required property string label
        readonly property var range: reader.format.bounds[key]
        readonly property int value: controls.values[key] !== undefined ? controls.values[key] : range["default"]
        readonly property bool mine: controls.own[key] !== undefined
        readonly property string valueText: controls.shown(key, value)

        objectName: controls.prefix + "_" + key
        Layout.fillWidth: true
        implicitHeight: Math.max(40, row.implicitHeight + 4)
        activeFocusOnTab: true
        Accessible.role: Accessible.SpinBox
        Accessible.name: label
        Accessible.description: valueText

        function to(wanted) { controls.change(key, Math.max(range.low, Math.min(range.high, wanted))) }
        function step(by) { to(value + by * range.step) }
        Keys.onPressed: (event) => {
            const less = win.rightToLeft ? Qt.Key_Right : Qt.Key_Left
            const more = win.rightToLeft ? Qt.Key_Left : Qt.Key_Right
            if (event.key === Qt.Key_Minus || event.key === less || event.key === Qt.Key_Down) step(-1)
            else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal || event.key === more || event.key === Qt.Key_Up) step(1)
            else if (event.key === Qt.Key_Home) to(range.low)
            else if (event.key === Qt.Key_End) to(range.high)
            else return
            event.accepted = true
        }

        Rectangle {
            anchors.fill: parent
            radius: theme.radius
            color: "transparent"
            border.width: stepper.activeFocus ? 2 : 0
            border.color: theme.foreground
        }
        RowLayout {
            id: row
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 6
            anchors.rightMargin: 2
            spacing: 4
            Label {
                Layout.fillWidth: true
                text: stepper.label
                wrapMode: Text.Wrap
            }
            AppButton {
                objectName: stepper.objectName + "_less"
                implicitWidth: 36
                implicitHeight: 36
                leftPadding: 0
                rightPadding: 0
                focusPolicy: Qt.NoFocus
                enabled: stepper.value > stepper.range.low
                text: "−"
                Accessible.name: qsTr("Less")
                onClicked: stepper.step(-1)
            }
            Label {
                objectName: stepper.objectName + "_value"
                // One width for every value, so the buttons stand in a column.
                Layout.minimumWidth: 96
                horizontalAlignment: Text.AlignHCenter
                text: stepper.valueText
                // A value the book has of its own is in the accent.
                color: stepper.mine ? theme.accent : theme.foreground
                font.bold: stepper.mine
            }
            AppButton {
                objectName: stepper.objectName + "_more"
                implicitWidth: 36
                implicitHeight: 36
                leftPadding: 0
                rightPadding: 0
                focusPolicy: Qt.NoFocus
                enabled: stepper.value < stepper.range.high
                text: "+"
                Accessible.name: qsTr("More")
                onClicked: stepper.step(1)
            }
        }
    }

    // A layout, as one of a row of buttons that says which is chosen.
    component Segment: AbstractButton {
        id: segment
        required property string word
        required property string name
        readonly property bool chosen: (controls.values.layout || "auto") === word
        objectName: controls.prefix + "Layout_" + word
        onClicked: {
            checked = Qt.binding(() => chosen)
            controls.change("layout", word)
        }
        Layout.fillWidth: true
        implicitWidth: Math.max(44, label.implicitWidth + 16)
        implicitHeight: 36
        focusPolicy: Qt.StrongFocus
        checkable: true
        checked: chosen
        Accessible.role: Accessible.RadioButton
        Accessible.name: name
        Accessible.checked: chosen
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        background: Rectangle {
            radius: theme.radius
            color: segment.chosen ? theme.accent : segment.down || segment.hovered ? theme.colors.selection : "transparent"
            border.width: segment.visualFocus ? 2 : 1
            border.color: segment.visualFocus ? theme.foreground : segment.chosen ? theme.accent : theme.colors.selection
        }
        contentItem: Label {
            id: label
            text: segment.name
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            color: segment.chosen ? theme.accentForeground : theme.foreground
        }
    }

    // How the book is read through: pages that turn, or one column that
    // scrolls on through the chapters, or each as the room has it.
    Label {
        text: qsTr("Layout")
        color: controls.own.layout !== undefined ? theme.accent : theme.colors.muted
        font.pixelSize: 12
    }
    RowLayout {
        id: layoutRow
        objectName: controls.prefix + "Layout"
        Layout.fillWidth: true
        spacing: 4
        Segment { id: layoutAuto; word: "auto"; name: qsTr("Automatic") }
        Segment { word: "pages"; name: qsTr("Pages") }
        Segment { word: "scroll"; name: qsTr("Scroll") }
    }
    Label {
        objectName: controls.prefix + "LayoutName"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
        text: controls.values.layout === "pages" ? qsTr("Pages that turn; each chapter starts on a page of its own, and a picture page is shown whole")
              : controls.values.layout === "scroll" ? qsTr("One column that scrolls on from one chapter into the next")
              : qsTr("Automatic: pages where two fit side by side, else a column that scrolls")
    }
    // Two pictures of pages side by side: the first alone, as a printed
    // book opens, or paired from the start.
    Switch {
        id: firstAlone
        objectName: controls.prefix + "FirstPage"
        Layout.fillWidth: true
        Layout.minimumHeight: 40
        visible: !controls.scoped || reader.kind === "pages"
        readonly property string word: controls.values.first_page || "auto"
        // What Automatic gives: alone for a book's first page, which is
        // taller than wide; paired for slides and comics.
        readonly property bool automatic: !controls.scoped || reader.pageAspect(0) > 1
        text: qsTr("First page alone")
        checked: word === "alone" || (word === "auto" && automatic)
        Accessible.name: text
        onToggled: controls.change("first_page", checked ? "alone" : "paired")
    }
    Label {
        objectName: controls.prefix + "FirstPageName"
        visible: firstAlone.visible
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
        text: firstAlone.checked ? qsTr("Two side by side: page 1 alone, then 2 and 3, as a printed book opens")
                                 : qsTr("Two side by side: 1 and 2, then 3 and 4, as slides and comics read")
    }

    ColumnLayout {
        id: typeControls
        Layout.fillWidth: true
        spacing: controls.spacing
        enabled: !controls.picturesOnly

    Label {
        text: qsTr("Font")
        color: controls.own.font_family !== undefined ? theme.accent : theme.colors.muted
        font.pixelSize: 12
    }
    ComboBox {
        id: fontPicker
        objectName: controls.prefix + "_font_family"
        Layout.fillWidth: true
        readonly property string desktop: qsTr("The desktop's font")
        model: [desktop].concat(reader.format.fonts)
        currentIndex: controls.values.font_family === "" ? 0 : Math.max(-1, model.indexOf(controls.values.font_family))
        displayText: controls.values.font_family === "" ? desktop : controls.values.font_family
        Material.roundedScale: theme.radius
        Accessible.name: qsTr("Font")
        onActivated: (index) => controls.change("font_family", index === 0 ? "" : model[index])
        delegate: ItemDelegate {
            required property int index
            required property string modelData
            width: ListView.view ? ListView.view.width : 200
            text: modelData
            // Each font is shown in itself.
            font.family: index === 0 ? Qt.application.font.family : modelData
            highlighted: fontPicker.highlightedIndex === index
        }
    }

    Stepper { key: "font_size"; label: qsTr("Text size") }
    Stepper { key: "measure"; label: qsTr("Longest line") }
    // What the measure comes to in this font at this size.
    Label {
        objectName: controls.prefix + "_measure_pixels"
        Layout.fillWidth: true
        Layout.leftMargin: 6
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
        text: qsTr("%1 px in this font at this size").arg(Math.round(reader.format.lineWidth).toLocaleString(Qt.locale(), 'f', 0))
    }

    // How many pages stand side by side: as many as fit, or a choice of
    // one, two or three, for every book (the setting isn't a book's own).
    Label {
        Layout.topMargin: 4
        text: qsTr("Pages side by side")
        color: theme.colors.muted
        font.pixelSize: 12
    }
    RowLayout {
        objectName: controls.prefix + "Pages"
        Layout.fillWidth: true
        spacing: 4
        // A book that scrolls is one column.
        enabled: controls.values.layout !== "scroll"
        Repeater {
            model: [0, 1, 2, 3]
            delegate: AbstractButton {
                id: count
                required property int modelData
                readonly property bool chosen: settings.pages === modelData
                readonly property string name: modelData === 0 ? qsTr("Automatic") : modelData.toLocaleString(Qt.locale(), 'f', 0)
                objectName: controls.prefix + "Pages_" + modelData
                Layout.fillWidth: true
                // Automatic is a word; the counts are a digit each.
                Layout.preferredWidth: modelData === 0 ? 120 : 44
                implicitHeight: 36
                focusPolicy: Qt.StrongFocus
                checkable: true
                checked: chosen
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData === 0 ? name : qsTr("%Ln pages", "", modelData)
                Accessible.checked: chosen
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
                onClicked: {
                    checked = Qt.binding(() => count.chosen)
                    settings.pages = modelData
                }
                background: Rectangle {
                    radius: theme.radius
                    color: count.chosen ? theme.accent : count.down || count.hovered ? theme.colors.selection : "transparent"
                    border.width: count.visualFocus ? 2 : 1
                    border.color: count.visualFocus ? theme.foreground : count.chosen ? theme.accent : theme.colors.selection
                }
                contentItem: Label {
                    text: count.name
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    color: count.chosen ? theme.accentForeground : theme.foreground
                }
            }
        }
    }
    // What shows, and why when it isn't what was chosen.
    Label {
        objectName: controls.prefix + "PagesName"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
        text: {
            translator.language
            const shown = reader.format.columns
            if (controls.values.layout === "scroll")
                return qsTr("Scrolling: one column, whatever the width")
            if (!reader.open)
                return settings.pages === 0 ? qsTr("Automatic: as many pages as the window has room for, up to three")
                                            : qsTr("%Ln pages side by side where they fit", "", settings.pages)
            if (settings.pages === 0)
                return qsTr("Automatic: %Ln pages here", "", reader.format.automaticColumns)
            if (!reader.format.pagesHonoured)
                return qsTr("Only %Ln pages fit at this width with lines of at least %1 characters", "", shown)
                    .arg(reader.format.minimumMeasure.toLocaleString(Qt.locale(), 'f', 0))
            return qsTr("%Ln pages side by side", "", shown)
        }
    }
    Stepper { key: "line_height"; label: qsTr("Line spacing") }
    Stepper { key: "paragraph_spacing"; label: qsTr("Between paragraphs") }
    Stepper { key: "indent"; label: qsTr("First-line indent") }
    Stepper { key: "margins"; label: qsTr("Margins") }

    Label {
        Layout.topMargin: 4
        text: qsTr("Alignment")
        color: controls.own.align !== undefined ? theme.accent : theme.colors.muted
        font.pixelSize: 12
    }
    // The four ways a paragraph's lines go, as one row. Start and end are
    // the sides the text starts and ends at, which turn round in a
    // right-to-left language; so do their pictures.
    RowLayout {
        objectName: controls.prefix + "Align"
        Layout.fillWidth: true
        spacing: 4
        Repeater {
            model: [
                { key: "start", name: qsTr("Start"), glyph: win.rightToLeft ? "alignRight" : "alignLeft" },
                { key: "justify", name: qsTr("Justified"), glyph: "alignJustify" },
                { key: "center", name: qsTr("Centred"), glyph: "alignCenter" },
                { key: "end", name: qsTr("End"), glyph: win.rightToLeft ? "alignLeft" : "alignRight" }
            ]
            delegate: AbstractButton {
                id: choice
                required property var modelData
                readonly property bool chosen: controls.values.align === modelData.key
                objectName: controls.prefix + "Align_" + modelData.key
                Layout.fillWidth: true
                implicitWidth: 44
                implicitHeight: 36
                focusPolicy: Qt.StrongFocus
                checkable: true
                checked: chosen
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.name
                Accessible.checked: chosen
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: modelData.name
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
                onClicked: {
                    checked = Qt.binding(() => choice.chosen)
                    controls.change("align", modelData.key)
                }
                background: Rectangle {
                    radius: theme.radius
                    color: choice.chosen ? theme.accent : choice.down || choice.hovered ? theme.colors.selection : "transparent"
                    border.width: choice.visualFocus ? 2 : 1
                    border.color: choice.visualFocus ? theme.foreground : choice.chosen ? theme.accent : theme.colors.selection
                }
                contentItem: Item {
                    Glyph {
                        anchors.centerIn: parent
                        name: choice.modelData.glyph
                        size: 18
                        color: choice.chosen ? theme.accentForeground : theme.foreground
                    }
                }
            }
        }
    }
    Label {
        objectName: controls.prefix + "AlignName"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: theme.colors.muted
        font.pixelSize: 12
        text: controls.values.align === "justify" ? qsTr("Justified: lines filled out to both sides")
              : controls.values.align === "center" ? qsTr("Centred: each line in the middle")
              : controls.values.align === "end" ? qsTr("End: lines against the side the text ends at")
              : qsTr("Start: lines against the side the text starts at")
    }
    }
}
