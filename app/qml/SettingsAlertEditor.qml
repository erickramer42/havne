// File/layer: app/qml/SettingsAlertEditor.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.alerts, QmlTest.phone (test_qml)
// One condition (ADR-0063 section 5): its name, up to four terms, its hold and its uses, saved through core's loader; the Alerts page's own page

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

ColumnLayout {
    id: root
    objectName: "settingsAlertEditor"
    required property SettingsPage host
    required property SettingsAlerts alerts
    Layout.fillWidth: true
    spacing: Theme.spacing * 2

    // The terms as edited (a signal term's level and clear in texts)
    property var terms: []
    // Each term's level and clear as typed, with what they showed and
    // stored at the start: a field left as it was keeps its stored value
    property var texts: []
    // Each seconds field's text as the load showed it, by key: a field
    // left so keeps its stored value, past the three decimals shown
    property var secondsShown: ({})
    property string join: "all"
    property string level: "warning"
    property string problem: ""
    property bool confirmRemove: false
    // The term whose signal is being picked, -1 for none
    property int picking: -1
    readonly property bool isNew: root.alerts.editIndex < 0

    property alias nameField: nameInput
    property alias forField: forInput
    property alias clearForField: clearForInput
    property alias alertBox: alertCheck
    property alias bannerBox: bannerCheck
    property alias soundBox: soundCheck
    property alias recordBox: recordCheck
    property alias beforeField: beforeInput
    property alias afterField: afterInput
    property alias markBox: markCheck
    property alias termRepeater: termRepeater

    Connections {
        target: root.alerts
        function onEditRequested() {
            root.load(root.alerts.draft)
            root.host.open("alertedit")
        }
    }

    // A level as shown, from the def's unit to the display unit
    function shownLevel(unit, v) {
        return typeof v === "number" ? root.alerts.shownNumber(Bridge.displayValue(unit, v)) : ""
    }
    function shownSeconds(v) {
        return typeof v === "number" ? root.alerts.shownNumber(v) : ""
    }
    // A number as typed, in the locale or plain; NaN for one that isn't
    function numberOf(text) {
        const t = String(text).trim()
        if (t === "")
            return NaN
        try {
            return Number.fromLocaleString(Qt.locale(), t)
        } catch (e) {
            return Number(t)
        }
    }
    // A level typed in the display unit, in the def's (D5), rounded as the
    // Dashboard's zones are where the unit system converted it
    function stored(unit, typed) {
        const v = Bridge.decodedValue(unit, typed)
        return v === typed ? v : DashboardEdit.roundStored(v)
    }

    function textsOf(t) {
        const c = ConditionBook.comparisonOf(t)
        const ordered = c === "above" || c === "below"
        const level = ordered ? shownLevel(t.unit ?? "", t[c])
                              : typeof t[c] === "number" ? root.alerts.shownNumber(t[c]) : ""
        const clear = ordered ? shownLevel(t.unit ?? "", t.clear) : ""
        return { level: level, clear: clear, levelWas: level, clearWas: clear,
                 levelValue: t[c], clearValue: t.clear }
    }
    // A field's value: as stored while its text is what it showed, else
    // the typed number in the def's unit; NaN for text that isn't one
    function valueOf(text, was, value, unit, ordered) {
        if (String(text) === String(was) && typeof value === "number")
            return value
        const typed = numberOf(text)
        if (!isFinite(typed))
            return NaN
        return ordered ? stored(unit, typed) : typed
    }
    function load(c) {
        nameInput.text = c.name ?? ""
        terms = ConditionBook.termsOf(c).map(t => ConditionBook.copy(t))
        texts = terms.map(t => textsOf(t))
        join = ConditionBook.joinOf(c) === "any" ? "any" : "all"
        forInput.text = shownSeconds(c["for"])
        clearForInput.text = shownSeconds(c.clear_for)
        alertCheck.checked = c.alert !== undefined
        level = c.alert?.level === "danger" ? "danger" : "warning"
        bannerCheck.checked = c.alert?.banner === true
        soundCheck.checked = c.alert?.sound === true
        recordCheck.checked = c.record !== undefined
        beforeInput.text = shownSeconds(c.record?.before)
        afterInput.text = shownSeconds(c.record?.after)
        markCheck.checked = c.mark === true
        secondsShown = { "for": forInput.text, clear_for: clearForInput.text,
                         before: beforeInput.text, after: afterInput.text }
        problem = ""
        confirmRemove = false
        picking = -1
        root.alerts.refreshRows()
    }

    // ---- Terms
    function rowOf(t) {
        const all = DashboardEdit.lookupRefs(t.signal ?? [], root.alerts.gaugeSets)
        for (const r of root.alerts.rows) {
            if (all.indexOf(r.ref) >= 0)
                return r
        }
        return null
    }
    // The comparisons a term's signal takes: a named state's equals and
    // differs, a number's above and below, all four when it isn't known
    function comparisonsOf(t) {
        const r = rowOf(t)
        if (r === null)
            return ["above", "below", "equals", "differs"]
        return r.isEnum ? ["equals", "differs"] : ["above", "below"]
    }
    readonly property var comparisonNames: ({ above: qsTr("Above"), below: qsTr("Below"),
                                              equals: qsTr("Is"), differs: qsTr("Isn't") })
    readonly property var linkChoices: [
        { word: "not_ok", name: qsTr("Not OK") }, { word: "timeout", name: qsTr("Not responding") },
        { word: "request_failed", name: qsTr("Requests failing") },
        { word: "hard_failure", name: qsTr("Adapter lost") }]

    function setTerm(i, t, text) {
        const next = terms.slice()
        next[i] = t
        terms = next
        if (text !== undefined) {
            const nt = texts.slice()
            nt[i] = text
            texts = nt
        }
    }
    function addTerm() {
        if (terms.length >= ConditionBook.maxTerms)
            return
        terms = terms.concat([ConditionBook.signalTerm([], "", "above", 0)])
        texts = texts.concat([{ level: "", clear: "" }])
    }
    function removeTerm(i) {
        if (terms.length <= 1)
            return
        const t = terms.slice()
        const x = texts.slice()
        t.splice(i, 1)
        x.splice(i, 1)
        terms = t
        texts = x
        picking = -1
    }
    function setKind(i, kind) {
        if (kind === ConditionBook.kindOf(terms[i]))
            return
        setTerm(i, kind === "link" ? { link: "not_ok" } : ConditionBook.signalTerm([], "", "above", 0),
                { level: "", clear: "" })
    }
    function setComparison(i, comparison) {
        const t = terms[i]
        const old = ConditionBook.comparisonOf(t)
        setTerm(i, ConditionBook.withComparison(t, comparison, t[old] ?? 0))
    }
    // The picked row's refs as a Dashboard cell builds them, and its unit
    function setSignal(i, row) {
        const t = ConditionBook.copy(terms[i])
        t.signal = DashboardEdit.counterparts(row.ref, root.alerts.gaugeSets).slice(0, ConditionBook.maxRefs)
        t.unit = row.decodedUnit
        // Another unit reads the shown texts afresh
        if (t.unit !== terms[i].unit) {
            const x = texts.slice()
            x[i] = { level: texts[i].level, clear: texts[i].clear }
            texts = x
        }
        let next = t
        const c = ConditionBook.comparisonOf(t)
        if (row.isEnum && (c === "above" || c === "below" || c === ""))
            next = ConditionBook.withComparison(t, "equals", t[c] ?? 0)
        else if (!row.isEnum && (c === "equals" || c === "differs"))
            next = ConditionBook.withComparison(t, "above", t[c] ?? 0)
        setTerm(i, next)
        picking = -1
    }

    // ---- Save: the condition built from the fields, null with problem set
    function build() {
        const c = ConditionBook.copy(root.alerts.draft)
        c.name = nameInput.text.trim()
        const bytes = ConditionBook.nameBytes(c.name)
        if (bytes > ConditionBook.maxName) {
            problem = qsTr("The name is too long: it takes %1 bytes, %2 fit, and a letter outside A to Z takes two or more.")
                          .arg(bytes).arg(ConditionBook.maxName)
            return null
        }
        const built = []
        for (let i = 0; i < terms.length; ++i) {
            let t = ConditionBook.copy(terms[i])
            if (ConditionBook.kindOf(t) === "signal") {
                const comparison = ConditionBook.comparisonOf(t) || "above"
                const ordered = comparison === "above" || comparison === "below"
                const x = texts[i]
                const level = valueOf(x.level, x.levelWas, x.levelValue, t.unit ?? "", ordered)
                if (!isFinite(level)) {
                    problem = qsTr("The level of term %1 isn't a number.").arg(i + 1)
                    return null
                }
                t = ConditionBook.withComparison(t, comparison, level)
                if (ordered && String(x.clear).trim() !== "") {
                    const clear = valueOf(x.clear, x.clearWas, x.clearValue, t.unit ?? "", true)
                    if (!isFinite(clear)) {
                        problem = qsTr("The clear of term %1 isn't a number.").arg(i + 1)
                        return null
                    }
                    t.clear = clear
                } else {
                    delete t.clear
                }
            }
            built.push(t)
        }
        c.when = ConditionBook.whenOf(built, join, root.alerts.draft.when)
        const seconds = [[forInput, "for", c], [clearForInput, "clear_for", c]]
        if (recordCheck.checked) {
            c.record = Object.assign({}, c.record ?? {})
            seconds.push([beforeInput, "before", c.record], [afterInput, "after", c.record])
        } else {
            delete c.record
        }
        for (const [field, key, into] of seconds) {
            const text = field.text.trim()
            if (text === "") {
                delete into[key]
                continue
            }
            if (field.text === secondsShown[key] && typeof into[key] === "number")
                continue
            const v = numberOf(text)
            if (!isFinite(v)) {
                problem = qsTr("%1 isn't a number of seconds.").arg(text)
                return null
            }
            into[key] = v
        }
        if (alertCheck.checked) {
            const a = Object.assign({}, c.alert ?? {})
            a.level = level
            for (const [box, key] of [[bannerCheck, "banner"], [soundCheck, "sound"]]) {
                if (box.checked)
                    a[key] = true
                else
                    delete a[key]
            }
            c.alert = a
        } else {
            delete c.alert
        }
        if (markCheck.checked)
            c.mark = true
        else
            delete c.mark
        return c
    }
    // Whether the fields hold anything but the condition the editor opened
    // on: a field that isn't a number counts as a change. A key build sets
    // again keeps its place (V4's objects do), so the texts compare
    function changed() {
        const kept = problem
        const c = build()
        problem = kept
        return c === null || JSON.stringify(c) !== JSON.stringify(root.alerts.draft)
    }
    function save() {
        const c = build()
        if (c === null)
            return false
        if (!root.alerts.saveDoc(ConditionBook.withCondition(root.alerts.doc, root.alerts.editIndex, c))) {
            refused()
            return false
        }
        problem = ""
        root.host.back()
        return true
    }
    function remove() {
        if (root.alerts.saveDoc(ConditionBook.without(root.alerts.doc, root.alerts.editIndex)))
            root.host.back()
        else
            refused()
    }
    // A refusal is the editor's to say, never left on the list
    function refused() {
        problem = root.alerts.problem
        root.alerts.problem = ""
    }

    // ---- The page
    LabeledFlow {
        label: qsTr("Name:")
        ThemedTextField {
            id: nameInput
            objectName: "alertNameField"
            width: Math.min(320, root.width)
            maximumLength: ConditionBook.maxName
            placeholderText: qsTr("What it watches")
        }
    }

    SegmentRow {
        objectName: "alertJoin"
        visible: root.terms.length > 1
        Layout.fillWidth: Theme.compact
        SegmentButton {
            objectName: "alertJoin_all"
            tapHeight: Theme.touchTarget
            share: Theme.compact
            wrapMode: Theme.compact ? Text.Wrap : Text.NoWrap
            Layout.preferredHeight: Math.max(Theme.touchTarget, implicitContentHeight)
            labelSize: Theme.fontBody
            text: qsTr("All of these")
            selected: root.join === "all"
            checked: selected
            onClicked: root.join = "all"
        }
        SegmentButton {
            objectName: "alertJoin_any"
            tapHeight: Theme.touchTarget
            share: Theme.compact
            wrapMode: Theme.compact ? Text.Wrap : Text.NoWrap
            Layout.preferredHeight: Math.max(Theme.touchTarget, implicitContentHeight)
            labelSize: Theme.fontBody
            text: qsTr("Any of these")
            selected: root.join === "any"
            checked: selected
            onClicked: root.join = "any"
        }
    }

    Repeater {
        id: termRepeater
        model: root.terms.length
        GaugeCard {
            id: termCard
            required property int index
            readonly property var term: root.terms[index] ?? ({})
            readonly property string kind: ConditionBook.kindOf(term)
            readonly property string comparison: ConditionBook.comparisonOf(term)
            readonly property bool ordered: comparison === "above" || comparison === "below"
            // Reading the unit system makes the unit's binding follow it
            readonly property string shownUnit: Bridge.unitSystem >= 0 ? Bridge.displayUnit(term.unit ?? "") : ""
            property alias levelField: levelField
            property alias clearField: clearField
            objectName: "alertTerm_" + index
            Layout.fillWidth: true
            padding: 12
            implicitHeight: termColumn.implicitHeight + 2 * padding

            ColumnLayout {
                id: termColumn
                width: parent.width
                spacing: Theme.spacing

                SegmentRow {
                    visible: termCard.kind !== "check"
                    SegmentButton {
                        objectName: "alertTermKind_signal"
                        tapHeight: Theme.touchTarget
                        text: qsTr("A signal")
                        selected: termCard.kind === "signal"
                        checked: selected
                        onClicked: root.setKind(termCard.index, "signal")
                    }
                    SegmentButton {
                        objectName: "alertTermKind_link"
                        tapHeight: Theme.touchTarget
                        text: qsTr("The link")
                        selected: termCard.kind === "link"
                        checked: selected
                        onClicked: root.setKind(termCard.index, "link")
                    }
                }
                Label {
                    objectName: "alertTermCheck"
                    visible: termCard.kind === "check"
                    text: qsTr("A recording's frames check (%1): kept as the file has it").arg(String(termCard.term.check))
                    color: Theme.textDim
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                // A signal's term
                ThemedButton {
                    objectName: "alertTermSignal"
                    visible: termCard.kind === "signal"
                    Layout.fillWidth: true
                    Layout.maximumWidth: implicitWidth
                    text: (termCard.term.signal ?? []).length > 0
                          ? root.alerts.nameOfRefs(termCard.term.signal) : qsTr("Pick a signal...")
                    Accessible.description: root.alerts.unlistedRef(termCard.term.signal)
                    checkable: true
                    checked: root.picking === termCard.index
                    onClicked: {
                        root.alerts.refreshRows()
                        root.picking = root.picking === termCard.index ? -1 : termCard.index
                    }
                }
                AlertSignalPicker {
                    objectName: "alertTermPicker"
                    room: termColumn.width
                    visible: root.picking === termCard.index
                    rows: root.alerts.rows
                    emptyText: qsTr("No signals: connect, or pick a plugin for your vehicle under Settings > Signals.")
                    onPicked: row => root.setSignal(termCard.index, row)
                }
                // Equal shares on a phone: four comparisons fit 320 px
                SegmentRow {
                    visible: termCard.kind === "signal"
                    Layout.fillWidth: Theme.compact
                    Repeater {
                        model: root.comparisonsOf(termCard.term)
                        SegmentButton {
                            required property string modelData
                            objectName: "alertComparison_" + modelData
                            tapHeight: Theme.touchTarget
                            share: Theme.compact
                            wrapMode: Theme.compact ? Text.Wrap : Text.NoWrap
                            Layout.preferredHeight: Math.max(Theme.touchTarget, implicitContentHeight)
                            text: root.comparisonNames[modelData]
                            selected: termCard.comparison === modelData
                            checked: selected
                            onClicked: root.setComparison(termCard.index, modelData)
                        }
                    }
                }
                LabeledFlow {
                    visible: termCard.kind === "signal"
                    label: termCard.ordered ? qsTr("Level:") : qsTr("Value:")
                    ThemedTextField {
                        id: levelField
                        objectName: "alertLevelField"
                        width: Math.min(140, root.width)
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        text: root.texts[termCard.index]?.level ?? ""
                        Accessible.name: termCard.ordered && termCard.shownUnit !== ""
                                         ? qsTr("Level of term %1, %2").arg(termCard.index + 1).arg(termCard.shownUnit)
                                         : termCard.ordered ? qsTr("Level of term %1").arg(termCard.index + 1)
                                         : qsTr("Value of term %1").arg(termCard.index + 1)
                        onTextEdited: root.texts[termCard.index].level = text
                    }
                    Label {
                        objectName: "alertLevelUnit"
                        visible: termCard.ordered
                        text: termCard.shownUnit
                        color: Theme.textDim
                        height: levelField.height
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                LabeledFlow {
                    visible: termCard.kind === "signal" && termCard.ordered
                    label: termCard.comparison === "above" ? qsTr("Clears below:") : qsTr("Clears above:")
                    ThemedTextField {
                        id: clearField
                        objectName: "alertClearField"
                        width: Math.min(140, root.width)
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        placeholderText: qsTr("the level")
                        text: root.texts[termCard.index]?.clear ?? ""
                        Accessible.name: termCard.shownUnit !== ""
                                         ? qsTr("Clear level of term %1, %2").arg(termCard.index + 1).arg(termCard.shownUnit)
                                         : qsTr("Clear level of term %1").arg(termCard.index + 1)
                        onTextEdited: root.texts[termCard.index].clear = text
                    }
                    Label {
                        text: termCard.shownUnit
                        color: Theme.textDim
                        height: clearField.height
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                // The link's term: one of core's four words (ADR-0063 section 4)
                Flow {
                    visible: termCard.kind === "link"
                    Layout.fillWidth: true
                    spacing: Theme.spacing
                    Repeater {
                        model: root.linkChoices
                        ThemedButton {
                            required property var modelData
                            objectName: "alertLink_" + modelData.word
                            width: Math.min(implicitWidth, termColumn.width)
                            text: modelData.name
                            checkable: true
                            checked: termCard.term.link === modelData.word
                            onClicked: root.setTerm(termCard.index, Object.assign(ConditionBook.copy(termCard.term),
                                                                                  { link: modelData.word }))
                        }
                    }
                }

                ThemedButton {
                    objectName: "alertTermRemove"
                    visible: root.terms.length > 1
                    text: qsTr("Remove term %1").arg(termCard.index + 1)
                    flat: true
                    onClicked: root.removeTerm(termCard.index)
                }
            }
        }
    }
    ThemedButton {
        objectName: "alertTermAdd"
        visible: root.terms.length < ConditionBook.maxTerms
        text: qsTr("Add a term")
        onClicked: root.addTerm()
    }

    // ---- How long: a hold before it turns on, and before it clears
    LabeledFlow {
        label: qsTr("On after:")
        ThemedTextField {
            id: forInput
            objectName: "alertForField"
            width: Math.min(100, root.width)
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            placeholderText: "0"
        }
        Label {
            text: qsTr("s, off after:")
            color: Theme.textDim
            height: forInput.height
            verticalAlignment: Text.AlignVCenter
        }
        ThemedTextField {
            id: clearForInput
            objectName: "alertClearForField"
            width: Math.min(100, root.width)
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            placeholderText: "0"
        }
        Label {
            text: qsTr("s")
            color: Theme.textDim
            height: clearForInput.height
            verticalAlignment: Text.AlignVCenter
        }
    }

    // ---- What it does
    ThemedCheckBox {
        id: alertCheck
        objectName: "alertUseAlert"
        text: qsTr("Alert")
    }
    ColumnLayout {
        visible: alertCheck.checked
        Layout.fillWidth: true
        Layout.leftMargin: Theme.spacing * 2
        spacing: Theme.spacing
        SegmentRow {
            SegmentButton {
                objectName: "alertLevel_warning"
                tapHeight: Theme.touchTarget
                text: qsTr("Warning")
                selected: root.level === "warning"
                checked: selected
                onClicked: root.level = "warning"
            }
            SegmentButton {
                objectName: "alertLevel_danger"
                tapHeight: Theme.touchTarget
                text: qsTr("Danger")
                selected: root.level === "danger"
                checked: selected
                onClicked: root.level = "danger"
            }
        }
        ThemedCheckBox {
            id: bannerCheck
            objectName: "alertUseBanner"
            text: qsTr("Banner")
        }
        ThemedCheckBox {
            id: soundCheck
            objectName: "alertUseSound"
            text: qsTr("Sound")
        }
        Label {
            objectName: "alertSoundNote"
            text: qsTr("Sound plays on Android.")
            color: Theme.textDim
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
    ThemedCheckBox {
        id: recordCheck
        objectName: "alertUseRecord"
        text: qsTr("Record")
    }
    LabeledFlow {
        visible: recordCheck.checked
        Layout.leftMargin: Theme.spacing * 2
        label: qsTr("Before:")
        ThemedTextField {
            id: beforeInput
            objectName: "alertBeforeField"
            width: Math.min(100, root.width)
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            placeholderText: "0"
        }
        Label {
            text: qsTr("s, after:")
            color: Theme.textDim
            height: beforeInput.height
            verticalAlignment: Text.AlignVCenter
        }
        ThemedTextField {
            id: afterInput
            objectName: "alertAfterField"
            width: Math.min(100, root.width)
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            placeholderText: "0"
        }
        Label {
            text: qsTr("s")
            color: Theme.textDim
            height: afterInput.height
            verticalAlignment: Text.AlignVCenter
        }
    }
    ThemedCheckBox {
        id: markCheck
        objectName: "alertUseMark"
        text: qsTr("Mark the recording")
    }

    Label {
        objectName: "alertEditorProblem"
        visible: text !== ""
        text: root.problem
        color: Theme.danger
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacing
        visible: !root.confirmRemove
        ThemedButton {
            objectName: "alertSave"
            width: Math.min(implicitWidth, root.width)
            text: qsTr("Save")
            highlighted: true
            onClicked: root.save()
        }
        ThemedButton {
            objectName: "alertCancel"
            width: Math.min(implicitWidth, root.width)
            text: qsTr("Cancel")
            onClicked: root.host.back()
        }
        ThemedButton {
            objectName: "alertRemove"
            visible: !root.isNew
            width: Math.min(implicitWidth, root.width)
            text: qsTr("Remove")
            flat: true
            onClicked: root.confirmRemove = true
        }
    }
    ColumnLayout {
        visible: root.confirmRemove
        Layout.fillWidth: true
        spacing: Theme.spacing
        Label {
            objectName: "alertRemoveText"
            text: qsTr("Remove %1?").arg(nameInput.text.trim())
            color: Theme.text
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "alertRemoveYes"
                width: Math.min(implicitWidth, root.width)
                text: qsTr("Remove")
                onClicked: root.remove()
            }
            ThemedButton {
                objectName: "alertRemoveNo"
                width: Math.min(implicitWidth, root.width)
                text: qsTr("Keep it")
                flat: true
                onClicked: root.confirmRemove = false
            }
        }
    }
}
