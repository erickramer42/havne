// File/layer: app/qml/SettingsAlerts.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.alerts, QmlTest.phone_* (test_qml)
// The app's one set of conditions (ADR-0063 sections 5 and 9): its list with each one's state, Add, Add from a signal, Import and Export; one part of SettingsPage

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    objectName: "settingsAlertsPart"
    required property SettingsPage host
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // The saved file's document (ConditionBook's), null while the saved
    // text isn't a condition file's JSON
    property var doc: ConditionBook.empty()
    // The last save's or import's refusal, in core's words
    property string problem: ""
    property string note: ""
    // "", "import" (Import replaces the saved alerts), "restart"
    property string confirming: ""
    property bool pickingSignal: false
    // What the editor opens on: the index edited (-1 a new one) and the
    // condition, a copy
    property int editIndex: -1
    property var draft: null

    // A saved file core refused reads as none: an edit of it would be
    // refused at each save. Start again or Import replaces it, Export
    // writes it as it is
    readonly property bool unreadable: root.doc === null || Bridge.conditionsProblem !== ""
    readonly property var conditions: root.unreadable ? [] : ConditionBook.conditions(root.doc)

    // Bridge.gaugeSignals as plain arrays, made once (it is CONSTANT):
    // each read of a bridge map makes a new JS wrapper
    readonly property var gaugeSets: DashboardEdit.copySets(Bridge.gaugeSignals)

    // The rows a term can bind: numbers and named states with references,
    // copied once into plain objects (a bridge map read per delegate
    // makes a new wrapper at each read)
    property var rows: []
    function plainRow(r) {
        const o = {}
        for (const k of Object.keys(r)) {
            const v = r[k]
            o[k] = v !== null && typeof v === "object" ? JSON.parse(JSON.stringify(v)) : v
        }
        return o
    }
    function refreshRows() {
        rows = Array.from(Bridge.profileSignals(Bridge.connected ? Bridge.currentUri : root.host.uriPart.offlineUri),
                          r => root.plainRow(r))
                   .filter(r => r.ref !== "" && !r.isText)
    }
    // Rows with a gauge's levels, for Add from a signal
    readonly property var levelRows: root.rows.filter(r => root.levelsOf(r) !== null)

    function reload() {
        doc = ConditionBook.parse(Bridge.conditionsText())
    }
    Component.onCompleted: {
        reload()
        refreshRows()
    }
    Connections {
        target: Bridge
        function onConditionsChanged() { root.reload() }
        function onConnectedChanged() { root.refreshRows() }
    }
    onVisibleChanged: if (visible) refreshRows()

    // A save through core's loader: true when kept. The words drop the
    // line and column, which name the text this page made
    function saveDoc(next) {
        const r = Bridge.saveConditions(ConditionBook.text(next))
        if (!r.ok) {
            problem = String(r.words).replace(/^line \d+, column \d+: /, "")
            return false
        }
        problem = ""
        note = r.pending !== "" ? qsTr("Saved: the change %1.").arg(r.pending) : ""
        return true
    }
    function setEnabled(i, on) {
        saveDoc(ConditionBook.withEnabled(doc, i, on))
    }

    // The editor's way in
    signal editRequested()
    function openEditor(i, c) {
        editIndex = i
        draft = ConditionBook.copy(c)
        problem = ""
        note = ""
        editRequested()
    }
    // The first "Alert N" no saved condition is named
    function addNew() {
        const taken = ConditionBook.namesOf(doc)
        let n = 1
        while (taken.indexOf(qsTr("Alert %1").arg(n)) >= 0)
            ++n
        const name = qsTr("Alert %1").arg(n)
        openEditor(-1, { key: ConditionBook.newKey(name, ConditionBook.keysOf(doc)), name: name,
                         when: { signal: [], unit: "", above: 0 },
                         alert: { level: "warning", banner: true } })
    }
    // A copy from a gauge or a signal's levels, never bound to it, named
    // and keyed anew among the saved conditions: "" when the editor opens,
    // else why not
    function openDraft(c) {
        if (unreadable)
            return qsTr("The saved alerts don't load: Settings > Alerts says why.")
        if (!ConditionBook.canAdd(doc))
            return qsTr("Settings > Alerts holds %1 alerts, the most a file takes.").arg(ConditionBook.maxConditions)
        const draft = ConditionBook.copy(c)
        draft.name = ConditionBook.newName(draft.name ?? "", ConditionBook.namesOf(doc))
        draft.key = ConditionBook.newKey(draft.name, ConditionBook.keysOf(doc))
        openEditor(-1, draft)
        return ""
    }

    // Levels: a signal file's gauge, else the zone table's
    function levelsOf(row) {
        const g = ConditionBook.gaugeLevels(row.gauge)
        const levels = Object.keys(g).length > 0
                       ? g : DashboardBook.zonesFor(DashboardEdit.keyOfRef(row.ref, gaugeSets))
        delete levels.coldBelow
        return Object.keys(levels).length > 0 ? levels : null
    }
    function rangeOf(row) {
        const g = row.gauge ?? ({})
        if (typeof g.min === "number" && typeof g.max === "number")
            return { from: g.min, to: g.max }
        return DashboardEdit.defaultRange(row.decodedUnit, row.decodedLow, row.decodedHigh)
    }
    function addFromSignal(row) {
        const levels = levelsOf(row)
        if (levels === null)
            return
        const range = rangeOf(row)
        const c = ConditionBook.alertFromLevels(row.name, DashboardEdit.counterparts(row.ref, gaugeSets),
                                                row.decodedUnit, levels, range.from, range.to,
                                                ConditionBook.keysOf(doc))
        pickingSignal = false
        openDraft(c)
    }

    // Words
    // A number as shown: up to three decimals, the locale's point. The
    // editor's fields keep three, so a saved level reads back as it was; the
    // list says a level in its signal's own decimals or as many as the level
    // needs (up to three), whichever is more
    function shownNumber(v, decimals) {
        if (typeof v !== "number" || !isFinite(v))
            return ""
        let d = decimals ?? 3
        while (d < 3 && Math.abs(v - Number(v.toFixed(d))) > 1e-9)
            ++d
        return String(Number(v.toFixed(d))).replace(".", Qt.locale().decimalPoint)
    }
    function rowOfRefs(refs) {
        const all = DashboardEdit.lookupRefs(refs ?? [], gaugeSets)
        return rows.find(r => all.indexOf(r.ref) >= 0) ?? null
    }
    function nameOfRefs(refs) {
        const row = rowOfRefs(refs)
        return row !== null ? row.name
                            : (refs ?? []).length > 0 ? qsTr("a signal not in this profile") : qsTr("no signal")
    }
    // A signal the rows don't list, by its reference, for a screen
    // reader's description; "" for one they list
    function unlistedRef(refs) {
        return (refs ?? []).length > 0 && rowOfRefs(refs) === null ? String(refs[0]) : ""
    }
    function unlistedRefsOf(c) {
        return ConditionBook.termsOf(c).filter(t => ConditionBook.kindOf(t) === "signal")
            .map(t => unlistedRef(t.signal)).filter(r => r !== "").join(", ")
    }
    readonly property var linkNames: ({
        not_ok: qsTr("the link isn't OK"), timeout: qsTr("the ECU isn't responding"),
        request_failed: qsTr("requests are failing"), hard_failure: qsTr("the adapter is lost") })
    // One term as the list says it, its level in the display unit
    function termWords(t) {
        const kind = ConditionBook.kindOf(t)
        if (kind === "link")
            return linkNames[t.link] ?? String(t.link)
        if (kind === "check")
            return qsTr("the frames check finds a %1").arg(String(t.check).replace("_", " "))
        const comparison = ConditionBook.comparisonOf(t)
        const level = t[comparison]
        const name = nameOfRefs(t.signal)
        if (comparison === "equals" || comparison === "differs")
            return (comparison === "equals" ? qsTr("%1 is %2") : qsTr("%1 isn't %2"))
                       .arg(name).arg(shownNumber(level))
        // Reading the unit system makes a binding follow its change
        const units = Bridge.unitSystem
        const row = rowOfRefs(t.signal)
        const shown = shownNumber(Bridge.displayValue(t.unit ?? "", level),
                                  row !== null && typeof row.decimals === "number" ? row.decimals : 3)
        const unit = units >= 0 ? Bridge.displayUnit(t.unit ?? "") : ""
        return (comparison === "below" ? qsTr("%1 below %2 %3") : qsTr("%1 above %2 %3"))
                   .arg(name).arg(shown).arg(unit).trim()
    }
    function summaryOf(c) {
        const terms = ConditionBook.termsOf(c).map(t => termWords(t))
        const join = ConditionBook.joinOf(c) === "any" ? qsTr(" or ") : qsTr(" and ")
        return terms.join(join)
    }
    // The state's word: off, not available, stale, ON, or nothing while
    // it watches or no session judges it
    function stateWords(c, s) {
        if (c.enabled === false)
            return qsTr("off")
        if (s === undefined || s === null || !s.live)
            return ""
        if (s.unavailable)
            return qsTr("not available: %1").arg(s.why)
        if (s.stale)
            return qsTr("stale")
        return s.state === "on" ? qsTr("ON") : ""
    }

    // Each row's state from the model, by its key: the saved file and the
    // set a session judges can differ while an edit waits
    property var stateOf: ({})
    component StateRow: QtObject {
        required property string key
        required property bool live
        required property string state
        required property bool stale
        required property bool unavailable
        required property string why
    }
    Instantiator {
        model: Bridge.conditionModel
        delegate: StateRow {}
        onObjectAdded: (index, object) => {
            const row = object as StateRow
            const m = Object.assign({}, root.stateOf)
            m[row.key] = row
            root.stateOf = m
        }
        onObjectRemoved: (index, object) => {
            const row = object as StateRow
            const m = Object.assign({}, root.stateOf)
            if (m[row.key] === row)
                delete m[row.key]
            root.stateOf = m
        }
    }

    // The page
    Label {
        textFormat: Text.PlainText
        objectName: "alertsNote"
        text: qsTr("An alert watches the session's signals and says when one passes its level. "
                   + "Havne ships none: a level that suits one engine is a false alarm on another.")
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Label {
        textFormat: Text.PlainText
        objectName: "alertsSavedProblem"
        visible: Bridge.conditionsProblem !== ""
        text: Bridge.conditionsProblem
        color: Theme.danger
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        textFormat: Text.PlainText
        objectName: "alertsPending"
        visible: Bridge.conditionsPending !== ""
        text: qsTr("Your last change %1.").arg(Bridge.conditionsPending)
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        ThemedButton {
            objectName: "alertAdd"
            width: Math.min(implicitWidth, root.width)
            text: qsTr("Add")
            enabled: !root.unreadable && ConditionBook.canAdd(root.doc) && root.confirming === ""
            onClicked: root.addNew()
        }
        ThemedButton {
            objectName: "alertAddFromSignal"
            width: Math.min(implicitWidth, root.width)
            text: qsTr("Add from a signal's levels")
            checkable: true
            checked: root.pickingSignal
            enabled: !root.unreadable && ConditionBook.canAdd(root.doc) && root.confirming === ""
            onClicked: {
                root.refreshRows()
                root.pickingSignal = !root.pickingSignal
            }
        }
        ThemedButton {
            objectName: "alertImport"
            width: Math.min(implicitWidth, root.width)
            text: qsTr("Import...")
            enabled: root.confirming === ""
            onClicked: {
                if (root.conditions.length > 0 || root.unreadable)
                    root.confirming = "import"
                else
                    importDialog.open()
            }
        }
        ThemedButton {
            objectName: "alertExport"
            width: Math.min(implicitWidth, root.width)
            text: qsTr("Export...")
            // A file core refused is exported as it is, to be mended elsewhere
            enabled: root.conditions.length > 0 || Bridge.conditionsProblem !== ""
            onClicked: exportDialog.open()
        }
        ThemedButton {
            objectName: "alertRestart"
            visible: root.unreadable
            width: Math.min(implicitWidth, root.width)
            text: qsTr("Start again")
            flat: true
            enabled: root.confirming === ""
            onClicked: root.confirming = "restart"
        }
    }

    // A question that waits for its answer
    ColumnLayout {
        objectName: "alertConfirm"
        visible: root.confirming !== ""
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            textFormat: Text.PlainText
            objectName: "alertConfirmText"
            text: root.confirming !== "import"
                  ? qsTr("Start again with no alerts? The saved file is replaced; one that couldn't be read is kept beside it.")
                  : root.unreadable ? qsTr("Import replaces the saved file, which doesn't load, with the file's. A file that couldn't be read is kept beside it.")
                  : root.conditions.length === 1 ? qsTr("Import replaces the 1 saved alert with the file's.")
                  : qsTr("Import replaces the %1 saved alerts with the file's.").arg(root.conditions.length)
            color: Theme.text
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "alertConfirmYes"
                width: Math.min(implicitWidth, root.width)
                text: root.confirming === "import" ? qsTr("Choose a file...") : qsTr("Start again")
                onClicked: {
                    const what = root.confirming
                    root.confirming = ""
                    if (what === "import")
                        importDialog.open()
                    else
                        root.saveDoc(ConditionBook.empty())
                }
            }
            ThemedButton {
                objectName: "alertConfirmNo"
                width: Math.min(implicitWidth, root.width)
                text: qsTr("Cancel")
                flat: true
                onClicked: root.confirming = ""
            }
        }
    }

    Label {
        textFormat: Text.PlainText
        objectName: "alertProblem"
        visible: text !== ""
        text: root.problem
        color: Theme.danger
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    Label {
        textFormat: Text.PlainText
        objectName: "alertSaveNote"
        visible: text !== ""
        text: root.note
        color: Theme.textDim
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    // Add from a signal's levels
    ColumnLayout {
        objectName: "alertSignalPanel"
        visible: root.pickingSignal
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            textFormat: Text.PlainText
            text: qsTr("A signal with a gauge's levels: the alert starts at its danger level, "
                       + "else its warning, and clears a little inside it.")
            color: Theme.textDim
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        AlertSignalPicker {
            objectName: "alertLevelPicker"
            room: root.width
            rows: root.levelRows
            emptyText: root.rows.length === 0
                       ? qsTr("No signals: connect, or pick a plugin for your vehicle under Settings > Signals.")
                       : qsTr("None of these signals has a gauge's levels: Add makes an alert to fill in.")
            onPicked: row => root.addFromSignal(row)
        }
    }

    // The list
    Label {
        textFormat: Text.PlainText
        objectName: "alertsNone"
        visible: root.conditions.length === 0 && !root.unreadable
        text: qsTr("No alerts yet.")
        color: Theme.textDim
        Layout.fillWidth: true
    }
    Repeater {
        objectName: "alertRows"
        model: root.conditions.length
        ThemedButton {
            id: alertRow
            required property int index
            readonly property var condition: root.conditions[index] ?? ({})
            readonly property var live: root.stateOf[condition.key] ?? null
            readonly property string stateText: root.stateWords(condition, live)
            readonly property bool on: stateText === qsTr("ON")
            objectName: "alertRow_" + index
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(Theme.touchTarget, rowContent.implicitHeight + 16)
            Accessible.name: stateText === "" ? qsTr("Edit %1").arg(condition.name ?? "")
                                              : qsTr("Edit %1, %2").arg(condition.name ?? "").arg(stateText)
            Accessible.description: root.unlistedRefsOf(condition)
            enabled: root.confirming === ""
            onClicked: root.openEditor(index, condition)
            contentItem: RowLayout {
                id: rowContent
                spacing: Theme.spacing
                ThemedCheckBox {
                    objectName: "alertEnabled_" + alertRow.index
                    checked: alertRow.condition.enabled !== false
                    Accessible.name: qsTr("Watch %1").arg(alertRow.condition.name ?? "")
                    onToggled: {
                        root.setEnabled(alertRow.index, checked)
                        checked = Qt.binding(() => alertRow.condition.enabled !== false)
                    }
                }
                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true
                    Label {
                        textFormat: Text.PlainText
                        objectName: "alertName"
                        text: alertRow.condition.name ?? ""
                        color: Theme.text
                        font.pixelSize: Theme.fontEmphasis
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        textFormat: Text.PlainText
                        objectName: "alertSummary"
                        text: root.summaryOf(alertRow.condition)
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "alertLevel"
                    readonly property string level: alertRow.condition.alert?.level ?? ""
                    visible: level !== ""
                    text: level === "danger" ? qsTr("Danger") : qsTr("Warning")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                }
                Label {
                    textFormat: Text.PlainText
                    objectName: "alertState"
                    visible: text !== ""
                    text: alertRow.stateText
                    color: alertRow.on ? (alertRow.condition.alert?.level === "danger" ? Theme.danger : Theme.warning)
                                       : Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    font.weight: alertRow.on ? Font.Bold : Font.Normal
                    wrapMode: Text.Wrap
                    Layout.maximumWidth: Math.max(80, root.width / 3)
                }
            }
        }
    }

    DocumentOpenDialog {
        id: importDialog
        objectName: "alertImportDialog"
        title: qsTr("Import alerts from a condition file")
        nameFilters: [qsTr("Condition files (*.json)"), qsTr("All files (*)")]
        onAccepted: root.importFrom(importDialog.selectedFile)
        onFailed: words => root.problem = words
    }
    DocumentSaveDialog {
        id: exportDialog
        objectName: "alertExportDialog"
        title: qsTr("Export the alerts")
        defaultSuffix: "json"
        currentFile: "conditions.json"
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.exportTo(exportDialog.selectedFile)
        onFailed: words => root.problem = words
    }
    function importFrom(url) {
        const r = Bridge.importConditions(url)
        if (!r.ok) {
            problem = r.words
            note = ""
            return false
        }
        problem = ""
        const imported = r.conditions === 1 ? qsTr("Imported 1 alert") : qsTr("Imported %1 alerts").arg(r.conditions)
        note = r.pending !== "" ? qsTr("%1: the change %2.").arg(imported).arg(r.pending) : imported + qsTr(".")
        return true
    }
    function exportTo(url) {
        const r = Bridge.exportConditions(url)
        problem = r.ok ? "" : r.words
        note = r.ok ? qsTr("Exported the alerts to a file.") : ""
        return r.ok
    }
}
