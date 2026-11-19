// File/layer: app/qml/RecordingPanes.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// The viewer's panes: the saved arrangement by view and key, the pane menu, ranges and shares, the plotted slots, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    function viewNameOf(folder) {
        const parts = String(folder).replace(/[\\/]+$/, "").split(/[\\/]/)
        return parts[parts.length - 1]
    }
    function writeView(patch) {
        if (part.host.viewName === "")
            return
        part.host.viewerSettings.views = JSON.stringify(ChartLayout.viewsWith(part.host.viewsBook, part.host.viewName, patch, Date.now()))
        part.host.chart.requestPaint()
    }
    // A signal's id: its key, else its name (ComparePairs.idOf)
    function idOf(s) {
        return ComparePairs.idOf(s)
    }
    function paneIdOf(s) {
        return part.host.panesByKey ? part.idOf(s) : s.name
    }
    // A signal's kept pane key: by its pane id, else by its name
    function paneOf(s) {
        const kept = part.host.viewAssign[part.paneIdOf(s)]
        return kept !== undefined ? kept : part.host.viewAssign[s.name]
    }
    // The assignment as the layout reads it: signal index -> pane key, the
    // signals the recording lacks passed by
    function assignRows() {
        const out = {}
        for (const s of part.host.recording.signalList) {
            const key = part.paneOf(s)
            if (key !== undefined)
                out[s.index] = key
        }
        return out
    }
    // A signal's pane, "" for the one its unit makes; kept by its pane id,
    // any entry by its name dropped
    function setPaneOf(s, key) {
        const id = part.paneIdOf(s)
        const next = Object.assign({}, part.host.viewAssign)
        delete next[s.name]
        if (key === "")
            delete next[id]
        else
            next[id] = key
        part.writeView({ assign: next })
    }
    // The panes a signal can join: each ordinary pane now drawn, named by the
    // signals plotted in it
    function paneChoices() {
        const l = part.host.chart.drawn
        const out = []
        if (!l)
            return out
        const byKey = {}
        for (const tr of l.traces) {
            if (tr.pinned || tr.pane === undefined)
                continue
            const key = l.panes[tr.pane].key
            if (key === "mixed" || key === "difference")
                continue
            const s = part.host.recording.signalList.find(x => x.index === tr.row)
            if (s === undefined)
                continue
            if (byKey[key] === undefined) {
                byKey[key] = []
                out.push({ key: key, names: byKey[key] })
            }
            if (byKey[key].indexOf(s.name) < 0)
                byKey[key].push(s.name)
        }
        return out.map(c => ({ key: c.key, label: c.names.join(", ") }))
    }
    // A new pane's key: the lowest "own:n" no pane or assignment holds
    function newPaneKey() {
        const used = Object.keys(part.host.viewAssign).map(n => part.host.viewAssign[n])
        for (const c of part.paneChoices())
            used.push(c.key)
        let n = 1
        while (used.indexOf("own:" + n) >= 0)
            ++n
        return "own:" + n
    }
    // The pane a plotted signal is drawn in: its label, "" when not drawn
    function paneLabelOf(index) {
        const l = part.host.chart.drawn
        if (!l)
            return ""
        for (const tr of l.traces) {
            if (tr.row === index && !tr.pinned && tr.pane !== undefined)
                return l.panes[tr.pane].label
        }
        return ""
    }
    function openPaneMenu(index, anchor) {
        const s = part.host.recording.signalList.find(x => x.index === index)
        if (s === undefined || s.isText || part.host.plotted[index] !== true)
            return
        part.host.paneMenuSignal = { key: s.key, name: s.name }
        const here = part.paneOf(s)
        const items = [{ text: qsTr("By unit"), key: "", checked: here === undefined }]
        for (const c of part.paneChoices())
            items.push({ text: qsTr("With %1").arg(c.label), key: c.key, checked: here === c.key })
        items.push({ text: qsTr("New pane"), key: "new", checked: false })
        part.host.paneMenuItems = items
        part.host.paneMenu.popup(anchor)
    }
    function choosePane(key) {
        part.setPaneOf(part.host.paneMenuSignal, key === "new" ? part.newPaneKey() : key)
    }
    // The signal whose value label lies at (x, y) in the chart, -1 for none
    function labelRowAt(x, y) {
        const l = part.host.chart.drawn
        if (!l)
            return -1
        for (const tr of l.traces) {
            if (tr.pinned || tr.labelY === undefined || !tr.labeled)
                continue
            if (x >= l.labelX - 8 && Math.abs(y - tr.labelY) <= ChartLayout.lineH)
                return tr.row
        }
        return -1
    }
    function setPaneRange(key, range) {
        const next = Object.assign({}, part.host.paneRanges)
        if (range)
            next[key] = { vMin: range.vMin, vMax: range.vMax }
        else
            delete next[key]
        part.host.paneRanges = next
        part.host.chart.requestPaint()
    }
    function setPaneShares(count, shares) {
        const next = Object.assign({}, part.host.paneShares)
        if (shares)
            next[count] = shares
        else
            delete next[count]
        part.writeView({ shares: next })
    }
    function notePanes(l) {
        const boxes = !l ? [] : l.panes.map((p, i) => ({ index: i, key: p.key, axis: p.axis,
                                                          top: p.top, h: p.h, left: l.left,
                                                          plotW: l.plotW }))
        if (JSON.stringify(boxes) !== JSON.stringify(part.host.paneBoxes))
            part.host.paneBoxes = boxes
    }
    // What a double tap at x, y on the chart means: a pane's axis (a touch
    // wide on a phone) returns its range to automatic, a seam's band evens
    // the heights, anywhere else maximizes
    function axisStripWidth(l) {
        return part.host.phoneShape ? Math.max(l.left, Theme.touchTarget) : l.left
    }
    function doubleTapAt(x, y) {
        const l = part.host.chart.drawn
        if (l && l.panes.length > 1) {
            for (const sy of l.seams) {
                if (Math.abs(y - sy) <= ChartLayout.paneGap / 2 + 8 && x >= l.left) {
                    part.setPaneShares(l.panes.length, null)
                    return "seam"
                }
            }
        }
        if (l && l.left > 0 && x < part.axisStripWidth(l)) {
            const i = ChartLayout.paneAt(l, y)
            if (i >= 0 && l.panes[i].axis) {
                part.setPaneRange(l.panes[i].key, null)
                return "axis"
            }
        }
        part.host.refreshPart.toggleMaximized()
        return "maximize"
    }

    function reconcileSlots() {
        const next = {}
        const taken = {}
        for (const k of Object.keys(part.host.slots)) {
            if (part.host.plotted[k] === true) {
                next[k] = part.host.slots[k]
                taken[part.host.slots[k]] = true
            }
        }
        for (const k of Object.keys(part.host.plotted).map(Number).sort((a, b) => a - b)) {
            if (part.host.plotted[k] !== true || next[k] !== undefined)
                continue
            let slot = 0
            while (taken[slot])
                ++slot
            next[k] = slot
            taken[slot] = true
        }
        part.host.slots = next
    }
    // Only a plotted signal is drawn, and each plotted one has a slot, so a
    // trace never falls back to its row; the fallback is for any other caller
    function slotOf(index) {
        const slot = part.host.slots[index]
        return slot === undefined ? index : slot
    }
}
