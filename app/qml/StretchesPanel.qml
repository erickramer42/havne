// File/layer: app/qml/StretchesPanel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

pragma ComponentBehavior: Bound

// The viewer's Stretches panel (ADR-0056 section 6): the levels that make a
// stretch a pull or a cruise, with core's defaults shown in each field, and
// where a pull ends. Apply asks again with the typed levels; a field left
// empty, or typed as 0, takes the default; a pull's end asks again at its
// tap. The page owns the ask, the stored levels and the words of a refusal;
// this panel reads them and says what was typed.
ColumnLayout {
    id: panel
    spacing: Theme.spacing

    // The opened recording (its stretchDefaults follow the unit system)
    required property RecordingView view
    // view.stretchDefaults as a plain object, copied again on its NOTIFY, so a
    // cell's read makes no new JS wrapper of the view's map
    readonly property var defaults: Object.assign({}, view.stretchDefaults)
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
    // The names come from the view's table, the words a refusal uses
    readonly property var fields: [
        { key: "pullLevel", unit: "%" },
        { key: "pullLoadLevel", unit: "%" },
        { key: "pullHoldS", unit: "s" },
        { key: "pullGapS", unit: "s" },
        { key: "pullRise", unit: "rpm" },
        { key: "pullGain", unit: "speed" },
        { key: "cruiseHoldS", unit: "s" },
        { key: "cruiseSpeedBand", unit: "speed" },
        { key: "cruiseThrottleBand", unit: "%" },
        { key: "cruiseFloor", unit: "speed" }].map(f => ({ key: f.key, unit: f.unit,
                                                          name: view.stretchRuleNames[f.key] }))

    // Where a pull ends: "lift" (the default) or "top-speed"
    property string pullEnd: "lift"

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
        if (pullEnd === "top-speed")
            out.pullEnd = pullEnd
        return out
    }
    // A pull's end chosen: asked for at once, with the levels typed. It
    // takes hold only once the fields are judged, so a bad number leaves the
    // end that is in force selected
    function choosePullEnd(word) {
        const before = pullEnd
        pullEnd = word
        const typed = typedRules()
        if (typed === null)
            pullEnd = before
        else
            applyRequested(typed)
    }
    // A refusal by core: the end shown goes back to the one in force
    function keepEnd() {
        pullEnd = rules.pullEnd === "top-speed" ? "top-speed" : "lift"
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
        pullEnd = "lift"
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
        pullEnd = rules.pullEnd === "top-speed" ? "top-speed" : "lift"
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
            textFormat: Text.PlainText
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
            placeholderText: panel.shown(panel.defaults[cell.key])
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
        textFormat: Text.PlainText
        objectName: "stretchesProblem"
        visible: text !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.danger
        text: panel.typedProblem !== "" ? panel.typedProblem : panel.problem
    }

    // Where a pull ends: at the throttle's lift, the limiter's fuel cut
    // included, or at top speed, which leaves it out
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: qsTr("A pull ends")
            color: Theme.textDim
            font.pixelSize: Theme.fontCaption
            elide: Text.ElideRight
        }
        SegmentRow {
            objectName: "pullEnd"
            Layout.fillWidth: Theme.compact
            SegmentButton {
                objectName: "pullEnd_lift"
                tapHeight: Theme.touchTarget
                share: Theme.compact
                wrapMode: Theme.compact ? Text.Wrap : Text.NoWrap
                Layout.preferredHeight: Math.max(Theme.touchTarget, implicitContentHeight)
                labelSize: Theme.fontBody
                text: qsTr("At the throttle's lift")
                selected: panel.pullEnd === "lift"
                checked: selected
                onClicked: panel.choosePullEnd("lift")
            }
            SegmentButton {
                objectName: "pullEnd_topSpeed"
                tapHeight: Theme.touchTarget
                share: Theme.compact
                wrapMode: Theme.compact ? Text.Wrap : Text.NoWrap
                Layout.preferredHeight: Math.max(Theme.touchTarget, implicitContentHeight)
                labelSize: Theme.fontBody
                text: qsTr("At top speed")
                selected: panel.pullEnd === "top-speed"
                checked: selected
                onClicked: panel.choosePullEnd("top-speed")
            }
        }
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
        textFormat: Text.PlainText
        objectName: "stretchesHint"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
        text: qsTr("An empty field, or 0, takes the default shown in it. Pulls and cruises are found again when you apply.")
    }

    Component.onCompleted: fill()
}
