// File/layer: tests/qml/tst_chartlayout.qml | Qt Quick Test, QML tier
// Testing: ChartLayout, the strip chart's layout arithmetic, with
//          synthetic series: nothing to draw, the plot's width against the
//          widest label, per-trace scaling with the minimum span, flat
//          lanes, segments broken at a gap, the time ticks and the value
//          labels nudged apart inside the plot. No bridge, no session.

import QtQuick
import QtTest
import QtTune

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

    function near(actual, expected, tolerance, what) {
        verify(Math.abs(actual - expected) <= tolerance,
               what + ": " + actual + ", expected " + expected + " ± " + tolerance)
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

    // A step of two units over a minimum span of twenty takes a tenth of
    // the height, where it would otherwise fill it (fuel level, 71–73 %)
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
}
