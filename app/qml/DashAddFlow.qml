// File/layer: app/qml/DashAddFlow.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit and its parts (QmlTest.dashedit_*), QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtQuick

pragma ComponentBehavior: Bound

// The Dashboard's Add flow: a kind, then a signal, a part of DashboardPage

Item {
    id: part
    required property DashboardPage host
    visible: false

    // ---- Add: a kind, then a signal

    property var profileRows: []
    // The address the rows were listed for with no session
    property string rowsFor: ""
    function refreshRows() {
        rowsFor = part.host.offlineUri
        profileRows = Bridge.profileSignals(Bridge.connected ? Bridge.currentUri : part.host.offlineUri)
    }
    // The rows a gauge of `kind` can show: numbers with references, an
    // enumeration only on a digital tile
    function pickable(kind) {
        return profileRows.filter(r => r.ref !== "" && !r.isText && (!r.isEnum || kind === "digital"))
    }
    // The picked sets' own signals a gauge of `kind` can show, one entry for
    // each set that has any ([{ title, rows }], the plugin first): offered
    // with or without a session, whatever profile the session reads
    function pickedOwnRows(kind) {
        const out = []
        for (const s of part.host.pickedSets) {
            const rows = Bridge.profileSignals("mock://?profile=" + s.profile)
                .filter(r => r.ref.startsWith("key:") && !r.isText && (!r.isEnum || kind === "digital"))
            if (rows.length === 0)
                continue
            // A plugin with several variants picked is told apart by its variant
            const twin = part.host.pickedSets.some(o => o !== s && o.name === s.name)
            out.push({ title: twin && s.variant ? qsTr("%1: %2").arg(s.name).arg(s.variant) : s.name,
                       rows: rows })
        }
        return out
    }
    function startAdd() {
        if (!DashboardBook.canAddCell(part.host.doc, part.host.pageIndex))
            return
        part.host.commitTyping()
        part.host.selected = -1
        part.host.addKind = ""
        part.host.changingKind = false
        part.host.panel = "add"
    }
    // The cell's own kind list, Add's with the cell's kind marked
    function changeKind() {
        part.host.commitTyping()
        part.host.addKind = ""
        part.host.changingKind = true
        part.host.panel = "add"
    }
    function pickKind(kind) {
        if (part.host.changingKind) {
            if (part.host.selected < 0)
                return
            let next = DashboardBook.setKind(part.host.doc, part.host.pageIndex, part.host.selected, kind)
            // A shift band isn't a dial's range: on an rpm signal, going to
            // Shift lights takes the band and leaving takes the default range
            // (another signal keeps its range both ways)
            if ((kind === "shift-light") !== (part.host.selectedCell.kind === "shift-light")
                    && (part.host.selectedCell.unit ?? "").toLowerCase() === "rpm") {
                const range = kind === "shift-light" ? Object.assign({}, DashboardBook.shiftBand)
                                                     : DashboardBook.defaultRange(part.host.selectedCell.unit)
                next = DashboardBook.setOptions(next, part.host.pageIndex, part.host.selected, range)
            }
            if (part.host.edit(next, part.host.historyPart.stepOf("", qsTr("changed %1's kind").arg(part.host.cellName(part.host.selectedCell)),
                                  part.host.selected, part.host.selected)))
                leaveKindList()
            return
        }
        if (kind === "blank") {
            const span = part.host.newSpans[kind]
            if (part.host.edit(DashboardBook.addCell(part.host.doc, part.host.pageIndex, { kind: kind, w: span[0], h: span[1], refs: [] }),
                     part.host.historyPart.stepOf("", qsTr("added %1").arg(part.host.kindNames[kind]), -1, part.host.shownCells.length)))
                part.host.select(part.host.shownCells.length - 1)
            return
        }
        part.host.addKind = kind
        refreshRows()
        part.host.panel = "signal"
    }
    function leaveKindList() {
        part.host.changingKind = false
        part.host.panel = "cell"
    }
    function changeSignal() {
        part.host.addKind = ""
        refreshRows()
        part.host.panel = "signal"
    }

    // What a gauge on this row starts with, in the unit the cell stores: the
    // signal file's own gauge (its range and levels), else the row's range
    // and, for a common signal, the zones of DashboardBook's table; a shift
    // light on engine speed starts from the shift band. All of it is written
    // into the cell, so the panel shows ordinary values to type over or clear
    function startOptions(kind, row) {
        const g = row.gauge ?? ({})
        const hasGauge = g.min !== undefined && g.max !== undefined
        const r = DashboardBook.defaultRange(row.decodedUnit, row.decodedLow, row.decodedHigh)
        const options = hasGauge ? { from: g.min, to: g.max } : { from: r.from, to: r.to }
        const key = part.host.keyOfRef(row.ref)
        if (kind === "shift-light") {
            if (key === "rpm")
                Object.assign(options, DashboardBook.shiftBand)
            return options
        }
        if (g.warnLow !== undefined || g.warnHigh !== undefined) {
            if (g.warnLow !== undefined)
                options.warnBelow = g.warnLow
            if (g.warnHigh !== undefined)
                options.warnFrom = g.warnHigh
        } else {
            Object.assign(options, DashboardBook.zonesFor(key))
        }
        return options
    }
    // True when startOptions' range is the unit table's or the field's own
    // extremes: no gauge block and no common signal's table entry
    function rangeIsGuess(kind, row) {
        const g = row.gauge ?? ({})
        const hasGauge = g.min !== undefined && g.max !== undefined
        return kind !== "shift-light" && !hasGauge && part.host.keyOfRef(row.ref) === ""
    }
    // The cell whose From and To were guessed at add time: { page, index, ref,
    // from, to }, or null. Page state only, never written to the layout; the
    // panel's note shows while the cell still holds those two values
    property var guessed: null
    function noteGuess(kind, row, index) {
        const o = startOptions(kind, row)
        guessed = rangeIsGuess(kind, row)
                  ? { page: part.host.pageIndex, index: index, ref: row.ref, from: o.from, to: o.to }
                  : null
    }
    // A new gauge's cell (section 5): the row's name, its unit and what a
    // gauge on it starts with (startOptions); decimals follow the row
    function newCell(kind, row) {
        const span = part.host.newSpans[kind] ?? [1, 4]
        return { kind: kind, w: span[0], h: span[1], label: part.host.labelFor(row),
                 refs: part.host.counterparts(row.ref), unit: row.decodedUnit,
                 options: startOptions(kind, row) }
    }
    function pickSignal(row) {
        if (part.host.addKind !== "") {
            const cell = newCell(part.host.addKind, row)
            if (part.host.edit(DashboardBook.addCell(part.host.doc, part.host.pageIndex, cell),
                     part.host.historyPart.stepOf("", qsTr("added %1").arg(part.host.cellName(cell)), -1, part.host.shownCells.length))) {
                part.host.addKind = ""
                part.host.select(part.host.shownCells.length - 1)
                noteGuess(cell.kind, row, part.host.selected)
            }
            return
        }
        if (part.host.selected < 0)
            return
        // Another signal: its name, unit, range and zones as a new gauge on
        // it would start, and none of the old signal's steps, words or
        // decimals, which were its own
        let next = DashboardBook.setCell(part.host.doc, part.host.pageIndex, part.host.selected, {
            refs: part.host.counterparts(row.ref), unit: row.decodedUnit, label: part.host.labelFor(row) })
        const cleared = {
            majorStep: null, minorStep: null, labelDivisor: null, caption: null,
            description: null, peakLabel: null, decimals: null }
        for (const zone of DashboardBook.zoneOptions)
            cleared[zone] = null
        next = DashboardBook.setOptions(next, part.host.pageIndex, part.host.selected,
                                        Object.assign(cleared, startOptions(part.host.selectedCell.kind, row)))
        part.host.edit(next, part.host.historyPart.stepOf("", qsTr("changed %1's signal").arg(part.host.cellName(part.host.selectedCell)), part.host.selected, part.host.selected))
        part.host.panel = "cell"
        noteGuess(part.host.selectedCell.kind, row, part.host.selected)
    }

    // Add's every signal of a set: the listed profile's own rows (a signal
    // file's or a plugin's, by key) that a gauge of `kind` can show, and of
    // those the ones this page doesn't show yet, as many as it has room for
    readonly property string listedName: {
        const uri = Bridge.connected ? Bridge.currentUri : part.host.offlineUri
        return uri === "" ? "" : part.host.profileName(part.host.profileOf(uri))
    }
    function addAllPlan(kind) {
        const own = pickable(kind).filter(r => r.ref.startsWith("key:"))
        let shown = []
        for (const c of part.host.shownCells)
            shown = shown.concat(Array.isArray(c.refs) ? c.refs : [])
        const missing = own.filter(r => shown.indexOf(r.ref) < 0)
        const room = Math.max(0, DashboardBook.maxCells - part.host.shownCells.length)
        return { own: own.length, missing: missing.length, adding: missing.slice(0, room) }
    }
    // The button's words: all, the rest, or as many as the page has room for
    function addAllWords(plan) {
        if (plan.adding.length < plan.missing)
            return qsTr("Add %1 of the %2 missing signals of %3: a page holds %4 cells")
                       .arg(plan.adding.length).arg(plan.missing).arg(listedName).arg(DashboardBook.maxCells)
        return plan.adding.length === plan.own
               ? qsTr("Add all %1 signals of %2").arg(plan.own).arg(listedName)
               : qsTr("Add %1 more of the %2 signals of %3").arg(plan.adding.length).arg(plan.own).arg(listedName)
    }
    function addAllSignals() {
        const plan = addAllPlan(part.host.addKind)
        if (part.host.addKind === "" || plan.adding.length === 0)
            return
        const cells = plan.adding.map(r => newCell(part.host.addKind, r))
        const words = cells.length === 1 ? qsTr("added 1 signal of %1").arg(listedName)
                                         : qsTr("added %1 signals of %2").arg(cells.length).arg(listedName)
        if (part.host.edit(DashboardBook.addCells(part.host.doc, part.host.pageIndex, cells), part.host.historyPart.stepOf("", words, -1, -1))) {
            part.host.addKind = ""
            part.host.panel = ""
        }
    }
    Connections {
        target: Bridge
        enabled: part.host.panel === "signal"
        function onConnectedChanged() { part.refreshRows() }
        function onUnitSystemChanged() { part.refreshRows() }
    }
}
