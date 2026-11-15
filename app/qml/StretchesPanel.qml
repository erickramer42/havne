// File/layer: app/qml/StretchesPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

pragma ComponentBehavior: Bound

// The viewer's Stretches panel (ADR-0056 section 6): the levels that make a
// stretch a pull or a cruise, with core's defaults shown in each field.
// Apply asks again with the typed levels; a field left empty, or typed as 0,
// takes the default. The page owns the ask, the stored levels and the words
// of a refusal; this panel reads them and says what was typed.
ColumnLayout {
    id: panel
    spacing: Theme.spacing

    // The opened recording (its stretchDefaults follow the unit system)
    required property RecordingView view
    // The levels in force, by key, in display units
    property var rules: ({})
    // The words of the last refusal or failed read, from the page
    property string problem: ""
    signal applyRequested(var rules)
    signal closeRequested()

    // What a field says it is: its key, name and unit ("speed" for the unit
    // system's speed unit, which a cell reads itself so a change of units
    // never remakes the cells and empties their fields)
    readonly property string speedUnit: Bridge.unitSystem === 1 ? "mph" : "km/h"
    readonly property var fields: [
        { key: "pullLevel", name: qsTr("Pull throttle"), unit: "%" },
        { key: "pullLoadLevel", name: qsTr("Pull load, with no throttle"), unit: "%" },
        { key: "pullHoldS", name: qsTr("Pull at least"), unit: "s" },
        { key: "pullGapS", name: qsTr("A dip shorter than"), unit: "s" },
        { key: "pullRise", name: qsTr("Engine speed rises"), unit: "rpm" },
        { key: "pullGain", name: qsTr("Speed gains, no rpm"), unit: "speed" },
        { key: "cruiseHoldS", name: qsTr("Cruise at least"), unit: "s" },
        { key: "cruiseSpeedBand", name: qsTr("Speed within"), unit: "speed" },
        { key: "cruiseThrottleBand", name: qsTr("Throttle within"), unit: "%" },
        { key: "cruiseFloor", name: qsTr("Cruise above"), unit: "speed" }]

    // What was typed that isn't a number, in words; "" for none
    property string typedProblem: ""

    // The locale levels are written and read in (a test sets another)
    property var numberLocale: Qt.locale()
    // A level as written in a field and a placeholder: up to three
    // decimals, the locale's decimal point and no group separator, so
    // numberOf reads back what it wrote
    function shown(value) {
        return value === undefined ? ""
            : String(Number(Number(value).toFixed(3))).replace(".", numberLocale.decimalPoint)
    }
    // A number as typed, in the locale or plain; NaN for one that isn't
    function numberOf(text) {
        const t = text.trim()
        try {
            return Number.fromLocaleString(numberLocale, t)
        } catch (e) {
            return Number(t)
        }
    }
    // The cell of the nth field, typed for what the Repeater hands back
    function cellAt(i) {
        return fieldRepeater.itemAt(i) as RuleCell
    }
    // The levels typed: a key each for the fields that hold a number; null
    // with the words set when one holds something else
    function typedRules() {
        const out = {}
        typedProblem = ""
        for (let i = 0; i < fieldRepeater.count; ++i) {
            const item = cellAt(i)
            const text = item.field.text.trim()
            if (text === "")
                continue
            const n = numberOf(text)
            if (!isFinite(n)) {
                typedProblem = qsTr("%1 must be a number").arg(item.name)
                return null
            }
            out[item.key] = n
        }
        return out
    }
    function apply() {
        const typed = typedRules()
        if (typed !== null)
            applyRequested(typed)
    }
    // Every field back to its default, asked for at once
    function reset() {
        for (let i = 0; i < fieldRepeater.count; ++i)
            cellAt(i).field.text = ""
        typedProblem = ""
        applyRequested({})
    }
    // The fields hold what is in force, each as it was typed
    function fill() {
        for (let i = 0; i < fieldRepeater.count; ++i) {
            const item = cellAt(i)
            const v = rules[item.key]
            item.field.text = shown(v)
        }
        typedProblem = ""
    }
    onRulesChanged: fill()

    // One level's caption and field
    component RuleCell: ColumnLayout {
        id: cell
        required property var modelData
        readonly property string key: modelData.key
        readonly property string name: modelData.name
        property alias field: input
        implicitWidth: Theme.compact ? Math.floor((grid.width - Theme.spacing) / 2)
                                     : Math.min(grid.width, 200)
        spacing: 2
        Label {
            Layout.fillWidth: true
            text: cell.modelData.name + " (" + (cell.modelData.unit === "speed" ? panel.speedUnit : cell.modelData.unit) + ")"
            color: Theme.textDim
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }
        ThemedTextField {
            id: input
            objectName: "rule_" + cell.key
            Layout.fillWidth: true
            placeholderText: panel.shown(panel.view.stretchDefaults[cell.key])
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            Accessible.name: cell.modelData.name
            onAccepted: panel.apply()
        }
    }

    RowLayout {
        spacing: Theme.spacing
        Layout.fillWidth: true
        ThemedButton {
            objectName: "stretchesApply"
            highlighted: true
            compact: Theme.compact
            Layout.fillWidth: Theme.compact
            Layout.preferredWidth: Theme.compact ? 1 : -1
            text: qsTr("Apply")
            onClicked: panel.apply()
        }
        ThemedButton {
            objectName: "stretchesReset"
            compact: Theme.compact
            Layout.fillWidth: Theme.compact
            Layout.preferredWidth: Theme.compact ? 1 : -1
            text: qsTr("Defaults")
            Accessible.name: qsTr("Back to the default levels")
            onClicked: panel.reset()
        }
        ThemedButton {
            objectName: "stretchesClose"
            compact: Theme.compact
            flat: true
            Layout.fillWidth: Theme.compact
            Layout.preferredWidth: Theme.compact ? 1 : -1
            text: qsTr("Close")
            onClicked: panel.closeRequested()
        }
        Item { Layout.fillWidth: !Theme.compact }
        ThemedCheckBox {
            objectName: "panelBands"
            // The Jump row holds the switch on the desktop; a phone's row
            // has no room for it
            visible: Theme.compact
            text: qsTr("Bands")
            checked: panel.bandsShown
            onToggled: panel.bandsToggled()
        }
    }
    property bool bandsShown: true
    signal bandsToggled()

    Label {
        objectName: "stretchesProblem"
        visible: text !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.danger
        text: panel.typedProblem !== "" ? panel.typedProblem : panel.problem
    }

    Flickable {
        id: flick
        Layout.fillWidth: true
        // The desktop's fields show whole; a phone's fill what the page has
        // left and scroll
        Layout.preferredHeight: Theme.compact ? -1 : Math.min(grid.implicitHeight, 280)
        Layout.minimumHeight: Theme.compact ? 96 : 0
        Layout.fillHeight: Theme.compact
        contentWidth: width
        contentHeight: grid.implicitHeight
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: flick.interactive ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded }

        Flow {
            id: grid
            width: flick.width - (flick.interactive ? 12 : 0)
            spacing: Theme.spacing
            Repeater {
                id: fieldRepeater
                model: panel.fields
                delegate: RuleCell {}
            }
        }
    }

    Label {
        objectName: "stretchesHint"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: qsTr("An empty field, or 0, takes the default shown in it. Pulls and cruises are found again when you apply.")
    }

    Component.onCompleted: fill()
}
