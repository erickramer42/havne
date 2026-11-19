// File/layer: app/qml/RecordingSeries.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// The viewer's chart series and the difference between a recording and its compare, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    // What a difference is taken between: a plotted number here and its
    // pair there in the same unit (the unit's case aside), each with samples
    function differencePairs() {
        if (!part.host.compareAligned || part.host.recording.state !== RecordingView.Open)
            return []
        const sigs = part.host.recording.signalList
        const theirs = part.host.compare.signalList
        return sigs.filter(s => !s.isText && part.host.plotted[s.index] && s.samples > 0)
                   .map(s => ({ mine: s.index, theirs: ComparePairs.pairOf(part.host.comparePairs, s.index) }))
                   .filter(p => p.theirs !== undefined && !theirs[p.theirs].isText
                                && theirs[p.theirs].samples > 0
                                && theirs[p.theirs].unit.toLowerCase() === sigs[p.mine].unit.toLowerCase())
    }
    // What an answer holds for: the pairs, the offset and the units
    function differencePairsKey(pairs) {
        return pairs.map(p => p.mine + ":" + p.theirs).join(",") + "|" + part.host.compareOffsetMs
               + "|" + Bridge.unitSystem
    }
    // Asks both recordings for the wanted grid's means, unless it is
    // answered or asked already
    function askDifference() {
        const w = part.host.differenceWanted
        if (!part.host.differenceShown || !part.host.compareAligned || w === null || w.pairs.length === 0)
            return
        if ((part.host.difference !== null && part.host.difference.key === w.key)
                || (part.host.differenceAsk !== null && part.host.differenceAsk.key === w.key))
            return
        const refused = part.host.differenceRefused !== null && part.host.differenceRefused.key === w.key
                        ? part.host.differenceRefused.count : 0
        if (refused >= 2)
            return
        // The grid's ends at whole microseconds, so each column's two spans
        // lie exactly the offset apart and hold the same samples
        const t0 = Math.round(w.g0 * 1000) / 1000
        const t1 = Math.round(w.g1 * 1000) / 1000
        // A view that refuses (0) answers nothing: the other isn't asked
        const mine = part.host.recording.columnMeans(w.pairs.map(p => p.mine), t0, t1, w.gn)
        const theirs = mine > 0 ? part.host.compare.columnMeans(w.pairs.map(p => p.theirs), t0 - part.host.compareOffsetMs,
                                                      t1 - part.host.compareOffsetMs, w.gn) : 0
        if (mine > 0 && theirs > 0) {
            part.host.differenceRefused = null
            part.host.differenceAsk = { key: w.key, pairsKey: w.pairsKey, pairs: w.pairs, t0: t0, t1: t1,
                              mine: mine, theirs: theirs, mineMeans: null, theirMeans: null }
        } else {
            part.host.differenceAsk = null
            part.host.differenceRefused = { key: w.key, count: refused + 1 }
        }
    }
    // One recording's means for the ask under way; with both, the difference
    function differenceAnswered(here, request, means) {
        const a = part.host.differenceAsk
        if (a === null || request !== (here ? a.mine : a.theirs))
            return
        if (here)
            a.mineMeans = means
        else
            a.theirMeans = means
        if (a.mineMeans === null || a.theirMeans === null)
            return
        part.host.differenceAsk = null
        // A read that failed answers with no rows: no difference to draw
        const rows = {}
        if (a.mineMeans.length === a.pairs.length && a.theirMeans.length === a.pairs.length) {
            for (let k = 0; k < a.pairs.length; ++k) {
                const m = a.mineMeans[k].means
                const t = a.theirMeans[k].means
                const d = []
                for (let c = 0; c < m.length; ++c)
                    d.push(t[c] - m[c])
                rows[a.pairs[k].mine] = d
            }
        }
        part.host.difference = { key: a.key, pairsKey: a.pairsKey, t0: a.t0, t1: a.t1, rows: rows }
        part.host.chart.requestPaint()
    }
    function setDifferenceShown(on) {
        part.host.differenceShown = on
        part.host.chart.requestPaint()
    }
    // Another recording on either side: an answer for the one before
    // holds for nothing
    function forgetDifference() {
        part.host.difference = null
        part.host.differenceWanted = null
        part.host.differenceAsk = null
        part.host.differenceRefused = null
    }

    // The rows a second of a recording's signals at those indexes
    function rowsEachSecond(list, indexes, lengthMs) {
        let rows = 0
        for (const i of indexes)
            rows += list[i].samples
        return rows / Math.max(1, lengthMs / 1000)
    }

    // The chart's series: each plotted number's columns over the view, its
    // label the cursor's value while that's in view, then the compared
    // recording's of the same names, dashed. Two pixels a column, or a
    // second when that would draw more than rawRowsPerFrame rows or hold
    // more than rawRowsCached
    function series(widthPx) {
        if (part.host.recording.state !== RecordingView.Open)
            return []
        // A plotted signal with no samples in this recording has nothing to
        // draw here, so its columns aren't read; its pair in the compared
        // recording still draws. With nothing drawn the chart says so
        const sigs = part.host.recording.signalList
        const plotted = sigs.filter(s => !s.isText && part.host.plotted[s.index]).map(s => s.index)
        if (plotted.length === 0)
            return []
        const indexes = plotted.filter(i => sigs[i].samples > 0)
        const theirs = part.host.compareAligned ? part.host.compare.signalList : []
        const pairs = !part.host.compareAligned ? []
            : plotted.map(i => ({ mine: i, theirs: ComparePairs.pairOf(part.host.comparePairs, i) }))
                     .filter(p => p.theirs !== undefined && !theirs[p.theirs].isText
                                  && theirs[p.theirs].samples > 0)
        const spanS = (part.host.viewT1 - part.host.viewT0) / 1000
        let n = Math.max(1, Math.floor((part.host.chart.drawn ? part.host.chart.drawn.plotW : widthPx * 0.8) / 2))
        const drawn = part.rowsEachSecond(sigs, indexes, part.host.cursorsPart.durationMs())
                      + part.rowsEachSecond(theirs, pairs.map(p => p.theirs), part.host.compare.durationMs)
        const held = Math.max(part.host.rowsPerSecond, part.host.compareAligned ? part.host.compareRowsPerSecond : 0)
        if (spanS * drawn > part.host.rawRowsPerFrame || spanS * held > part.host.rawRowsCached)
            n = Math.max(1, Math.min(n, Math.floor(spanS)))
        // The labels follow the readout's A, the earlier of the two cursors,
        // the compared recording's too
        const fromMs = part.host.hasSpan ? part.host.spanFromMs : part.host.cursorMs
        const fromInView = fromMs >= part.host.viewT0 && fromMs <= part.host.viewT1
        const labelOf = (at, shown) => shown && at && at.text !== "" ? Number(at.text) : NaN
        // Where that value's sample lies on the axis: a dot marks it
        const labelMsOf = (at, offset, shown) => shown && at && at.text !== "" && at.atMs >= 0
                                                 ? at.atMs + offset : NaN
        // The columns lie on a grid from time 0, not from the view's
        // start, so a pan carries each column's samples along with it
        // rather than regrouping them, and a noisy trace's peaks hold
        // still (Live's slices do the same). One more column covers the
        // view's ends; the chart cuts it to the view
        const colMs = (part.host.viewT1 - part.host.viewT0) / n
        const g0 = Math.floor(part.host.viewT0 / colMs) * colMs
        const gn = Math.max(1, Math.ceil((part.host.viewT1 - g0) / colMs - 1e-9))
        const g1 = g0 + gn * colMs
        const out = indexes.length === 0 ? [] : part.host.recording.columns(indexes, g0, g1, gn).map(c => {
            const s = sigs[c.index]
            return { columns: { lows: c.lows, highs: c.highs, t0: g0, t1: g1 }, row: c.index,
                     slot: part.host.panesPart.slotOf(c.index),
                     scaleKey: c.index, decimals: s.decimals, unit: s.unit,
                     labelValue: labelOf(part.host.readoutFrom[c.index], fromInView),
                     labelMs: labelMsOf(part.host.readoutFrom[c.index], 0, fromInView) }
        })
        if (pairs.length === 0)
            return out
        const cols = part.host.compare.columns(pairs.map(p => p.theirs), g0 - part.host.compareOffsetMs,
                                     g1 - part.host.compareOffsetMs, gn)
        for (let k = 0; k < cols.length; ++k) {
            const mine = pairs[k].mine
            const s = theirs[pairs[k].theirs]
            out.push({ columns: { lows: cols[k].lows, highs: cols[k].highs, t0: g0, t1: g1 },
                       row: mine, slot: part.host.panesPart.slotOf(mine),
                       scaleKey: mine, dash: Theme.seriesCompareDash(part.host.panesPart.slotOf(mine)), decimals: s.decimals,
                       unit: s.unit, labelValue: labelOf(part.host.compareReadout[mine], fromInView),
                       labelMs: labelMsOf(part.host.compareReadout[mine], part.host.compareOffsetMs, fromInView) })
        }
        if (!part.host.differenceShown)
            return out
        // Each difference in a pane of its own quantity, under the signals'.
        // The last answer draws where its own columns lie while a newer
        // grid's is read, which waits for the view to rest
        const apart = part.differencePairs()
        const pairsKey = part.differencePairsKey(apart)
        const key = [g0, g1, gn, pairsKey].join("|")
        if (part.host.difference !== null && part.host.difference.pairsKey === pairsKey) {
            for (const p of apart) {
                const d = part.host.difference.rows[p.mine]
                if (d === undefined)
                    continue
                const s = sigs[p.mine]
                const here = labelOf(part.host.readoutFrom[p.mine], fromInView)
                const there = labelOf(part.host.compareReadout[p.mine], fromInView)
                out.push({ columns: { lows: d, highs: d, t0: part.host.difference.t0, t1: part.host.difference.t1 },
                           row: p.mine, slot: part.host.panesPart.slotOf(p.mine), decimals: s.decimals, unit: s.unit,
                           pane: "difference " + ChartLayout.paneKey(s.unit), pinned: true,
                           paneLabel: qsTr("Δ %1").arg(s.unit), labelValue: there - here })
            }
        }
        if (part.host.differenceWanted === null || part.host.differenceWanted.key !== key)
            part.host.differenceWanted = { key: key, pairsKey: pairsKey, pairs: apart, g0: g0, g1: g1, gn: gn }
        if (!part.host.chart.moving)
            Qt.callLater(part.askDifference)
        return out
    }
}
