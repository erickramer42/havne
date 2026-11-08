// File/layer: tests/qml/tst_kinds.qml | Qt Quick Test, QML tier
// Testing: ADR-0057 step 5's kinds on their own, no session: SegmentGauge
//          (cells, rise, lit up to the value, the peak notch), SegmentReadout
//          (digits, the point, overflow, the zones), SparkGauge (the window,
//          the history, the scale), the bar's `vertical` and `marks`, and
//          the held peak on the other kinds; each at two sizes and in each
//          theme, with the danger mark's slot that never moves the readout.

import QtQuick
import QtTest
import App

TestCase {
    id: testCase
    name: "Kinds"
    when: windowShown

    Component { id: segmentsC; SegmentGauge { width: 300; height: 110; from: 0; to: 8000; warnFrom: 5500; dangerFrom: 6800 } }
    Component { id: readoutC;  SegmentReadout { width: 260; height: 90; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: sparkC;    SparkGauge { width: 300; height: 110; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: barC;      BarGauge { width: 300; height: 72; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: ringC;     RingGauge { width: 240; height: 220; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        Theme.theme = "dark"
    }
    function cleanup() { Theme.theme = "dark" }

    function find(item, name) {
        const found = findChild(item, name)
        verify(found, name + " is there")
        return found
    }
    function all(item, name) {
        const out = []
        const walk = (i) => {
            for (let k = 0; k < i.children.length; ++k) {
                if (i.children[k].objectName === name)
                    out.push(i.children[k])
                walk(i.children[k])
            }
        }
        walk(item)
        return out
    }
    function same(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }
    function make(comp, props) {
        const g = createTemporaryObject(comp, testCase.parent, props || {})
        verify(g, "loads")
        return g
    }

    // ---- SegmentGauge

    function test_segmentsCountTheirCells_data() {
        return [{ tag: "default", cells: undefined, count: 24 },
                { tag: "12", cells: 12, count: 12 },
                { tag: "64", cells: 64, count: 64 },
                { tag: "1 reads as 2", cells: 1, count: 2 },
                { tag: "200 reads as 64", cells: 200, count: 64 }]
    }
    function test_segmentsCountTheirCells(data) {
        const g = make(segmentsC, data.cells === undefined ? {} : { cells: data.cells })
        compare(all(g, "cell").length, data.count)
    }

    // Each cell taller than the one before, its height by the curve; a
    // rise of 0 gives cells of one height
    function test_segmentsRiseWithTheirCurve() {
        const g = make(segmentsC, { cells: 10, rise: 1.6 })
        waitForRendering(g)
        const cells = all(g, "cell")
        for (let i = 1; i < cells.length; ++i)
            verify(cells[i].height > cells[i - 1].height, "cell " + i + " is taller")
        verify(cells[0].height < cells[9].height * 0.3, "the first is low")
        g.rise = 0
        waitForRendering(g)
        for (const c of all(g, "cell"))
            fuzzyCompare(c.height, all(g, "cell")[0].height, 0.01, "one height")
        g.rise = 9
        compare(g.curve, 4, "a rise past 4 reads as 4")
    }

    // Lit up to the value: the cells below it full, those above empty
    function test_segmentsLightUpToTheValue() {
        const g = make(segmentsC, { cells: 8 })
        g.hasValue = true
        g.value = 4000   // half of 0 to 8000
        tryVerify(() => Math.abs(g.fraction - 0.5) < 0.001, 2000, "the value settles")
        const fills = all(g, "cell").map(c => c.fill)
        compare(fills.slice(0, 4).join(","), "1,1,1,1")
        compare(fills.slice(4).join(","), "0,0,0,0")
    }

    // The danger cells are outlined, the others not; each cell takes its
    // zone's color
    function test_segmentsOutlineTheirDangerCells() {
        const g = make(segmentsC, { cells: 16 })
        const cells = all(g, "cell")
        let danger = 0
        for (const c of cells) {
            compare(c.border.width, c.inDangerZone ? 2 : 0)
            if (c.inDangerZone) {
                ++danger
                same(c.lit, Theme.gaugeDanger, "a danger cell")
            }
        }
        verify(danger > 0 && danger < cells.length, "some, not all: " + danger)
        same(cells[0].lit, Theme.gaugeValue, "the first cell is in the normal zone")
    }

    // A held peak shows as a notch over its cell, only when asked for
    function test_segmentsNotchTheirPeak() {
        const g = make(segmentsC, { cells: 8 })
        g.hasValue = true
        g.value = 6000
        g.windowHigh = 6000
        const notch = find(g, "peakNotch")
        verify(!notch.visible, "no peak asked for")
        g.showPeak = true
        tryVerify(() => !isNaN(g.peakValue), 2000, "a peak is held")
        verify(notch.visible, "the notch shows")
        compare(notch.at, 6, "over the cell the peak reached (6000 of 8000 in 8 cells)")
        g.resetPeaks()
        verify(!notch.visible, "Reset peaks clears it")
    }

    // ---- SegmentReadout

    function shownText(g) { return g.shown.map(e => e.ch + (e.dp ? "." : "")).join("") }

    function test_digitsShowTheValueRightAligned_data() {
        return [{ tag: "an integer", v: 5, d: 0, n: 4, text: "   5" },
                { tag: "a decimal", v: 123.4, d: 1, n: 4, text: "123.4" },
                { tag: "its leading blanks", v: 7.5, d: 1, n: 4, text: "  7.5" },
                { tag: "a negative", v: -5, d: 0, n: 4, text: "  -5" },
                { tag: "too long: dashes", v: 99999, d: 0, n: 4, text: "----" },
                { tag: "one digit", v: 7, d: 0, n: 1, text: "7" },
                { tag: "eight", v: 12345678, d: 0, n: 8, text: "12345678" }]
    }
    function test_digitsShowTheValueRightAligned(data) {
        const g = make(readoutC, { digits: data.n, decimals: data.d })
        g.hasValue = true
        g.value = data.v
        compare(shownText(g), data.text)
        compare(all(g, "digit").length, data.n)
    }

    function test_digitsAreBlankWithoutAValue() {
        const g = make(readoutC, { digits: 3 })
        compare(shownText(g), "   ")
        g.hasValue = true
        g.value = 42
        compare(shownText(g), " 42")
        g.hasValue = false
        compare(shownText(g), "   ")
    }

    function test_digitsClampTheirCount() {
        compare(make(readoutC, { digits: 0 }).count, 1)
        compare(make(readoutC, { digits: 12 }).count, 8)
    }

    // A digit lights its own segments (a to g): an eight all seven, a one
    // only b and c; the others stay faint
    function test_aDigitLightsItsSegments() {
        const g = make(readoutC, { digits: 2 })
        g.hasValue = true
        g.value = 18
        const digits = all(g, "digit")
        const segs = (d) => d.children.filter(c => c.name !== undefined)
        const lit = (d) => segs(d).filter(s => s.opacity === 1.0).map(s => s.name).sort().join("")
        compare(lit(digits[0]), "bc", "a one")
        compare(lit(digits[1]), "abcdefg", "an eight")
        for (const s of segs(digits[0]).filter(s => "bc".indexOf(s.name) < 0))
            verify(s.opacity > 0 && s.opacity < 0.2, "the unlit " + s.name + " is faint")
    }

    // Every character lights the segments a seven-segment display does
    function test_everyCharacterLightsItsSegments_data() {
        return [{ ch: "0", segs: "abcdef" }, { ch: "1", segs: "bc" }, { ch: "2", segs: "abdeg" },
                { ch: "3", segs: "abcdg" }, { ch: "4", segs: "bcfg" }, { ch: "5", segs: "acdfg" },
                { ch: "6", segs: "acdefg" }, { ch: "7", segs: "abc" }, { ch: "8", segs: "abcdefg" },
                { ch: "9", segs: "abcdfg" }].map(r => ({ tag: r.ch, ch: r.ch, segs: r.segs }))
    }
    function test_everyCharacterLightsItsSegments(data) {
        const g = make(readoutC, { digits: 1 })
        g.hasValue = true
        g.value = Number(data.ch)
        const digit = all(g, "digit")[0]
        const lit = digit.children.filter(c => c.name !== undefined && c.opacity === 1.0)
                         .map(c => c.name).sort().join("")
        compare(lit, data.segs)
    }

    function test_aNegativeLightsTheMiddleBar() {
        const g = make(readoutC, { digits: 2 })
        g.hasValue = true
        g.value = -3
        const first = all(g, "digit")[0]
        compare(first.children.filter(c => c.name !== undefined && c.opacity === 1.0)
                     .map(c => c.name).join(""), "g")
    }

    function test_theDecimalPointFollowsItsDigit() {
        const g = make(readoutC, { digits: 3, decimals: 1 })
        g.hasValue = true
        g.value = 12.5
        const points = all(g, "point").map(p => p.opacity)
        compare(points.join(","), "0,1,0", "after the second digit")
    }

    // The digits take the zones' colors, in every theme
    function test_theDigitsTakeTheZonesColors_data() {
        return Theme.themeNames.map(n => ({ tag: n, name: n }))
    }
    function test_theDigitsTakeTheZonesColors(data) {
        Theme.theme = data.name
        const g = make(readoutC, { digits: 3 })
        g.hasValue = true
        g.value = 30
        same(g.digitColor, Theme.gaugeValue, "normal")
        g.value = 70
        same(g.digitColor, Theme.gaugeWarn, "warm")
        g.value = 90
        same(g.digitColor, Theme.gaugeDanger, "danger")
        g.hasValue = false
        same(g.digitColor, Theme.textDim, "no value")
    }

    // ---- SparkGauge

    function test_theWindowIsClampedAndSizesTheHistory() {
        compare(make(sparkC, { windowS: 5 }).span, 10)
        compare(make(sparkC, { windowS: 30 }).historyLength, 300)
        const g = make(sparkC, { windowS: 500 })
        compare(g.span, 120)
        compare(g.historyLength, 1200, "at 10 Hz 1,200 points at most")
    }

    function test_theHistoryKeepsOneWindow() {
        const g = make(sparkC, { windowS: 10 })
        g.hasValue = true
        for (let i = 0; i < 130; ++i) {
            g.value = i % 50
            g.takeSample()
        }
        compare(g.history.length, 100, "10 s at 10 Hz")
        compare(g.history[99], 129 % 50, "the newest value is at the end")
        compare(g.tracePoints(300, 80).length, 100, "one point a sample")
    }

    // The window's own low and high, padded to 4 % of full scale at least
    function test_theScaleIsTheWindowsLowAndHigh() {
        const g = make(sparkC, { windowS: 10, decimals: 0 })
        g.hasValue = true
        for (const v of [20, 35, 50, 30])
            { g.value = v; g.takeSample() }
        compare(g.extent.lo, 20)
        compare(g.extent.hi, 50)
        compare(find(g, "scaleHigh").text, "50")
        compare(find(g, "scaleLow").text, "20")
        const flat = make(sparkC, { windowS: 10, decimals: 1 })
        flat.hasValue = true
        flat.value = 40
        flat.takeSample()
        fuzzyCompare(flat.extent.hi - flat.extent.lo, 4, 0.001, "padded to 4 % of 0 to 100")
    }

    // It samples only while shown
    function test_theTraceSamplesOnlyWhileShown() {
        const g = make(sparkC, {})
        g.hasValue = true
        g.value = 10
        tryVerify(() => g.history.length > 2, 2000, "samples while shown")
        g.visible = false
        const n = g.history.length
        wait(250)
        compare(g.history.length, n, "none while hidden")
    }

    // ---- The danger mark's slot and the readout, on the new kinds

    function test_theReadoutDoesNotMoveOnTheNewKinds_data() {
        return [{ tag: "segments", c: segmentsC, v: 6000, from: 7000, at: 6000 },
                { tag: "sparkline", c: sparkC, v: 80, from: 85, at: 80 }]
    }
    function test_theReadoutDoesNotMoveOnTheNewKinds(data) {
        const g = make(data.c, {})
        g.hasValue = true
        g.value = data.v
        g.unit = "rpm"
        g.dangerFrom = data.from
        waitForRendering(g)
        const readout = find(g, "readout")
        const unit = find(g, "unit")
        const where = () => ({ x: readout.mapToItem(g, 0, 0).x, u: unit.mapToItem(g, 0, 0).x })
        const before = where()
        g.dangerFrom = data.at
        verify(find(g, "dangerMark").opacity === 1, "the mark is up")
        waitForRendering(g)
        const after = where()
        fuzzyCompare(after.x, before.x, 0.5, "the readout's x")
        fuzzyCompare(after.u, before.u, 0.5, "the unit's x")
    }

    // ---- Every new kind, at two sizes and in each theme

    function test_everyNewKindLoadsAtTwoSizesInEachTheme_data() {
        const rows = []
        for (const n of Theme.themeNames)
            for (const k of [{ t: "segments", c: segmentsC }, { t: "seven-segment", c: readoutC },
                             { t: "sparkline", c: sparkC }])
                for (const s of [{ w: 300, h: 110 }, { w: 120, h: 64 }])
                    rows.push({ tag: k.t + " " + s.w + "x" + s.h + " " + n, c: k.c, w: s.w, h: s.h, theme: n })
        return rows
    }
    function test_everyNewKindLoadsAtTwoSizesInEachTheme(data) {
        Theme.theme = data.theme
        const g = make(data.c, { width: data.w, height: data.h })
        g.hasValue = true
        g.value = 70
        g.label = "Engine speed"
        g.unit = "rpm"
        waitForRendering(g)
        same(g.accent, Theme.gaugeValue, "the lit color")
        verify(g.width === data.w && g.height === data.h)
        // Nothing drawn past the gauge's edges
        const past = []
        const walk = (item) => {
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.visible && c.width > 0 && c.height > 0) {
                    const r = c.mapToItem(g, c.width, c.height)
                    if (r.x > g.width + 1)
                        past.push((c.objectName || c.toString()) + " ends at " + r.x.toFixed(0))
                }
                walk(c)
            }
        }
        walk(g)
        compare(past.join("; "), "", "nothing past the right edge at " + data.w + " wide")
    }

    // ---- The bar: vertical and marks; the peak on the bar and the ring

    function test_theBarCanStandUp() {
        const g = make(barC, { vertical: true, width: 100, height: 260 })
        g.hasValue = true
        g.value = 50
        waitForRendering(g)
        const segs = all(g, "segment")
        verify(segs.length > 4, "segments")
        // Stacked bottom to top: the lowest value at the bottom
        const ys = segs.map(s => s.mapToItem(g, 0, 0).y)
        for (let i = 1; i < ys.length; ++i)
            verify(ys[i] < ys[i - 1], "segment " + i + " is above " + (i - 1))
        const xs = new Set(segs.map(s => Math.round(s.mapToItem(g, 0, 0).x)))
        compare(xs.size, 1, "one column")
        verify(find(g, "readout").visible)
    }

    function test_theBarsMarksCanBeLeftOut() {
        const g = make(barC, { marks: true })
        g.scaleMarks = [0, 25, 50, 75, 100]
        waitForRendering(g)
        verify(find(g, "scaleMarks").visible, "marks by default")
        g.marks = false
        verify(!find(g, "scaleMarks").visible, "left out")
    }

    function test_thePeakShowsOnTheBarAndTheRing_data() {
        return [{ tag: "bar", c: barC }, { tag: "ring", c: ringC }]
    }
    function test_thePeakShowsOnTheBarAndTheRing(data) {
        const g = make(data.c, {})
        g.hasValue = true
        g.value = 70
        g.windowHigh = 70
        const mark = find(g, "peakMark")
        verify(!mark.visible, "no peak asked for")
        g.showPeak = true
        tryVerify(() => !isNaN(g.peakValue), 2000, "a peak is held")
        verify(mark.visible, "the mark shows")
        g.resetPeaks()
        verify(!mark.visible, "Reset peaks clears it")
    }
}
