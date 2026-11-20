// File/layer: app/qml/ChartBands.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.chartlayout{,_panes} (test_qml)

// A dense trace's strokes as polylines and min-max bands: the sample
// pairs cut at outages, and the pieces a band is drawn as. ChartLayout's
// layout reads them; they read nothing of it.

pragma Singleton
import QtQml

QtObject {
    // The sample pairs as polylines, broken wherever two samples are more
    // than gapMs apart, so an outage isn't drawn as a ramp
    function segments(samples, gapMs, xOf, yOf) {
        const out = []
        let seg = []
        for (let i = 0; i < samples.length; i += 2) {
            if (i > 0 && samples[i] - samples[i - 2] > gapMs) {
                out.push(seg)
                seg = []
            }
            seg.push({ x: xOf(samples[i]), y: yOf(samples[i + 1]) })
        }
        if (seg.length > 0)
            out.push(seg)
        return out
    }

    // A crowded segment as pieces: per column of w px, where its line
    // enters and leaves, its lowest and highest y, how far it travels and
    // how often it turns back (each piece of line cut at the column
    // edges). Over a column and its neighbors, so a turn on a column edge
    // counts too, the travel beyond the net change per turn is the size
    // of a swing; past swing px the column swings. Calm columns join into
    // a line through their entries and exits; swinging ones into a band,
    // its top and bottom edges joined to the line on either side, so a
    // lone spike keeps its peak. The columns start at phase px (0 if
    // left out): the layout passes where time 0 falls, so a column covers
    // the same stretch of time at every paint as the chart scrolls.
    // [{ band: false, points: [{x, y}] }, { band: true, top, bottom }]
    function bandPieces(seg, w, swing, phase) {
        const off = phase || 0
        const colOf = x => Math.floor((x - off) / w)
        const cols = {}
        const add = (c, x, y, travel) => {
            let col = cols[c]
            if (!col) {
                col = cols[c] = { x0: x, yIn: y, x1: x, yOut: y, top: y, bottom: y, travel: 0,
                                  turns: 0, firstDir: 0, lastDir: 0 }
            } else {
                if (x < col.x0) { col.x0 = x; col.yIn = y }
                if (x >= col.x1) { col.x1 = x; col.yOut = y }
                col.top = Math.min(col.top, y)
                col.bottom = Math.max(col.bottom, y)
            }
            col.travel += travel
        }
        add(colOf(seg[0].x), seg[0].x, seg[0].y, 0)
        for (let i = 1; i < seg.length; ++i) {
            const p = seg[i - 1], q = seg[i]
            let a = p
            const cut = b => {
                const c = colOf((a.x + b.x) / 2)
                add(c, a.x, a.y, 0)
                add(c, b.x, b.y, Math.abs(b.y - a.y))
                const col = cols[c]
                if (b.y !== a.y) {
                    const dir = b.y > a.y ? 1 : -1
                    if (col.lastDir !== 0 && dir !== col.lastDir)
                        ++col.turns
                    if (col.firstDir === 0)
                        col.firstDir = dir
                    col.lastDir = dir
                }
                a = b
            }
            for (let c = colOf(p.x) + 1; c * w + off < q.x; ++c) {
                const x = c * w + off
                cut({ x: x, y: p.y + (q.y - p.y) * (x - p.x) / (q.x - p.x) })
            }
            cut(q)
        }
        const list = Object.keys(cols).map(Number).sort((a, b) => a - b).map(c => cols[c])
        const swings = list.map((col, i) => {
            const from = Math.max(0, i - 1), to = Math.min(list.length - 1, i + 1)
            let travel = 0, turns = 0
            for (let k = from; k <= to; ++k) {
                travel += list[k].travel
                turns += list[k].turns
                if (k > from && list[k - 1].lastDir * list[k].firstDir < 0)
                    ++turns
            }
            return turns > 0
                && (travel - Math.abs(list[to].yOut - list[from].yIn)) / turns > swing
        })
        const pieces = []
        let i = 0
        while (i < list.length) {
            let j = i
            while (j + 1 < list.length && swings[j + 1] === swings[i])
                ++j
            const first = list[i], last = list[j]
            if (swings[i]) {
                const top = [{ x: first.x0, y: first.yIn }]
                const bottom = [{ x: first.x0, y: first.yIn }]
                for (let k = i; k <= j; ++k) {
                    const x = (list[k].x0 + list[k].x1) / 2
                    top.push({ x: x, y: list[k].top })
                    bottom.push({ x: x, y: list[k].bottom })
                }
                top.push({ x: last.x1, y: last.yOut })
                bottom.push({ x: last.x1, y: last.yOut })
                pieces.push({ band: true, top: top, bottom: bottom })
            } else {
                const points = [{ x: first.x0, y: first.yIn }]
                for (let k = i; k <= j; ++k)
                    points.push({ x: list[k].x1, y: list[k].yOut })
                pieces.push({ band: false, points: points })
            }
            i = j + 1
        }
        return pieces
    }

    // A recording's columns as pieces (RecordingView.columns: each its
    // lowest and highest, the reader's per-second summaries when zoomed
    // out). cols are the columns with samples, in order: { c, x0, x1, top,
    // bottom } in px. Two more than gapCols columns apart break the trace,
    // as a gap does a live one. A column swings when its height beyond
    // what its neighbors' lean explains (a ramp's column is as tall as its
    // slope) is over swing px; swinging runs draw as a band, the rest as a
    // line through each column's middle, joined end to end.
    function columnPieces(cols, gapCols, swing) {
        let pieces = []
        let start = 0
        for (let i = 1; i <= cols.length; ++i) {
            if (i === cols.length || cols[i].c - cols[i - 1].c - 1 > gapCols) {
                pieces = pieces.concat(columnRunPieces(cols.slice(start, i), swing))
                start = i
            }
        }
        return pieces
    }

    function columnRunPieces(seg, swing) {
        const n = seg.length
        const mid = k => (seg[k].top + seg[k].bottom) / 2
        // The lean: the line's slope a column across the neighbors it has
        const swings = seg.map((col, k) => {
            const a = Math.max(0, k - 1), b = Math.min(n - 1, k + 1)
            const lean = b > a ? Math.abs(mid(b) - mid(a)) / (b - a) : 0
            return col.bottom - col.top - lean > swing
        })
        const pieces = []
        let i = 0
        while (i < n) {
            let j = i
            while (j + 1 < n && swings[j + 1] === swings[i])
                ++j
            if (swings[i]) {
                const top = [{ x: seg[i].x0, y: mid(i) }]
                const bottom = [{ x: seg[i].x0, y: mid(i) }]
                for (let k = i; k <= j; ++k) {
                    const x = (seg[k].x0 + seg[k].x1) / 2
                    top.push({ x: x, y: seg[k].top })
                    bottom.push({ x: x, y: seg[k].bottom })
                }
                top.push({ x: seg[j].x1, y: mid(j) })
                bottom.push({ x: seg[j].x1, y: mid(j) })
                pieces.push({ band: true, top: top, bottom: bottom })
            } else {
                const points = i > 0 ? [{ x: seg[i - 1].x1, y: mid(i - 1) }]
                                     : [{ x: seg[i].x0, y: mid(i) }]
                for (let k = i; k <= j; ++k)
                    points.push({ x: (seg[k].x0 + seg[k].x1) / 2, y: mid(k) })
                points.push(j < n - 1 ? { x: seg[j + 1].x0, y: mid(j + 1) }
                                      : { x: seg[j].x1, y: mid(j) })
                pieces.push({ band: false, points: points })
            }
            i = j + 1
        }
        return pieces
    }
}
