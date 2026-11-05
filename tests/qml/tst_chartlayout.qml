// File/layer: tests/qml/tst_chartlayout.qml | Qt Quick Test, QML tier
// Testing: ChartLayout, the strip chart's layout arithmetic, with
//          synthetic series: nothing to draw, the plot's width against the
//          widest label, per-trace scaling with the minimum span, flat
//          lanes, segments broken at a gap, the time ticks and the value
//          labels nudged apart inside the plot, a crowded trace's band;
//          a recording's columns over a fixed span (a line through calm
//          columns, a band where one swings past its neighbors' lean, a
//          steep ramp a line, a gap past gapMs a break), columns over a
//          span of their own cut to the axis, and absolute ticks,
//          before zero too; a scaleKey's shared range and lane, a series'
//          dash. No bridge, no session.

import QtQuick
import QtTest
import App
TestCase {
    name: "ChartLayout"

    readonly property int chartW: 400
    readonly property int chartH: 200

    // Seven pixels a character, and a plain fixed-point value
    function opts(over) {
        const o = { gapMs: 3000, minSpanUnits: 20,
                    measure: text => 7 * text.length,
                    format: (v, decimals, unit) => Number(v).toFixed(decimals)
                                                   + (unit !== "" ? " " + unit : "") }
        for (const k in over)
            o[k] = over[k]
        return o
    }

    // A series of (t, v) pairs (no Array.flat in the QML engine)
    function series(row, pairs, decimals, unit) {
        return { row: row, samples: [].concat.apply([], pairs), decimals: decimals || 0, unit: unit || "" }
    }

    function layout(seriesList, over) {
        return ChartLayout.layout(seriesList, chartW, chartH, opts(over))
    }

    // A recording's columns, as RecordingView.columns gives them: NaN in
    // both lists for a column with no sample
    function columns(row, lows, highs, decimals) {
        return { row: row, columns: { lows: lows, highs: highs }, decimals: decimals || 0, unit: "" }
    }

    function near(actual, expected, tolerance, what) {
        verify(Math.abs(actual - expected) <= tolerance,
               what + ": " + actual + ", expected " + expected + " ± " + tolerance)
    }

    function zigzag(n) {
        const pairs = []
        for (let i = 0; i < n; ++i)
            pairs.push([i * 100, i % 2 === 0 ? 0.8 : 1.3])
        return pairs
    }

    function bandsOf(tr) { return tr.pieces.filter(p => p.band) }
    function linesOf(tr) { return tr.pieces.filter(p => !p.band) }
    function bandSpan(tr) {
        let span = 0
        for (const b of bandsOf(tr))
            span += b.top[b.top.length - 1].x - b.top[0].x
        return span
    }

    // A crowded trace that swings every point (about a pixel each, a
    // phone's chart through the limiter) draws as one band, each column
    // the whole swing; a sparse one is its line
    function test_aSwingingCrowdedTraceDrawsAsABand() {
        const l = layout([series(0, zigzag(360), 2), series(1, [[0, 1], [35900, 2]])])
        verify(l)
        const byRow = row => l.traces.filter(t => t.row === row)[0]
        const dense = byRow(0)
        const seg = dense.segments[0]
        compare(bandsOf(dense).length, 1, "one band, 360 points over " + l.plotW + " px")
        verify(bandSpan(dense) >= 0.9 * (seg[seg.length - 1].x - seg[0].x),
               "the band spans " + bandSpan(dense) + " px")
        const ys = seg.map(p => p.y)
        const top = Math.min.apply(null, ys), bottom = Math.max.apply(null, ys)
        const b = bandsOf(dense)[0]
        // The first and last points join the line on either side
        for (let i = 1; i < b.top.length - 1; ++i) {
            near(b.top[i].y, top, 0.001, "column " + i + "'s top")
            near(b.bottom[i].y, bottom, 0.001, "column " + i + "'s bottom")
            verify(b.top[i].x > b.top[i - 1].x, "left to right")
        }
        const sparse = byRow(1)   // two points across the plot
        compare(sparse.pieces.length, sparse.segments.length, "a piece a segment")
        for (let i = 0; i < sparse.pieces.length; ++i) {
            compare(sparse.pieces[i].band, false)
            compare(sparse.pieces[i].points, sparse.segments[i])
        }
        const wide = ChartLayout.layout([series(0, zigzag(40), 2)], 1200, chartH, opts({}))
        compare(bandsOf(wide.traces[0]).length, 0, "40 points over 1200 px")
    }

    // A swing whose reversals fall on the column edges (a point every
    // column) still bands: the swing counts over a column's neighbors
    function test_aSwingAtTheColumnsPitchBands() {
        // 355 px less the label's 54 is a 301 px plot: 150 steps of 2 px
        const l = ChartLayout.layout([series(0, zigzag(151), 2)], 355, chartH, opts({}))
        const tr = l.traces[0]
        const seg = tr.segments[0]
        const spacing = (seg[seg.length - 1].x - seg[0].x) / (seg.length - 1)
        near(spacing, ChartLayout.bandColumnW, 0.0001, "a point on every column edge")
        verify(bandSpan(tr) >= 0.9 * (seg[seg.length - 1].x - seg[0].x),
               "the band spans " + bandSpan(tr) + " px")
    }

    // A crowded ramp is its line, sampled at the column edges: no band,
    // whose two edges a slope apart drew a ramp double
    function test_aCrowdedRampStaysALine() {
        const pairs = []
        for (let i = 0; i < 360; ++i)
            pairs.push([i * 100, i / 3.6])
        const tr = layout([series(0, pairs, 0)]).traces[0]
        compare(tr.pieces.length, 1)
        compare(tr.pieces[0].band, false)
        const seg = tr.segments[0], a = seg[0], z = seg[seg.length - 1]
        const pts = tr.pieces[0].points
        verify(pts.length >= (z.x - a.x) / ChartLayout.bandColumnW - 1, pts.length + " points")
        for (const pt of pts)
            near(pt.y, a.y + (z.y - a.y) * (pt.x - a.x) / (z.x - a.x), 0.01, "on the ramp at " + pt.x)
        compare(pts[0], a, "from the first point")
        compare(pts[pts.length - 1], z, "to the last")
    }

    // A small wiggle on a crowded ramp (a sensor's noise, 5 % of its
    // range) stays a line: under bandSwingShare of the plot's height
    function test_aSmallWiggleStaysALine() {
        const pairs = []
        for (let i = 0; i < 360; ++i)
            pairs.push([i * 100, i / 3.6 + (i % 2 ? 5 : 0)])
        const l = layout([series(0, pairs, 0)])
        const tr = l.traces[0]
        const seg = tr.segments[0]
        const wiggle = Math.abs(seg[1].y - seg[0].y)
        verify(wiggle > ChartLayout.bandSwingMin && wiggle < ChartLayout.bandSwingShare * l.plotH,
               "a " + wiggle + " px wiggle on a " + l.plotH + " px plot")
        compare(bandsOf(tr).length, 0, "no band")
    }

    // A lone spike in a crowded trace bands only around it, its top edge
    // through the peak
    function test_aCrowdedSpikeKeepsItsPeak() {
        const pairs = []
        for (let i = 0; i < 360; ++i)
            pairs.push([i * 100, i === 180 ? 2 : 1])
        const tr = layout([series(0, pairs, 2)]).traces[0]
        const bands = bandsOf(tr)
        compare(bands.length, 1, "one band, at the spike")
        verify(bandSpan(tr) <= 4 * ChartLayout.bandColumnW, "the band spans " + bandSpan(tr) + " px")
        const peak = tr.segments[0][180].y
        near(Math.min.apply(null, bands[0].top.map(p => p.y)), peak, 0.001, "the top edge's peak")
        compare(linesOf(tr).length, 2, "the line on either side")
    }

    function test_nothingToDraw() {
        compare(layout([]), null, "no series")
        compare(layout(null), null, "no list")
        compare(layout([series(0, [[1000, 5]])]), null, "one sample has no time span")
        compare(layout([series(0, [[1000, 5]]), series(1, [[1000, 7]])]), null,
                "one sample each, the same instant")
    }

    function test_plotLeavesRoomForTheWidestLabel() {
        const l = layout([series(0, [[0, 1], [1000, 2]], 0, "rpm"),
                          series(1, [[0, 12.5], [1000, 13.75]], 2, "V")])
        verify(l)
        // Labels "2 rpm" and "13.75 V": the widest is 7 characters
        compare(l.plotW, chartW - (7 * 7 + ChartLayout.swatchW + ChartLayout.labelGap))
        compare(l.plotH, chartH - ChartLayout.axisH - ChartLayout.top - ChartLayout.bottomPad)
        compare(l.labelX, l.plotW + 6)
        compare(l.traces.length, 2)
        compare(l.traces.map(t => t.label).sort().join("|"), "13.75 V|2 rpm")
    }

    // A label column can't take more than 40 % of the width
    function test_labelColumnIsCapped() {
        const l = layout([series(0, [[0, 1], [1000, 2]], 0, "a very long unit name indeed")])
        compare(l.plotW, chartW - chartW * 0.4)
    }

    // A trace spans the plot's height from its own minimum to its maximum
    function test_eachTraceScalesToItsOwnRange() {
        const l = layout([series(0, [[0, 0], [1000, 100], [2000, 50]]),
                          series(1, [[0, 1000], [1000, 3000], [2000, 2000]])],
                         { minSpanUnits: 0 })
        for (const tr of l.traces) {
            compare(tr.segments.length, 1, "row " + tr.row + " is one segment")
            const ys = tr.segments[0].map(p => p.y)
            near(ys[0], l.top + l.plotH, 0.01, "row " + tr.row + " minimum at the bottom")
            near(ys[1], l.top, 0.01, "row " + tr.row + " maximum at the top")
            near(ys[2], l.top + l.plotH / 2, 0.01, "row " + tr.row + " midpoint halfway")
            near(tr.endY, ys[2], 0.01, "the trace ends at its last sample")
        }
        const xs = l.traces[0].segments[0].map(p => p.x)
        compare(xs[0], 0)
        compare(xs[2], l.plotW - 1)
        near(xs[1], (l.plotW - 1) / 2, 0.01, "time runs left to right")
    }

    // Series sharing a scaleKey (one signal in two recordings) take one
    // range over both, so a value sits at one height in each; flat ones
    // share a lane
    function test_aScaleKeySharesOneRange() {
        const a = series(0, [[0, 0], [1000, 100]])
        const b = series(0, [[0, 0], [1000, 200]])
        const apart = layout([a, b], { minSpanUnits: 0 })
        for (const tr of apart.traces)
            near(tr.segments[0][1].y, apart.top, 0.01, "each its own: its maximum at the top")

        a.scaleKey = 0
        b.scaleKey = 0
        const shared = layout([a, b], { minSpanUnits: 0 })
        near(shared.traces.find(t => t.endY > shared.top + 1).segments[0][1].y,
             shared.top + shared.plotH / 2, 0.01, "100 of 200: halfway")
        near(shared.traces.find(t => t.endY <= shared.top + 1).segments[0][1].y, shared.top, 0.01,
             "200 at the top")

        const flats = layout([series(0, [[0, 7], [1000, 7]]), series(1, [[0, 3], [1000, 3]]),
                              series(2, [[0, 7], [1000, 7]])].map((s, i) => {
                                  if (i !== 1) s.scaleKey = 5
                                  return s
                              }), { minSpanUnits: 0 })
        const y = row => flats.traces.find(t => t.row === row).segments[0][0].y
        near(y(0), y(2), 0.01, "one key, one lane")
        near(y(0), flats.top + flats.plotH / 3, 0.01, "the first lane")
        near(y(1), flats.top + flats.plotH * 2 / 3, 0.01, "the unkeyed one the second")
    }

    // A series' dash reaches its trace; without one the trace has none
    // and its row's is drawn
    function test_aSeriesDashReachesItsTrace() {
        const s = columns(1, [1, 2], [1, 2])
        s.dash = [8, 4]
        const l = layout([s, columns(2, [1, 2], [1, 2])], { span: { t0: 0, t1: 1000 } })
        compare(JSON.stringify(l.traces.find(t => t.row === 1).dash), "[8,4]")
        compare(l.traces.find(t => t.row === 2).dash, undefined)
        const pairs = series(3, [[0, 1], [1000, 2]])
        pairs.dash = [2, 3]
        compare(JSON.stringify(layout([pairs]).traces[0].dash), "[2,3]")
    }

    // A step of two units over a minimum span of twenty takes a tenth of
    // the height, where it would otherwise fill it (fuel level, 71-73 %)
    function test_minimumSpanKeepsSmallStepsSmall() {
        const fuel = [series(0, [[0, 71], [1000, 73], [2000, 71], [3000, 73]])]
        const wide = layout(fuel, { minSpanUnits: 20 })
        const ys = wide.traces[0].segments[0].map(p => p.y)
        near(Math.max(...ys) - Math.min(...ys), wide.plotH * 2 / 20, 0.01,
             "two units of a twenty-unit span")
        near((Math.max(...ys) + Math.min(...ys)) / 2, wide.top + wide.plotH / 2, 0.01,
             "widened around the midpoint")

        const full = layout(fuel, { minSpanUnits: 0 })
        const ys2 = full.traces[0].segments[0].map(p => p.y)
        near(Math.max(...ys2) - Math.min(...ys2), full.plotH, 0.01, "no minimum: full height")

        // The span counts displayed digits: at one decimal, 20 units is 2.0
        const volts = layout([series(0, [[0, 13.8], [1000, 14.2]], 1)], { minSpanUnits: 20 })
        const ys3 = volts.traces[0].segments[0].map(p => p.y)
        near(Math.max(...ys3) - Math.min(...ys3), volts.plotH * 0.4 / 2.0, 0.01,
             "0.4 V of a 2.0 V span")
    }

    // A trace that never moves by half a displayed digit is flat: it takes
    // a lane of its own (top down in list order) rather than the plot's
    // height, and flat traces don't stack
    function test_flatTracesTakeLanes() {
        const l = layout([series(0, [[0, 50], [1000, 50.2], [2000, 50]]),
                          series(1, [[0, 0], [1000, 10]]),
                          series(2, [[0, 7], [1000, 7]])], { minSpanUnits: 0 })
        const flat0 = l.traces.find(t => t.row === 0)
        const flat2 = l.traces.find(t => t.row === 2)
        const moving = l.traces.find(t => t.row === 1)
        const y0 = flat0.segments[0].map(p => p.y)
        verify(y0.every(y => Math.abs(y - y0[0]) < 0.01), "a flat trace is a level line")
        near(y0[0], l.top + l.plotH / 3, 0.01, "first lane, a third down")
        near(flat2.segments[0][0].y, l.top + l.plotH * 2 / 3, 0.01, "second lane, two thirds")
        near(moving.segments[0][0].y, l.top + l.plotH, 0.01, "the moving trace uses the height")
        near(moving.segments[0][1].y, l.top, 0.01)
    }

    // Samples more than gapMs apart start a new segment, so an outage is
    // a break, not a ramp; a shorter pause stays joined
    function test_gapBreaksTheTrace() {
        const pairs = [[0, 1], [1000, 2], [2000, 3], [6000, 4], [7000, 5]]
        const broken = layout([series(0, pairs)], { gapMs: 3000 })
        compare(broken.traces[0].segments.length, 2, "one gap of 4 s")
        compare(broken.traces[0].segments[0].length, 3)
        compare(broken.traces[0].segments[1].length, 2)
        near(broken.traces[0].endY, broken.traces[0].segments[1][1].y, 0.01,
             "the trace still ends at its last sample")

        const joined = layout([series(0, pairs)], { gapMs: 5000 })
        compare(joined.traces[0].segments.length, 1, "4 s is under a 5 s gap")
        compare(joined.traces[0].segments[0].length, 5)

        const twice = layout([series(0, [[0, 1], [5000, 2], [10000, 3]])], { gapMs: 3000 })
        compare(twice.traces[0].segments.length, 3, "every sample on its own")
    }

    function test_tickStep() {
        compare(ChartLayout.tickStep(3), 1)
        compare(ChartLayout.tickStep(6), 1)
        compare(ChartLayout.tickStep(6.5), 2)
        compare(ChartLayout.tickStep(12), 2)
        compare(ChartLayout.tickStep(12.5), 5)
        compare(ChartLayout.tickStep(60), 5)
        compare(ChartLayout.tickStep(60, 20, 40), 5, "room enough at 5 s")
        compare(ChartLayout.tickStep(60, 6, 40), 10, "raised until 40 px fit")
        compare(ChartLayout.tickStep(300, 1, 40), 60)
        compare(ChartLayout.tickStep(3, 100, 40), 1, "never under the span's step")
        compare(ChartLayout.tickStep(5000, 0.01, 40), 600, "the longest step at most")
    }

    // A window fixes the time axis's span before the newest sample, so
    // two seconds of history sit at the right of a 30 s axis
    function test_aWindowSpansTheAxis() {
        const l = layout([series(0, [[100000, 0], [102000, 1]])], { windowMs: 30000 })
        compare(l.ticks[l.ticks.length - 1].seconds, 30, "the axis runs back 30 s")
        const xs = l.traces[0].segments[0].map(p => p.x)
        verify(xs[0] > l.plotW * 0.9, "the first sample is at x " + xs[0] + " of " + l.plotW)
        compare(xs[xs.length - 1], l.plotW - 1, "the newest at the right edge")
    }

    // A long span on a narrow plot (Live on a phone, 45 s in about 380
    // px) takes a step whose captions don't overlap one another or "now"
    function test_captionsNeverOverlap() {
        const caption = s => s === 0 ? "now" : "\u2212" + s + " s"
        const measure = text => 9 * text.length
        const l = ChartLayout.layout([series(0, [[0, 0], [45000, 1]])], 420, 200, {
            gapMs: 0, minSpanUnits: 0, measure: measure,
            format: (v, d, u) => v.toFixed(d), caption: caption })
        verify(l.ticks.length > 1)
        compare(l.ticks[0].caption, "now")
        compare(l.ticks[1].caption, "\u2212" + l.ticks[1].seconds + " s")
        const shown = l.ticks.filter(t => t.captioned)
        for (let i = 1; i < shown.length; ++i) {
            const right = shown[i].x + measure(shown[i].caption) / 2
            const left = i === 1 ? shown[0].x - measure(shown[0].caption)
                                 : shown[i - 1].x - measure(shown[i - 1].caption) / 2
            verify(right <= left, "caption " + i + " ends at " + right + ", the next starts at " + left)
        }
    }

    // Ticks count seconds back from the newest sample, newest first; a
    // caption needs room left of its tick, the newest one always shows
    function test_ticksRunBackFromNow() {
        const l = layout([series(0, [[0, 0], [10000, 1]])])
        compare(l.ticks.map(t => t.seconds).join(","), "0,2,4,6,8,10")
        compare(l.ticks[0].x, l.plotW - 1, "now is at the right edge")
        compare(l.ticks[5].x, 0, "the oldest sample is at the left edge")
        verify(l.ticks[0].captioned)
        verify(!l.ticks[5].captioned, "no room left of x = 0 for a centered caption")
        verify(l.ticks[4].captioned, "the second tick's x is " + l.ticks[4].x)
        compare(l.ticks[0].align, "right", "now sits against the edge")
        compare(l.ticks[1].align, "center")
        compare(l.hGrid.length, 3)
        near(l.hGrid[1], l.top + l.plotH / 2, 0.01, "quarter lines")
    }

    // Three traces ending at the same height get labels a line apart:
    // the first stays on its trace and the others are pushed down a
    // line each; a trace ending at the very top or bottom keeps its
    // label inside the plot
    function test_labelsAreNudgedApartInsideThePlot() {
        const l = layout([series(0, [[0, 0], [1000, 1], [2000, 0.5]]),
                          series(1, [[0, 10], [1000, 20], [2000, 15]]),
                          series(2, [[0, 100], [1000, 300], [2000, 200]])],
                         { minSpanUnits: 0 })
        const ys = l.traces.map(t => t.labelY)
        const mid = l.top + l.plotH / 2
        near(ys[0], mid, 0.01, "the first label sits on its trace")
        near(ys[1], mid + ChartLayout.lineH, 0.01, "one line below")
        near(ys[2], mid + 2 * ChartLayout.lineH, 0.01, "two lines below")
        verify(ys[0] < ys[1] && ys[1] < ys[2], "returned top down")

        const edges = layout([series(0, [[0, 0], [1000, 1]]),
                              series(1, [[0, 1], [1000, 0]])], { minSpanUnits: 0 })
        const half = ChartLayout.lineH / 2
        near(edges.traces[0].labelY, edges.top + half, 0.01, "the top label stays inside")
        near(edges.traces[1].labelY, edges.top + edges.plotH - half, 0.01,
             "the bottom label stays inside")
    }

    // n traces all ending mid-plot, so their labels stack
    function stacked(n) {
        const list = []
        for (let i = 0; i < n; ++i)
            list.push(series(i, [[0, 0], [1000, 10], [2000, 5]]))
        return layout(list, { minSpanUnits: 0 })
    }

    // The labeled rows lie inside the plot, top down, at least labelPx
    // and at most lineH apart; returns how many
    function checkLabelRows(l, what) {
        const ys = l.traces.filter(t => t.labeled).map(t => t.labelY)
        const step = ys.length > 1 ? ys[1] - ys[0] : ChartLayout.lineH
        for (let i = 1; i < ys.length; ++i) {
            verify(ys[i] - ys[i - 1] >= ChartLayout.labelPx - 0.01,
                   what + ": rows " + (i - 1) + " and " + i + " overlap")
            verify(ys[i] - ys[i - 1] <= ChartLayout.lineH + 0.01,
                   what + ": rows " + (i - 1) + " and " + i + " spread past lineH")
        }
        verify(ys[0] - step / 2 >= l.top - 0.01, what + ": the top row leaves the plot")
        verify(ys[ys.length - 1] + step / 2 <= l.top + l.plotH + 0.01,
               what + ": the bottom row leaves the plot")
        return ys.length
    }

    // More labels than fit at lineH close up and all show; more than fit
    // even at labelPx show as many as fit, the stack's ends among them.
    // Every trace is still returned, so its line still draws.
    function test_labelsCloseUpThenThinWhenTheyDontFit() {
        const fit = stacked(3)
        compare(checkLabelRows(fit, "three"), 3)
        near(fit.traces[1].labelY - fit.traces[0].labelY, ChartLayout.lineH, 0.01,
             "rows that fit keep lineH")

        const closer = Math.floor(stacked(1).plotH / ChartLayout.lineH) + 2
        const tight = stacked(closer)
        verify(closer * ChartLayout.labelPx <= tight.plotH, "the case needs tightening only")
        compare(checkLabelRows(tight, closer + " traces"), closer, "every label shows")

        const many = 2 * Math.floor(stacked(1).plotH / ChartLayout.labelPx)
        const thin = stacked(many)
        compare(thin.traces.length, many, "every trace is still drawn")
        compare(checkLabelRows(thin, many + " traces"),
                Math.floor(thin.plotH / ChartLayout.labelPx), "as many as fit at labelPx")
        verify(thin.traces[0].labeled && thin.traces[many - 1].labeled,
               "the stack's ends keep their labels")
    }

    // A recording's calm columns over a fixed span draw as one line through
    // each column's middle, from the span's left edge to its right
    function test_calmColumnsAreALineAcrossTheSpan() {
        const lows = [], highs = []
        for (let c = 0; c < 10; ++c) {
            lows.push(40 + c)
            highs.push(41 + c)
        }
        const l = layout([columns(0, lows, highs)], { span: { t0: 0, t1: 1000 } })
        verify(l, "a span and columns lay out")
        const tr = l.traces[0]
        compare(bandsOf(tr).length, 0, "nothing swings")
        compare(linesOf(tr).length, 1)
        const pts = linesOf(tr)[0].points
        compare(pts[0].x, 0, "from the span's start")
        near(pts[pts.length - 1].x, l.plotW - 1, 0.01, "to its end")
        verify(pts[0].y > pts[pts.length - 1].y, "rising")
        compare(tr.label, "50", "the last column's highest")
    }

    // Columns on a span of their own, wider than the axis, are placed by
    // their own times and cut to the axis: a pan's grid
    function test_columnsOnTheirOwnSpanAreCutToTheAxis() {
        const lows = [], highs = []
        for (let c = 0; c < 11; ++c) {
            lows.push(40 + c)
            highs.push(41 + c)
        }
        const s = columns(0, lows, highs)
        s.columns.t0 = -50
        s.columns.t1 = 1050
        const l = layout([s], { span: { t0: 0, t1: 1000 } })
        const pts = linesOf(l.traces[0])[0].points
        const xOf = t => t / 1000 * (l.plotW - 1)
        compare(pts[0].x, 0, "the first column cut at the axis's start")
        near(pts[1].x, xOf(25), 0.01, "its middle inside the axis")
        near(pts[2].x, xOf(100), 0.01, "the next column's middle at its own time")
        near(pts[pts.length - 1].x, l.plotW - 1, 0.01, "the last cut at the axis's end")
    }

    // Without a span a column series has no axis: nothing to draw
    function test_columnsNeedASpan() {
        compare(layout([columns(0, [1, 2], [1, 2])]), null)
    }

    // One column that swings past its neighbors draws as a band joined to
    // the line on both sides
    function test_aSwingingColumnIsABand() {
        const lows = [], highs = []
        for (let c = 0; c < 20; ++c) {
            lows.push(c === 10 ? 0 : 50)
            highs.push(c === 10 ? 100 : 50)
        }
        const tr = layout([columns(0, lows, highs)], { span: { t0: 0, t1: 2000 } }).traces[0]
        compare(tr.pieces.map(p => p.band ? "band" : "line").join(","), "line,band,line")
        const before = tr.pieces[0].points, band = tr.pieces[1], after = tr.pieces[2].points
        compare(before[before.length - 1].x, band.top[0].x, "the line runs into the band")
        compare(band.top[band.top.length - 1].x, after[0].x, "and on out of it")
        verify(band.bottom[1].y - band.top[1].y > 100, "the band spans the swing")
    }

    // A steep ramp's columns are each tall, but their neighbors lean as
    // much: a line, ends included
    function test_aSteepRampOfColumnsStaysALine() {
        const tr = layout([columns(0, [0, 25, 50, 75], [25, 50, 75, 100])],
                          { span: { t0: 0, t1: 4000 } }).traces[0]
        compare(bandsOf(tr).length, 0)
        compare(linesOf(tr).length, 1)
    }

    // Empty columns between samples are bridged while they last no longer
    // than gapMs; past it the trace breaks
    function test_emptyColumnsBreakTheTraceOnlyPastTheGap() {
        const lows = [], highs = []
        for (let c = 0; c < 100; ++c) {
            const sampled = c % 10 === 0 || c >= 60
            lows.push(sampled && (c < 20 || c >= 60) ? 50 : NaN)
            highs.push(sampled && (c < 20 || c >= 60) ? 50 + (c % 2) : NaN)
        }
        // 100 ms a column: 0 and 10 a second apart, then nothing to 60
        const l = layout([columns(0, lows, highs)], { span: { t0: 0, t1: 10000 }, gapMs: 3000 })
        const lines = linesOf(l.traces[0])
        compare(lines.length, 2, "broken once, between 10 and 60")
        verify(lines[0].points[lines[0].points.length - 1].x < lines[1].points[0].x)
    }

    // The label shows labelValue when a series gives one
    function test_aSeriesLabelValueIsItsLabel() {
        const l = layout([columns(0, [10, 20], [10, 20], 1)],
                         { span: { t0: 0, t1: 1000 } })
        compare(l.traces[0].label, "20.0")
        const s = columns(0, [10, 20], [10, 20], 1)
        s.labelValue = 12.34
        compare(layout([s], { span: { t0: 0, t1: 1000 } }).traces[0].label, "12.3")
    }

    // A series' labelMs, its value's sample time, reaches its trace, from
    // columns and from samples alike; none leaves it undefined
    function test_aSeriesLabelMsReachesItsTrace() {
        const c = columns(0, [10, 20], [10, 20], 1)
        c.labelValue = 12
        c.labelMs = 250
        compare(layout([c], { span: { t0: 0, t1: 1000 } }).traces[0].labelMs, 250)
        compare(layout([{ samples: [0, 1, 500, 2], row: 0, decimals: 1, unit: "",
                          labelMs: 500 }], {}).traces[0].labelMs, 500)
        verify(!isFinite(layout([columns(0, [10, 20], [10, 20], 1)],
                                { span: { t0: 0, t1: 1000 } }).traces[0].labelMs))
    }

    // Absolute ticks fall on whole steps of the time itself, in time order,
    // each caption centered and kept inside the plot
    function test_absoluteTicksFallOnWholeSteps() {
        const l = layout([columns(0, [1, 2, 3], [1, 2, 3])],
                         { span: { t0: 12000, t1: 42000 }, absolute: true })
        compare(l.ticks.map(t => t.seconds).join(","), "15,20,25,30,35,40")
        near(l.ticks[0].x, (15000 - 12000) / 30000 * (l.plotW - 1), 0.01, "15 s")
        for (const tick of l.ticks) {
            compare(tick.align, "center")
            compare(tick.caption, String(tick.seconds))
        }
        verify(l.ticks[0].captioned)
    }

    // An axis aligned on another recording starts before zero: its ticks
    // run through zero on the same steps, with room for the caption at
    // either end (here the one before zero is the wider: 10 s steps, where
    // "20" alone would fit 2 s ones)
    function test_absoluteTicksRunThroughZero() {
        const caption = s => s < 0 ? "before " + (-s) : String(s)
        const l = layout([columns(0, [1, 2, 3], [1, 2, 3])],
                         { span: { t0: -10000, t1: 20000 }, absolute: true, caption: caption })
        compare(l.ticks.map(t => t.seconds).join(","), "-10,0,10,20")
        near(l.ticks[1].x, 10000 / 30000 * (l.plotW - 1), 0.01, "zero")
        compare(l.ticks[0].caption, "before 10")
    }
}
