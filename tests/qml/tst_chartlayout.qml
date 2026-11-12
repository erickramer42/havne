// File/layer: tests/qml/tst_chartlayout.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.chartlayout (test_qml)

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

    // A StripChart draws a band on its Shape: the trace's fill, faint in
    // its row's color, and its two edges, beside its lines
    Component {
        id: stripChart
        StripChart { shownW: 400; shownH: 200 }
    }

    function test_aBandIsTheChartsFillAndItsTwoEdges() {
        const chart = createTemporaryObject(stripChart, this, {
            fetch: w => [series(0, zigzag(360), 2)], layoutOpts: opts({}) })
        verify(chart)
        tryVerify(() => chart.drawn !== null, 2000, "drawn")
        const tr = chart.drawn.traces[0]
        const bands = bandsOf(tr).length
        verify(bands > 0, "the trace bands")
        const set = chart.tracePaths[0]
        compare(set.fill.pathElements[0].paths.length, bands, "a fill per band")
        compare(set.edge.pathElements[0].paths.length, 2 * bands, "two edges per band")
        compare(set.line.pathElements[0].paths.length, linesOf(tr).length, "the lines beside it")
        const b = bandsOf(tr)[0]
        compare(set.fill.pathElements[0].paths[0].length, b.top.length + b.bottom.length,
                "the fill runs along the top and back along the bottom")
        const c = Qt.color(Theme.seriesColor(0))
        near(set.fill.fillColor.a, chart.bandAlpha, 0.01, "the fill's alpha")
        near(set.fill.fillColor.r, c.r, 0.01, "the fill's red")
        verify(Qt.colorEqual(set.edge.strokeColor, Theme.seriesColor(0)), "the edges in the row's color")
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

    // A label wider than its column loses its unit from the end, never a
    // digit: with room for part of the unit that part and an ellipsis, with
    // less room than the number alone the number whole
    function test_aLabelsCutTakesTheUnitAndKeepsTheNumberWhole() {
        // 138 px of label room at 7 px a character: 19 characters
        const wide = layout([series(0, [[0, 1], [1000, 2]], 0, "a very long unit name indeed")])
        compare(wide.traces[0].label, "2 a very long unit…")
        // 100 px wide the column is capped at 40, which leaves the label 18:
        // less than "123456.789", let alone its unit
        const narrow = ChartLayout.layout([series(0, [[0, 1], [1000, 123456.789]], 3, "kPa")],
                                          100, chartH, opts())
        compare(narrow.traces[0].label, "123456.789", "the number whole, the unit gone")
        // Room for the number and an ellipsis, none for a letter of the unit
        const between = ChartLayout.layout([series(0, [[0, 1], [1000, 1234.5]], 1, "kPa")],
                                           160, chartH, opts())
        compare(between.traces[0].label, "1234.5", "no ellipsis stands for a unit wholly cut")
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

    // A cursor's time tag sits on the caption row: the captions it would
    // cover are left out, the others stay
    function test_aCursorsTagCoversTheCaptionsUnderIt() {
        const over = { span: { t0: 12000, t1: 42000 }, absolute: true }
        const plain = layout([columns(0, [1, 2, 3], [1, 2, 3])], over)
        over.tags = [{ ms: 20000, width: 40 }]
        const l = layout([columns(0, [1, 2, 3], [1, 2, 3])], over)
        const hidden = l.ticks.filter(t => !t.captioned).map(t => t.seconds)
        compare(plain.ticks.filter(t => !t.captioned).length, 0, "all captioned without a tag")
        compare(hidden.join(","), "20", "only the tick under the tag, and no other")
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

    // Panes (the recordings viewer's): a ramp of one unit from lo to hi
    function ramp(row, unit, lo, hi) {
        return series(row, [[0, lo], [10000, (lo + hi) / 2], [20000, hi]], 0, unit)
    }
    function paned(seriesList, height, panes, over) {
        // The ramps' samples are 10 s apart: one line, not three gaps
        const o = { panes: panes || {}, gapMs: 60000 }
        for (const k in over)
            o[k] = over[k]
        return ChartLayout.layout(seriesList, chartW, height, opts(o))
    }
    function inPane(l, row) {
        return l.traces.filter(t => t.row === row)[0].pane
    }

    // Without the panes option (Live) the chart is one pane with no axis,
    // the plot from the left edge, no seams
    function test_withoutPanesTheChartIsOnePaneWithNoAxis() {
        const l = layout([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)])
        compare(l.left, 0)
        compare(l.panes.length, 1)
        compare(l.panes[0].axis, false)
        compare(l.panes[0].label, "")
        compare(l.panes[0].ticks.length, 0)
        compare(l.seams.length, 0)
        compare(l.top, ChartLayout.top)
        compare(l.plotH, chartH - ChartLayout.axisH - ChartLayout.top - ChartLayout.bottomPad)
    }

    // Each trace goes to its unit's pane, the units of one quantity
    // sharing one; the panes stack in the order their first trace comes
    function test_tracesGoToTheirUnitsPaneInTheListsOrder() {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 120),
                         ramp(3, "%", 10, 40), ramp(4, "mph", 0, 70)], 600)
        compare(l.panes.map(p => p.key).join("|"), "unit rpm|unit %|speed")
        compare(l.panes.map(p => p.label).join("|"), "rpm|%|km/h, mph")
        compare([0, 1, 2, 3, 4].map(r => inPane(l, r)).join(","), "0,1,2,1,2")
        for (let i = 1; i < l.panes.length; ++i) {
            near(l.panes[i].top, l.panes[i - 1].top + l.panes[i - 1].h + ChartLayout.paneGap, 1e-9,
                 "pane " + i + " under the one before")
            near(l.seams[i - 1], l.panes[i].top - ChartLayout.paneGap / 2, 1e-9, "seam " + i)
        }
        const last = l.panes[l.panes.length - 1]
        near(l.top + l.plotH, last.top + last.h, 1e-9, "the plot's height spans the panes")
        // Every point inside its own pane
        for (const tr of l.traces) {
            const p = l.panes[tr.pane]
            for (const piece of tr.pieces)
                for (const pt of piece.points)
                    verify(pt.y >= p.top - 1e-9 && pt.y <= p.top + p.h + 1e-9, "row " + tr.row + " in its pane")
        }
    }

    // A pane chosen for a row (a later per-row choice) wins over its unit
    function test_anAssignedPaneWinsOverTheUnit() {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "km/h", 0, 120), ramp(2, "%", 0, 100)], 600,
                        { assign: { 0: "drive", 1: "drive" } })
        compare(l.panes.map(p => p.key).join("|"), "drive|unit %")
        compare(inPane(l, 1), 0)
    }

    // Past the limit, the last units fold into one mixed pane: no axis, its
    // traces each on its own scale as without panes, named at its axis
    function test_pastTheLimitTheLastUnitsFoldIntoAMixedPane_data() {
        return [ { tag: "4", max: 4, axes: 3 }, { tag: "3, a phone's page", max: 3, axes: 2 } ]
    }
    function test_pastTheLimitTheLastUnitsFoldIntoAMixedPane(data) {
        const list = [ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 120),
                      ramp(3, "°C", -10, 90), ramp(4, "V", 0, 50)]
        const l = paned(list, 800, { max: data.max, mixedLabel: "mixed" })
        compare(l.panes.length, data.max)
        for (let i = 0; i < data.axes; ++i)
            compare(l.panes[i].axis, true, "pane " + i)
        const mixed = l.panes[data.max - 1]
        compare(mixed.axis, false)
        compare(mixed.label, list.slice(data.axes).map(t => t.unit).join(" · "),
                "the folded units, joined")
        compare(mixed.ticks.length, 0)
        for (let r = data.axes; r < list.length; ++r)
            compare(inPane(l, r), data.max - 1, "row " + r + " folded")
        // Each folded trace fills the mixed pane on its own scale
        for (const tr of l.traces.filter(t => t.pane === data.max - 1)) {
            const ys = tr.pieces[0].points.map(p => p.y)
            near(Math.min.apply(null, ys), mixed.top, 1e-6, "row " + tr.row + "'s top")
            near(Math.max.apply(null, ys), mixed.top + mixed.h, 1e-6, "row " + tr.row + "'s foot")
        }
    }

    // Under paneMinH a pane the panes fold too, until they fit; one pane
    // left holds every trace mixed, or one unit with its axis
    function test_panesUnderTheirLeastHeightFold_data() {
        const room = n => n * ChartLayout.paneMinH + (n - 1) * ChartLayout.paneGap
                          + ChartLayout.top + ChartLayout.axisH + ChartLayout.bottomPad
        return [ { tag: "two fit", height: room(2), panes: 2, axes: 1 },
                 { tag: "one short of two", height: room(2) - 1, panes: 1, axes: 0 },
                 { tag: "three fit", height: room(3), panes: 3, axes: 3 } ]
    }
    function test_panesUnderTheirLeastHeightFold(data) {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 120)],
                        data.height)
        compare(l.panes.length, data.panes)
        compare(l.panes.filter(p => p.axis).length, data.axes)
        for (const p of l.panes)
            verify(p.h >= ChartLayout.paneMinH - 1e-9, "a pane " + p.h + " px tall")
        const one = paned([ramp(0, "rpm", 800, 6000)], 100)
        compare(one.panes.length, 1)
        compare(one.panes[0].axis, true, "one unit keeps its axis in any height")
    }

    // A pane's range is its traces' lowest to highest, padded 5 % either
    // side, so traces of one unit share a scale and fill the pane
    function test_aPanesRangeIsItsTracesPadded() {
        const l = paned([ramp(0, "%", 10, 40), ramp(1, "%", 20, 90)], 400)
        const p = l.panes[0]
        near(p.vMin, 10 - 4, 1e-9, "low")
        near(p.vMax, 90 + 4, 1e-9, "high")
        compare(p.manual, false)
        const yOf = v => p.top + (p.vMax - v) / (p.vMax - p.vMin) * p.h
        const byRow = row => l.traces.filter(t => t.row === row)[0]
        near(byRow(0).endY, yOf(40), 1e-6, "40 % on the shared scale")
        near(byRow(1).endY, yOf(90), 1e-6, "90 % on the shared scale")
        // A flat one is widened about its value and drawn at it
        const flat = paned([series(0, [[0, 50], [20000, 50]], 1, "%")], 400).panes[0]
        near((flat.vMin + flat.vMax) / 2, 50, 1e-9, "centered")
        near(flat.vMax - flat.vMin, 20 * 0.1 * 1.1, 1e-9, "20 displayed digits, padded")
    }

    // A y-axis marks 2 to 4 round values: steps of 1, 2, 2.5 or 5 times a
    // power of ten, the finest that fits
    function test_theAxisTicksAreRound_data() {
        return [ { tag: "0-100", lo: 0, hi: 100, most: 4, values: "0,50,100", decimals: 0 },
                 { tag: "6-94", lo: 6, hi: 94, most: 3, values: "25,50,75", decimals: 0 },
                 { tag: "tenths", lo: 0.12, hi: 0.31, most: 4, values: "0.15,0.2,0.25,0.3", decimals: 2 },
                 { tag: "across zero", lo: -12, hi: 3, most: 2, values: "-10,0", decimals: 0 },
                 { tag: "a quarter step", lo: 0, hi: 1, most: 4, values: "0,0.25,0.5,0.75,1", decimals: 2,
                   most5: true } ]
    }
    function test_theAxisTicksAreRound(data) {
        const t = ChartLayout.valueTicks(data.lo, data.hi, data.most5 ? 5 : data.most)
        compare(t.values.join(","), data.values)
        compare(t.decimals, data.decimals)
    }

    // Each pane's ticks sit at their values' heights, captioned as the page
    // writes a value, the one too near the unit left uncaptioned; the
    // axes' column is the widest caption's, the plot to its right
    function test_theAxisColumnHoldsTheCaptions() {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400)
        let widest = 0
        for (const p of l.panes) {
            verify(p.ticks.length >= 2 && p.ticks.length <= 4, p.key + ": " + p.ticks.length + " ticks")
            widest = Math.max(widest, 7 * p.label.length)
            for (const tick of p.ticks) {
                near(tick.y, p.top + (p.vMax - tick.value) / (p.vMax - p.vMin) * p.h, 1e-9, "tick " + tick.value)
                compare(tick.caption, String(tick.value))
                compare(tick.captioned, tick.y - ChartLayout.labelPx / 2 >= p.top + ChartLayout.labelPx + 1)
                if (tick.captioned)
                    widest = Math.max(widest, 7 * tick.caption.length)
                verify(l.hGrid.indexOf(tick.y) >= 0, "a grid line at each tick")
            }
        }
        compare(l.left, widest + ChartLayout.axisGap)
        compare(l.labelX, l.left + l.plotW + 6)
        for (const tr of l.traces)
            verify(tr.pieces[0].points[0].x >= l.left, "the traces start at the plot")
    }

    // A range the user set is the pane's, and a trace past it is cut at the
    // pane's edges rather than drawn over its neighbor
    function test_aRangeTheUserSetCutsTheTraceToItsPane() {
        const l = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400,
                        { ranges: { "unit %": { vMin: 20, vMax: 60 } } })
        const p = l.panes[1]
        compare(p.manual, true)
        compare(p.vMin, 20)
        compare(p.vMax, 60)
        compare(l.panes[0].manual, false)
        const tr = l.traces.filter(t => t.row === 1)[0]
        compare(tr.pieces.length, 1, "one run inside the pane")
        const pts = tr.pieces[0].points
        near(pts[0].y, p.top + p.h, 1e-6, "enters at the foot")
        near(pts[pts.length - 1].y, p.top, 1e-6, "leaves at the top")
        verify(tr.endY >= p.top && tr.endY <= p.top + p.h, "its label inside")
        // A range that isn't one is left out
        compare(paned([ramp(0, "%", 0, 100)], 400, { ranges: { "unit %": { vMin: 5, vMax: 5 } } })
                .panes[0].manual, false)
    }

    function test_aLineIsCutWhereItCrossesAnEdge() {
        const pts = [{ x: 0, y: 0 }, { x: 10, y: 100 }, { x: 20, y: 0 }, { x: 30, y: 50 }]
        const runs = ChartLayout.clipLine(pts, 25, 75)
        compare(runs.length, 3)
        compare(JSON.stringify(runs[0]), JSON.stringify([{ x: 2.5, y: 25 }, { x: 7.5, y: 75 }]))
        compare(JSON.stringify(runs[1]), JSON.stringify([{ x: 12.5, y: 75 }, { x: 17.5, y: 25 }]))
        compare(JSON.stringify(runs[2]), JSON.stringify([{ x: 25, y: 25 }, { x: 30, y: 50 }]))
        compare(ChartLayout.clipLine([{ x: 0, y: 10 }, { x: 5, y: 10 }], 25, 75).length, 0, "flat outside")
        compare(ChartLayout.clipLine([{ x: 0, y: 30 }, { x: 5, y: 40 }], 25, 75).length, 1, "inside whole")
        const band = ChartLayout.clipPieces([{ band: true, top: [{ x: 0, y: 0 }], bottom: [{ x: 0, y: 99 }] }],
                                            25, 75)[0]
        compare(band.top[0].y, 25)
        compare(band.bottom[0].y, 75)
    }

    // The seams' shares set the heights; none goes under paneMinH
    function test_theSharesSetTheHeights() {
        const two = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400,
                          { shares: { 2: [3, 1] } })
        near(two.panes[0].h / two.panes[1].h, 3, 1e-9, "three to one")
        const fair = ChartLayout.paneHeights([1, 3], 2, 200, 72)
        compare(fair.join(","), "72,128")
        compare(ChartLayout.paneHeights(null, 3, 300, 72).join(","), "100,100,100")
        compare(ChartLayout.paneHeights([1, 1], 3, 300, 72).join(","), "100,100,100",
                "shares for another count are ignored")
        // Each count keeps its own shares
        const three = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 99)], 400,
                            { shares: { 2: [3, 1] } })
        near(three.panes[0].h, three.panes[2].h, 1e-9, "equal")
        const both = { 2: [3, 1], 3: [1, 1, 2] }
        const kept = paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100), ramp(2, "km/h", 0, 99)], 400,
                           { shares: both })
        near(kept.panes[2].h / kept.panes[0].h, 2, 1e-9, "three's own shares")
        near(paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400, { shares: both }).panes[0].h
             / 3, paned([ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)], 400, { shares: both }).panes[1].h,
             1e-9, "two's own")
    }

    // A seam's drag trades its two neighbors' heights and no other's
    function test_aSeamTradesItsNeighborsHeights() {
        const s = ChartLayout.seamShares([100, 100, 100], 0, 20, 72)
        compare(s.map(v => Math.round(v * 300)).join(","), "120,80,100")
        const held = ChartLayout.seamShares([100, 100, 100], 1, 50, 72)
        compare(held.map(v => Math.round(v * 300)).join(","), "100,128,72", "the lower one keeps its least")
        const up = ChartLayout.seamShares([100, 100, 100], 0, -60, 72)
        compare(up.map(v => Math.round(v * 300)).join(","), "72,128,100", "the upper one keeps its least")
    }

    // The page's zoom and pan of a pane's range, by the pointer's value
    function test_aPanesRangeZoomsAboutThePointerAndPans() {
        const l = paned([ramp(0, "%", 0, 100), ramp(1, "rpm", 800, 6000)], 400)
        const p = l.panes[0]
        compare(ChartLayout.paneAt(l, p.top + 1), 0)
        compare(ChartLayout.paneAt(l, l.seams[0]), -1, "a seam is no pane")
        compare(ChartLayout.paneAt(l, l.panes[1].top + 1), 1)
        const y = p.top + p.h / 4
        const v = ChartLayout.valueAt(p, y)
        near(v, p.vMax - (p.vMax - p.vMin) / 4, 1e-9, "a quarter down")
        const zoomed = ChartLayout.scaledRange(p, y, 0.5)
        near(zoomed.vMax - zoomed.vMin, (p.vMax - p.vMin) / 2, 1e-9, "half the range")
        near(ChartLayout.valueAt({ top: p.top, h: p.h, vMin: zoomed.vMin, vMax: zoomed.vMax }, y), v, 1e-9,
             "the value under the pointer stays")
        const panned = ChartLayout.pannedRange({ vMin: 0, vMax: 100 }, 200, 50)
        compare(panned.vMin, 25)
        compare(panned.vMax, 125)
    }

    // The chart draws a paned layout: its panes are what it drew
    // The time axis's height is the page's to grow: the plot gives way
    function test_theAxisStripTakesItsHeightFromTheOption() {
        const s = [series(0, [[0, 0], [1000, 10]], 0, "rpm")]
        const plain = layout(s, { span: { t0: 0, t1: 1000 } })
        compare(plain.axisH, ChartLayout.axisH)
        const grown = layout(s, { span: { t0: 0, t1: 1000 }, axisH: ChartLayout.axisHCursor })
        compare(grown.axisH, ChartLayout.axisHCursor)
        compare(plain.plotH - grown.plotH, ChartLayout.axisHCursor - ChartLayout.axisH)
    }

    // A value's height on its trace: where the trace ends for its end
    // value, null outside a pane a user's range has narrowed, a flat
    // trace's lane for anything
    function test_aValuesHeightOnItsTrace() {
        const s = [series(0, [[0, 0], [500, 40], [1000, 100]], 0, "rpm")]
        const panes = { span: { t0: 0, t1: 1000 }, panes: { max: 4, minH: 72 } }
        const l = layout(s, panes)
        const tr = l.traces[0]
        fuzzyCompare(ChartLayout.traceYAt(l, tr, 100), tr.endY, 0.001)
        const p = l.panes[tr.pane]
        fuzzyCompare(ChartLayout.traceYAt(l, tr, (p.vMin + p.vMax) / 2), p.top + p.h / 2, 0.001)

        const narrow = layout(s, { span: { t0: 0, t1: 1000 },
                                   panes: { max: 4, minH: 72, ranges: { "unit rpm": { vMin: 20, vMax: 60 } } } })
        compare(ChartLayout.traceYAt(narrow, narrow.traces[0], 100), null, "over the range")
        compare(ChartLayout.traceYAt(narrow, narrow.traces[0], 0), null, "under the range")
        verify(ChartLayout.traceYAt(narrow, narrow.traces[0], 40) !== null, "inside it")

        const flat = layout([series(0, [[0, 5], [1000, 5]], 0, "")], { span: { t0: 0, t1: 1000 } })
        const f = flat.traces[0]
        verify(f.flat, "a flat trace")
        fuzzyCompare(ChartLayout.traceYAt(flat, f, 5), f.endY, 0.001)
    }

    // The extremes' dots: on the trace at their times, the highest filled;
    // out of view or past a user's range none; none on a compared trace
    function test_theExtremesDotsAreOnTheirTraces() {
        const s = [series(0, [[0, 0], [500, 40], [1000, 100]], 0, "rpm")]
        const l = layout(s, { span: { t0: 0, t1: 1000 }, panes: { max: 4, minH: 72 } })
        const stat = { 0: { lowestMs: 0, lowest: 0, highestMs: 1000, highest: 100 } }
        const dots = ChartLayout.extremeDots(l, stat, 0, 1000)
        compare(dots.length, 2)
        compare(dots[0].filled, false, "the lowest hollow")
        compare(dots[1].filled, true, "the highest filled")
        compare(dots[0].row, 0)
        fuzzyCompare(dots[0].x, l.left, 0.001)
        fuzzyCompare(dots[1].x, l.left + l.plotW - 1, 0.001)
        fuzzyCompare(dots[1].y, ChartLayout.traceYAt(l, l.traces[0], 100), 0.001)

        compare(ChartLayout.extremeDots(l, { 0: { lowestMs: -1, lowest: NaN, highestMs: -1, highest: NaN } },
                                        0, 1000).length, 0, "none with no samples")
        compare(ChartLayout.extremeDots(l, { 0: { lowestMs: 2000, lowest: 0, highestMs: 3000, highest: 100 } },
                                        0, 1000).length, 0, "none out of view")
        compare(ChartLayout.extremeDots(l, {}, 0, 1000).length, 0, "none with no statistics")

        const narrow = layout(s, { span: { t0: 0, t1: 1000 },
                                   panes: { max: 4, minH: 72, ranges: { "unit rpm": { vMin: 20, vMax: 60 } } } })
        compare(ChartLayout.extremeDots(narrow, stat, 0, 1000).length, 0, "none past the range")

        const dashed = [{ row: 0, samples: [0, 0, 1000, 100], decimals: 0, unit: "rpm", dash: [3, 3] }]
        const d = layout(dashed, { span: { t0: 0, t1: 1000 }, panes: { max: 4, minH: 72 } })
        compare(ChartLayout.extremeDots(d, stat, 0, 1000).length, 0, "none on a compared trace")
    }

    function test_aStripChartDrawsPanes() {
        const chart = createTemporaryObject(stripChart, this, {
            shownH: 400,
            fetch: w => [ramp(0, "rpm", 800, 6000), ramp(1, "%", 0, 100)],
            layoutOpts: opts({ panes: { mixedLabel: "mixed" } }) })
        tryVerify(() => chart.drawn !== null, 2000, "drawn")
        compare(chart.drawn.panes.length, 2)
        compare(chart.drawn.seams.length, 1)
        verify(chart.drawn.left > 0)
    }
}
