// File/layer: app/qml/ChartLayout.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_chartlayout.qml): nothing to draw, the
//          plot's room for the widest label, each trace scaled to its
//          own range with a minimum span, flat traces in lanes of their
//          own, a gap breaking a trace into segments, the time ticks'
//          step and captions, and the value labels nudged apart and kept
//          inside the plot, closed up and then thinned when they don't fit;
//          a crowded trace's wide swings as a band (each column its whole
//          range, a swing on a column edge too), a ramp, a small wiggle and
//          a spike's peak as its line. LiveViewPage.qml draws
//          what it returns; the drawing itself is manual smoke
//          (docs/testing.md).

pragma Singleton
import QtQml

// The strip chart's layout, as arithmetic on the model's series: no
// Canvas, no Theme, no text. LiveView passes the series from
// Bridge.signalModel.chartData(), the chart's size and two callbacks
// (how wide a label is, how a value is written) and strokes the result.
QtObject {
    // Geometry shared with the page, in logical pixels
    readonly property int labelPx: 11    // the value labels' font size
    readonly property int lineH: 14      // a value label's row, room permitting
    readonly property int swatchW: 12    // the stroke sample before a label
    readonly property int axisH: 16      // the time axis under the plot
    readonly property int top: 6         // above the plot
    readonly property int bottomPad: 4
    readonly property int labelGap: 14   // between the plot and the labels' column
    readonly property int minTickX: 24   // no caption left of this
    // A trace's stroke. Where its points crowd and the line swings back
    // and forth widely it draws as a band instead: per column, the lowest
    // to the highest the line passes through, filled faintly and edged
    // with a thin stroke. A signal that swings every window (lambda
    // through fuel cut) zigzags at about a pixel a point on a phone, which
    // hatched the plot as a line; as a band it reads as the range it
    // swings over. A small wiggle (a sensor's noise on a ramp, the rev
    // limiter's bounce) stays a line: banded, its edges drew it double
    readonly property int lineW: 2
    readonly property real bandEdgeW: 1.5
    readonly property int denseSpacing: 3   // px a point, below which it's dense
    readonly property int bandColumnW: 2    // px of the plot a band column covers
    readonly property real bandSwingShare: 0.1   // of the plot's height, the least swing banded
    readonly property int bandSwingMin: 6        // px, the least on a short plot

    // The time axis's tick step, in seconds, for a span in seconds
    function tickStep(spanS) {
        return spanS > 12 ? 5 : spanS > 6 ? 2 : 1
    }

    // A series' value range. decimals is the value's displayed precision:
    // a trace that never moves by half a displayed digit is flat, and a
    // range narrower than minSpanUnits displayed digits is widened around
    // its midpoint, so a one-digit step doesn't fill the plot.
    function valueRange(samples, decimals, minSpanUnits) {
        let vMin = samples[1], vMax = samples[1]
        for (let i = 1; i < samples.length; i += 2) {
            if (samples[i] < vMin) vMin = samples[i]
            if (samples[i] > vMax) vMax = samples[i]
        }
        const unit = Math.pow(10, -decimals)
        const flat = vMax - vMin < 0.5 * unit
        const minSpan = minSpanUnits * unit
        if (!flat && vMax - vMin < minSpan) {
            const mid = (vMin + vMax) / 2
            vMin = mid - minSpan / 2
            vMax = mid + minSpan / 2
        }
        return { vMin: vMin, vMax: vMax, flat: flat }
    }

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
    // lone spike keeps its peak.
    // [{ band: false, points: [{x, y}] }, { band: true, top, bottom }]
    function bandPieces(seg, w, swing) {
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
        add(Math.floor(seg[0].x / w), seg[0].x, seg[0].y, 0)
        for (let i = 1; i < seg.length; ++i) {
            const p = seg[i - 1], q = seg[i]
            let a = p
            const cut = b => {
                const c = Math.floor((a.x + b.x) / 2 / w)
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
            for (let c = Math.floor(p.x / w) + 1; c * w < q.x; ++c) {
                const x = c * w
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

    // Label rows pushed apart where traces end close together, and kept
    // inside the plot's height. traces have endY; each gets labeled, and
    // a labeled one labelY. Rows are lineH apart while they fit, closer
    // down to labelPx (the text's own height) when they don't; past
    // that, only as many as fit at labelPx are labeled, picked evenly
    // down the stack so each stays near its trace. The rows above the
    // chart show every value.
    function placeLabels(traces, plotTop, plotH) {
        const byY = traces.slice().sort((a, b) => a.endY - b.endY)
        const n = byY.length
        const keep = Math.min(n, Math.max(1, Math.floor(plotH / labelPx)))
        const kept = []
        for (const tr of byY)
            tr.labeled = false
        for (let j = 0; j < keep; ++j) {
            const tr = byY[keep === 1 ? 0 : Math.round(j * (n - 1) / (keep - 1))]
            tr.labeled = true
            kept.push(tr)
        }
        const step = Math.min(lineH, plotH / keep)   // at least labelPx, by keep
        for (let i = 0; i < kept.length; ++i) {
            kept[i].labelY = Math.max(kept[i].endY,
                                      i > 0 ? kept[i - 1].labelY + step : plotTop + step / 2)
        }
        for (let i = kept.length - 1; i >= 0; --i) {
            kept[i].labelY = Math.min(kept[i].labelY, i < kept.length - 1
                                                       ? kept[i + 1].labelY - step
                                                       : plotTop + plotH - step / 2)
        }
        return byY
    }

    // series: [{ samples: [t0, v0, t1, v1, ...] in ms, row, decimals, unit }]
    // opts: { gapMs, minSpanUnits, measure(text) -> width,
    //         format(value, decimals, unit) -> text }
    // Returns null when there is nothing to draw (no series with a
    // sample, or no time span), else:
    // { plotW, plotH, top, hGrid: [y], ticks: [{ x, seconds }] newest first,
    //   traces: [{ row, segments: [[{x, y}]], pieces (what's drawn,
    //              each a segment's line or, crowded, bandPieces'),
    //              endY, label, labeled, labelY }]
    //   in label order top down, labelX }
    function layout(series, width, height, opts) {
        if (!series || series.length === 0)
            return null

        // Shared TIME axis across series (actual timestamps)
        let tMin = Infinity, tMax = -Infinity
        for (const s of series) {
            const sp = s.samples
            if (sp.length >= 2) {
                if (sp[0] < tMin) tMin = sp[0]
                // flattened pairs end with a VALUE; the last timestamp is
                // at length - 2
                if (sp[sp.length - 2] > tMax) tMax = sp[sp.length - 2]
            }
        }
        if (!isFinite(tMin) || tMax <= tMin)
            return null

        // Per-series VALUE scaling, kept independent, so the numbers come
        // from the labels: one per trace, at its newest sample's height
        const traces = []
        let labelW = 0
        for (const s of series) {
            const sp = s.samples
            if (sp.length < 2)
                continue
            const range = valueRange(sp, s.decimals, opts.minSpanUnits)
            const label = opts.format(sp[sp.length - 1], s.decimals, s.unit)
            labelW = Math.max(labelW, opts.measure(label))
            traces.push({ sp: sp, row: s.row, vMin: range.vMin, vMax: range.vMax,
                          flat: range.flat, label: label })
        }

        const plotH = Math.max(1, height - axisH - top - bottomPad)
        const plotW = Math.max(1, width - Math.min(Math.ceil(labelW) + swatchW + labelGap,
                                                   width * 0.4))
        const xOf = t => (t - tMin) / (tMax - tMin) * (plotW - 1)
        const yOf = norm => top + (1 - norm) * plotH
        const swing = Math.max(bandSwingMin, bandSwingShare * plotH)

        // Flat traces each get a lane of their own instead of stacking on
        // one line, top down in list order; the label carries the value
        const flatCount = traces.filter(tr => tr.flat).length
        let lane = 0
        for (const tr of traces) {
            if (tr.flat)
                tr.lane = 1 - (++lane) / (flatCount + 1)
        }

        const hGrid = []
        for (let g = 1; g < 4; ++g)
            hGrid.push(top + plotH * g / 4)

        // Time ticks: seconds before the newest sample, newest first
        const spanS = (tMax - tMin) / 1000
        const stepS = tickStep(spanS)
        const ticks = []
        for (let k = 0; k * stepS <= spanS; ++k) {
            const x = xOf(tMax - k * stepS * 1000)
            ticks.push({ x: x, seconds: k * stepS, captioned: k === 0 || x >= minTickX })
        }

        const out = []
        for (const tr of traces) {
            const traceY = tr.flat ? () => yOf(tr.lane)
                                   : v => yOf((v - tr.vMin) / (tr.vMax - tr.vMin))
            // Its points' mean spacing across the time they cover
            const points = tr.sp.length / 2
            const spread = xOf(tr.sp[tr.sp.length - 2]) - xOf(tr.sp[0])
            const spacing = points > 1 ? spread / (points - 1) : Infinity
            const segs = segments(tr.sp, opts.gapMs, xOf, traceY)
            let pieces = []
            for (const seg of segs) {
                pieces = pieces.concat(spacing < denseSpacing
                                       ? bandPieces(seg, bandColumnW, swing)
                                       : [{ band: false, points: seg }])
            }
            out.push({ row: tr.row,
                       segments: segs,
                       pieces: pieces,
                       endY: traceY(tr.sp[tr.sp.length - 1]),
                       label: tr.label })
        }

        return { plotW: plotW, plotH: plotH, top: top, hGrid: hGrid, ticks: ticks,
                 traces: placeLabels(out, top, plotH), labelX: plotW + 6 }
    }
}
