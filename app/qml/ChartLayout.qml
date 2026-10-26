// File/layer: app/qml/ChartLayout.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_chartlayout.qml): nothing to draw, the
//          plot's room for the widest label, each trace scaled to its
//          own range with a minimum span, flat traces in lanes of their
//          own, a gap breaking a trace into segments, the time ticks'
//          step and captions, and the value labels nudged apart and kept
//          inside the plot, closed up and then thinned when they don't fit. LiveViewPage.qml draws what it returns; the
//          drawing itself is manual smoke (docs/testing.md).

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
    //   traces: [{ row, segments: [[{x, y}]], endY, label, labeled, labelY }]
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
            out.push({ row: tr.row,
                       segments: segments(tr.sp, opts.gapMs, xOf, traceY),
                       endY: traceY(tr.sp[tr.sp.length - 1]),
                       label: tr.label })
        }

        return { plotW: plotW, plotH: plotH, top: top, hGrid: hGrid, ticks: ticks,
                 traces: placeLabels(out, top, plotH), labelX: plotW + 6 }
    }
}
