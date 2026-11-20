// File/layer: app/qml/SettingsOwnComputed.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.settings, QmlTest.phone (test_qml)
// The form's computed-signal part (ADR-0062's appendix of 2026-11-16): the
// expression, checked as it is typed by core, the signals it reads, typed
// test values and the value they give, and a list that inserts a signal's
// name. Core judges everything (ExprEditor); this part shows its words.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    required property SettingsPage host
    property alias formExpr: formExprField
    // What the editor last said of the expression, and the file it checks against
    property var checked: ({ ok: false, at: 0, words: "", leaves: [] })
    property var leafRefs: []
    property var testTexts: ({})
    property var preview: ({ text: "", warn: false })
    property bool insertOpen: false
    property string liveNote: ""
    property var nameRows: []
    property alias editor: exprEditorItem
    readonly property bool shown: root.host.ownPart.formKind === "computed"
    // The vehicle's own file as the editor reads it: obd2's signals named too
    readonly property var fileDoc: {
        const f = root.host.ownFile !== null ? root.host.ownFile
            : SignalForm.emptyFile(root.host.pickedVehicle !== null ? root.host.pickedVehicle.label : "")
        // A file with no signal is refused by the loader: the editor's copy of
        // a vehicle with none yet holds one that is never offered or saved
        return SignalForm.withExtends(f.signals.length > 0 ? f : SignalForm.withSignal(
            f, { key: "placeholder_", name: "Placeholder",
                 read: { service: "22", number: "F000" }, field: { byte: 0 } },
            { rx: "7E8" }, ""))
    }
    readonly property string problem: !root.shown || formExprField.text.trim() === "" || root.checked.ok
        ? "" : String(root.checked.words)
    // The name typed is one of obd2's signals (not one of the vehicle's own): core
    // replaces that signal by name and keeps its ID, so what reads it reads this one
    readonly property string replaced: {
        const name = root.host.ownFormPart.formName.text.trim()
        if (!root.shown || name === "" || root.host.ownSignals.some(s => s.name === name))
            return ""
        return root.nameRows.some(r => r.name === name) ? name : ""
    }
    // Where these values move on the demo car: an obd2 input, or a file that extends obd2
    readonly property bool demoHint: {
        const file = root.host.ownFile
        const extendsObd2 = file !== null && file.extends === "obd2"
        return extendsObd2 || root.readsObd2
    }
    readonly property bool readsObd2: root.leafRefs.some(r => r.startsWith("obd2."))

    objectName: "formComputed"
    visible: root.shown
    Layout.fillWidth: true
    spacing: Theme.spacing

    ExprEditor {
        id: exprEditorItem
    }

    onFileDocChanged: loadFile()
    onShownChanged: {
        if (root.shown)
            recheck()
    }
    Component.onCompleted: loadFile()

    Connections {
        target: root.host.ownPart
        function onEditingKeyChanged() {
            root.recheck()
        }
    }

    Connections {
        target: root.host.ownFormPart.formUnit
        function onTextChanged() {
            root.recompute()
        }
    }
    Connections {
        target: root.host.ownFormPart.formDecimals
        function onTextChanged() {
            root.recompute()
        }
    }

    function loadFile() {
        exprEditorItem.setFile(JSON.stringify(root.fileDoc))
        // Plain JS copies: a delegate never reads a C++ map (qml.md)
        root.nameRows = JSON.parse(JSON.stringify(exprEditorItem.names())).filter(r => r.ref !== "placeholder_")
        root.recheck()
    }

    function recheck() {
        if (!root.shown)
            return
        const r = exprEditorItem.check(root.host.ownPart.editingKey, formExprField.text)
        root.checked = JSON.parse(JSON.stringify(r))
        const refs = r.ok ? r.leaves.map(l => l.ref) : []
        if (JSON.stringify(refs) !== JSON.stringify(root.leafRefs))
            root.leafRefs = refs
        root.recompute()
    }

    // The test values typed so far, as the editor takes them: a number is
    // a value, anything else is none
    function valuesOf() {
        const out = []
        for (const l of root.checked.leaves) {
            const t = String(root.testTexts[l.ref] === undefined ? "" : root.testTexts[l.ref]).trim()
            const n = Number(t)
            if (t !== "" && isFinite(n))
                out.push({ ref: l.ref, value: n, ageMs: 0 })
        }
        return out
    }

    function notNumber() {
        for (const l of root.checked.leaves) {
            const t = String(root.testTexts[l.ref] === undefined ? "" : root.testTexts[l.ref]).trim()
            if (t !== "" && !isFinite(Number(t)))
                return l.ref
        }
        return ""
    }

    // An Instantiator's object and a Repeater's item are a QObject and an Item
    // to the linter, which doesn't see the delegate's own members
    function untyped(item) {
        return item
    }

    // Fills the test values from the session's rows of the same signals
    // (matched by id: the model's ref is its own key). The rows are read
    // once, as the button is pressed: an Instantiator left on would follow
    // every value's change
    function useLive() {
        // The model shows values in the chosen unit system; a test value is
        // in the decoded unit, which is the leaf's
        const leafOf = {}
        for (const l of root.checked.leaves)
            leafOf[l.id] = l
        liveRows.active = true
        const found = {}
        for (let i = 0; i < liveRows.count; ++i) {
            const r = root.untyped(liveRows.objectAt(i))
            const leaf = r !== null ? leafOf[r.id] : undefined
            if (leaf !== undefined && r.hasValue && !r.isText)
                found[leaf.ref] = Bridge.decodedValue(leaf.unit, r.value)
        }
        liveRows.active = false
        let filled = 0
        for (let i = 0; i < root.leafRefs.length; ++i) {
            const v = found[root.leafRefs[i]]
            if (v === undefined)
                continue
            root.untyped(testRepeater.itemAt(i)).field.text = String(Number(v.toPrecision(7)))
            ++filled
        }
        root.liveNote = qsTr("Filled %1 of %2 from the session").arg(filled).arg(root.leafRefs.length)
    }

    function shownNumber(v) {
        const d = Number(root.host.ownFormPart.formDecimals.text)
        return root.host.ownFormPart.formDecimals.text.trim() !== "" && isFinite(d) && d >= 0 && d <= 3
            ? v.toFixed(d) : String(Number(v.toPrecision(7)))
    }

    function recompute() {
        if (!root.checked.ok) {
            root.preview = { text: "", warn: false }
            return
        }
        const bad = root.notNumber()
        if (bad !== "") {
            root.preview = { text: qsTr("%1 is not a number").arg(bad), warn: true }
            return
        }
        const r = exprEditorItem.evaluate(root.valuesOf())
        if (!r.ok) {
            root.preview = { text: String(r.words), warn: true }
        } else if (r.hasValue) {
            const unit = root.host.ownFormPart.formUnit.text
            root.preview = { text: qsTr("= %1%2").arg(root.shownNumber(r.value),
                                                   unit !== "" ? " " + unit : ""), warn: false }
        } else if (r.why === "missing") {
            root.preview = { text: qsTr("No value yet: give a value for %1").arg(r.leafName), warn: false }
        } else if (r.why === "stale") {
            root.preview = { text: qsTr("No value: %1 is older than its %2 s bound")
                                       .arg(r.leafName, String(r.boundMs / 1000)), warn: false }
        } else {
            root.preview = { text: qsTr("No value: the steps gave no number (a division by zero, "
                                        + "a root of a negative, or a result too large)"), warn: false }
        }
    }

    // Puts text at the cursor, a space on a side where a name would run
    // into its neighbor; the field keeps the focus
    function insertText(text, padded) {
        const at = formExprField.cursorPosition
        const before = formExprField.text.charAt(at - 1)
        const after = formExprField.text.charAt(at)
        const left = padded && before !== "" && before !== " " && before !== "(" ? " " : ""
        const right = padded && after !== "" && after !== " " && after !== ")" ? " " : ""
        formExprField.insert(at, left + text + right)
        formExprField.forceActiveFocus()
        root.insertOpen = false
    }

    // A computed signal into the own variant: checked once more as typed,
    // the file's format raised when the expression needs it and obd2's
    // signals read with it when an input is one, each said in the note
    function save(f) {
        const signal = SignalForm.draftComputed(f)
        const named = root.host.ownSignals.find(s => s.name === signal.name && s.key !== root.host.ownPart.editingKey)
        if (root.host.ownPart.editingKey !== "" && named !== undefined) {
            root.host.ownPart.formProblem = qsTr("%1 is saved already: give this one another name").arg(named.name)
            return false
        }
        signal.key = SignalForm.keyFor(root.host.ownSignals, signal.name, root.host.ownPart.editingKey)
        if (root.host.ownPart.editingKey === "" && SignalForm.startsWithDigit(signal.key)) {
            root.host.ownPart.formProblem = qsTr("A computed signal's name can't start with a digit: an expression "
                               + "would read it as a number. Start the name with a letter.")
            return false
        }
        const c = exprEditorItem.check(root.host.ownSignals.some(s => s.key === signal.key) ? signal.key : "", signal.expr)
        if (!c.ok) {
            const w = String(c.words)
            root.host.ownPart.formProblem = w.charAt(0).toUpperCase() + w.slice(1)
            return false
        }
        const base = root.host.ownFile !== null ? root.host.ownFile
            : SignalForm.emptyFile(root.host.pickedVehicle.label, root.host.ownPart.ownName(root.host.pickedVehicle.label))
        let doc = SignalForm.withComputed(base, signal, root.host.ownPart.editingKey)
        let note = qsTr("%1 saved in %2").arg(signal.name, base.name)
        if (c.format > (base.havne_signals === undefined ? 1 : base.havne_signals)) {
            doc = SignalForm.withFormat(doc, c.format)
            note += qsTr(". This file now needs Havne 0.8.0 or newer: '%1' uses a function, "
                         + "a comparison or a computed signal.").arg(signal.name)
        }
        if (c.leaves.some(l => String(l.ref).startsWith("obd2.")) && doc.extends === undefined) {
            doc = SignalForm.withExtends(doc)
            note += qsTr(". Your signals now read with obd2's: the profile row shows obd2 + %1, "
                         + "and the car is asked obd2's PIDs too.").arg(base.name)
        }
        if (!root.host.ownPart.saveOwn(doc, note))
            return false
        root.host.ownPart.editingKey = ""
        return true
    }

    function closeInsert() {
        root.insertOpen = false
        formExprField.forceActiveFocus()
    }

    onInsertOpenChanged: {
        if (root.insertOpen)
            insertFilter.forceActiveFocus()
    }

    function shownRows() {
        const words = insertFilter.text.toLowerCase().split(/\s+/).filter(w => w !== "")
        return root.nameRows.filter(r => (root.host.ownPart.editingKey === "" || r.ref !== root.host.ownPart.editingKey)
            && words.every(w => (r.name + " " + r.ref + " " + r.unit).toLowerCase().indexOf(w) >= 0))
    }

    Captioned {
        caption: qsTr("Expression")
        Layout.fillWidth: true
        ThemedTextField {
            id: formExprField
            objectName: "formExpr"
            placeholderText: qsTr("For example: obd2.map - obd2.baro")
            width: parent.width
            onTextChanged: root.recheck()

            // The refused character, marked under the field
            Rectangle {
                objectName: "formExprMark"
                // A rect is in the field's own coordinates, which move as the text scrolls:
                // the cursor and the text's width tell when
                readonly property real scrolled: formExprField.cursorRectangle.x + formExprField.contentWidth
                readonly property rect at: formExprField.positionToRectangle(Math.max(root.checked.at - 1, 0) + 0 * scrolled)
                readonly property rect next: formExprField.positionToRectangle(root.checked.at + 0 * scrolled)
                visible: root.problem !== "" && root.checked.at > 0
                x: at.x
                y: formExprField.height - Theme.radius
                width: Math.max(next.x - at.x, Theme.spacing)
                height: Theme.radius / 2
                color: Theme.danger
            }
        }
    }

    Label {
        objectName: "formExprProblem"
        visible: text !== ""
        text: root.problem === "" ? "" : root.problem.charAt(0).toUpperCase() + root.problem.slice(1)
        color: Theme.warning
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Accessible.description: text
    }

    Label {
        objectName: "formReplaces"
        visible: root.replaced !== ""
        text: qsTr("Replaces obd2's %1: a gauge or layout that shows it will show this one.").arg(root.replaced)
        color: Theme.warning
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Accessible.description: text
    }

    Flow {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: Theme.controlHeight
        spacing: Theme.spacing
        ThemedButton {
            objectName: "formInsert"
            text: qsTr("Insert signal")
            checkable: true
            checked: root.insertOpen
            onClicked: root.insertOpen = !root.insertOpen
        }
        Repeater {
            model: ["min", "max", "abs", "clamp", "round", "if"]
            ThemedButton {
                required property string modelData
                objectName: "formChip_" + modelData
                text: modelData
                flat: true
                compact: true
                onClicked: root.insertText(modelData + "(", false)
            }
        }
    }

    ColumnLayout {
        objectName: "formInsertPanel"
        visible: root.insertOpen
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedTextField {
            id: insertFilter
            objectName: "formInsertFilter"
            placeholderText: qsTr("Find a signal")
            Layout.fillWidth: true
            Keys.onEscapePressed: root.closeInsert()
        }
        ListView {
            id: insertList
            objectName: "formInsertList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Theme.touchTarget * 5)
            clip: true
            currentIndex: -1
            highlightFollowsCurrentItem: false
            model: root.insertOpen ? root.shownRows() : []
            delegate: AbstractButton {
                id: pick
                required property var modelData
                required property int index
                objectName: "formInsertRow_" + index
                width: ListView.view.width
                height: Math.max(Theme.touchTarget, rowText.implicitHeight + 8)
                enabled: pick.modelData.usable
                Accessible.name: rowText.text
                Keys.onEscapePressed: root.closeInsert()
                onClicked: root.insertText(pick.modelData.ref, true)
                contentItem: Label {
                    id: rowText
                    text: pick.modelData.usable
                        ? qsTr("%1 (%2)%3").arg(pick.modelData.name, pick.modelData.ref,
                                                pick.modelData.unit !== "" ? ": " + pick.modelData.unit : "")
                        : qsTr("%1: %2").arg(pick.modelData.name, pick.modelData.why)
                    color: pick.modelData.usable ? Theme.text : Theme.textDim
                    wrapMode: Text.Wrap
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: pick.down ? Theme.elevated : "transparent"
                    border.width: 0
                }
            }
        }
        Label {
            objectName: "formInsertNone"
            visible: insertList.count === 0
            text: qsTr("No signal to read")
            color: Theme.textDim
        }
    }

    Repeater {
        model: root.checked.ok ? root.checked.leaves : []
        Label {
            required property var modelData
            required property int index
            objectName: "formInput_" + index
            readonly property string display: Bridge.displayUnit(modelData.unit)
            text: modelData.unit === "" ? qsTr("Reads %1 (%2)").arg(modelData.name, modelData.ref)
                : display !== modelData.unit
                    ? qsTr("Reads %1 (%2): %3, shown as %4").arg(modelData.name, modelData.ref,
                                                            modelData.unit, display)
                    : qsTr("Reads %1 (%2): %3").arg(modelData.name, modelData.ref, modelData.unit)
            color: Theme.textDim
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }

    Label {
        objectName: "formUnitsNote"
        visible: root.leafRefs.length > 0
        text: qsTr("Values below are in the units signals are decoded in, as a gauge's levels are.")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Instantiator {
        id: liveRows
        active: false
        model: Bridge.signalModel
        delegate: QtObject {
            required property int id
            required property double value
            required property bool hasValue
            required property bool isText
        }
    }

    ThemedButton {
        objectName: "formUseLive"
        visible: Bridge.connected && root.leafRefs.length > 0
        text: qsTr("Use the session's values")
        onClicked: root.useLive()
    }

    Label {
        objectName: "formUseLiveNote"
        visible: text !== "" && root.leafRefs.length > 0
        text: root.liveNote
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Repeater {
        id: testRepeater
        model: root.leafRefs
        RowLayout {
            id: testRow
            required property string modelData
            required property int index
            property alias field: testField
            Layout.fillWidth: true
            spacing: Theme.spacing
            Label {
                text: testRow.modelData
                color: Theme.text
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 0
            }
            ThemedTextField {
                id: testField
                objectName: "formTest_" + testRow.index
                placeholderText: qsTr("Value")
                Layout.preferredWidth: Theme.controlHeight * 3
                Component.onCompleted: {
                    const t = root.testTexts[testRow.modelData]
                    text = t === undefined ? "" : t
                }
                onTextChanged: {
                    root.testTexts[testRow.modelData] = text
                    root.recompute()
                }
            }
        }
    }

    Label {
        objectName: "formPreview"
        visible: text !== ""
        text: root.preview.text
        color: root.preview.warn ? Theme.warning : Theme.text
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Accessible.description: text
    }

    Label {
        objectName: "formDemoHint"
        visible: root.demoHint
        text: qsTr("Or connect the demo: mock://demo with %1's signals, where these values move.")
                  .arg(root.host.pickedVehicle !== null ? root.host.pickedVehicle.label : "")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
}
