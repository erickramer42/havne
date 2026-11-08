// File/layer: tests/qml/tst_kinds.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.kinds

import QtQuick
import QtTest
import App

TestCase {
    id: testCase
    name: "Kinds"

    // A palette by its name: Dark is Night off, the others Night on in a style
    function usePalette(name) {
        Theme.night = name === "dark" ? "off" : "on"
        if (name !== "dark")
            Theme.nightStyle = name === "nightRed" ? "red" : "amber"
    }
    when: windowShown

    Component { id: segmentsC; SegmentGauge { width: 300; height: 110; from: 0; to: 8000; warnFrom: 5500; dangerFrom: 6800 } }
    Component { id: readoutC;  SegmentReadout { width: 260; height: 90; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: sparkC;    SparkGauge { width: 300; height: 110; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: barC;      BarGauge { width: 300; height: 72; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: ringC;     RingGauge { width: 240; height: 220; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        usePalette("dark")
    }
    function cleanup() { usePalette("dark") }

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
        const g = make(segmentsC, data.cells === undefined ? { width: 600 } : { cells: data.cells, width: 600 })
        compare(all(g, "cell").length, data.count)
    }

    // A card too narrow for its cells draws what fits at a pitch of 3 px
    // and never a collapsed row: 64 cells on a phone's 120 px
    function test_aNarrowCardDrawsTheCellsThatFit() {
        const g = make(segmentsC, { cells: 64, width: 120, height: 64 })
        waitForRendering(g)
        const cells = all(g, "cell")
        compare(cells.length, g.drawn)
        verify(g.drawn < 64 && g.drawn >= 2, "fewer than asked: " + g.drawn)
        for (const c of cells)
            verify(c.width >= 2, "a cell is " + c.width + " wide")
        const last = cells[cells.length - 1]
        verify(last.x + last.width <= g.width + 0.5, "the row ends inside the card")
        compare(make(segmentsC, { cells: 24, width: 300 }).drawn, 24, "room enough: all of them")
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
        g.value = 4500   // 0.5625 of 0 to 8000: four cells and half of the fifth
        tryVerify(() => Math.abs(g.fraction - 0.5625) < 0.001, 2000, "the value settles")
        const fills = all(g, "cell").map(c => c.fill)
        compare(fills.slice(0, 4).join(","), "1,1,1,1")
        fuzzyCompare(fills[4], 0.5, 0.01, "the leading cell fades in")
        compare(fills.slice(5).join(","), "0,0,0")
        // Out of range: clamped, all lit or none
        g.value = 9000
        tryVerify(() => all(g, "cell").every(c => c.fill === 1), 2000, "past the top")
        g.value = -50
        tryVerify(() => all(g, "cell").every(c => c.fill === 0), 2000, "below the bottom")
    }

    // The danger cells are outlined, the others not; each cell takes its
    // zone's color
    function test_segmentsOutlineTheirDangerCells() {
        const g = make(segmentsC, { cells: 16, dangerFrom: 6700 })
        const cells = all(g, "cell")
        // A cell is in the danger zone when its middle is: (i + 0.5) / 16 of
        // 8000 is 6700 or more from cell 13 on
        const dangerous = cells.map((c, i) => c.inDangerZone ? i : -1).filter(i => i >= 0)
        compare(dangerous.join(","), "13,14,15")
        for (let i = 0; i < cells.length; ++i) {
            compare(cells[i].border.width, i >= 13 ? 2 : 0)
            same(cells[i].lit, i >= 13 ? Theme.gaugeDanger : (i * 500 + 250 >= 5500 ? Theme.gaugeWarn : Theme.gaugeValue),
                 "cell " + i)
        }
    }

    // A held peak shows as a notch over its cell, only when asked for
    function test_segmentsNotchTheirPeak() {
        const g = make(segmentsC, { cells: 8 })
        g.hasValue = true
        g.value = 6500   // 6.5 cells in: the cell is the 6th (floor)
        g.windowHigh = 6500
        const notch = find(g, "peakNotch")
        verify(!notch.visible, "no peak asked for")
        g.showPeak = true
        tryVerify(() => !isNaN(g.peakValue), 2000, "a peak is held")
        waitForRendering(g)
        verify(notch.visible, "the notch shows")
        compare(notch.at, 6, "over the cell the peak reached")
        const cell = all(g, "cell")[6]
        fuzzyCompare(notch.x, cell.x, 0.5, "over that cell's column")
        fuzzyCompare(notch.width, cell.width, 0.5, "its width")
        fuzzyCompare(notch.y, cell.y - 6, 0.5, "6 px over its top")
        verify(notch.y >= 0, "inside the card")
        verify(!Qt.colorEqual(notch.color, cell.lit), "not the color of the cell it sits over")
        g.resetPeaks()
        verify(!notch.visible, "Reset peaks clears it")
    }

    // The notch over the tallest cell stays inside the card
    function test_theNotchOverTheTallestCellStaysInsideTheCard() {
        const g = make(segmentsC, { cells: 8, showPeak: true })
        g.hasValue = true
        g.value = 7900
        g.windowHigh = 7900
        tryVerify(() => !isNaN(g.peakValue), 2000, "a peak is held")
        waitForRendering(g)
        const notch = find(g, "peakNotch")
        compare(notch.at, 7)
        verify(notch.y >= 0, "the notch is at " + notch.y)
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

    // Edge values: a minus zero is a zero, a number that doesn't fit or
    // isn't finite is dashes, not a number is blanks
    function test_digitsHandleTheEdgeValues_data() {
        return [{ tag: "-0.4 at no decimals", v: -0.4, d: 0, n: 3, text: "  0" },
                { tag: "-0.5 at one", v: -0.5, d: 1, n: 3, text: "-0.5" },
                { tag: "-100 in 3 digits", v: -100, d: 0, n: 3, text: "---" },
                { tag: "1e21", v: 1e21, d: 0, n: 4, text: "----" },
                { tag: "1e15", v: 1e15, d: 0, n: 8, text: "--------" },
                { tag: "Infinity", v: Infinity, d: 0, n: 4, text: "----" },
                { tag: "-Infinity", v: -Infinity, d: 0, n: 4, text: "----" },
                { tag: "Infinity at eight digits", v: Infinity, d: 0, n: 8, text: "--------" },
                { tag: "1e21 at eight digits", v: 1e21, d: 0, n: 8, text: "--------" },
                { tag: "NaN", v: NaN, d: 0, n: 4, text: "    " }]
    }
    function test_digitsHandleTheEdgeValues(data) {
        const g = make(readoutC, { digits: data.n, decimals: data.d })
        g.hasValue = true
        g.value = data.v
        compare(shownText(g), data.text)
    }

    // The digits take the cell's accent in the normal zone
    function test_theDigitsTakeTheAccent() {
        const g = make(readoutC, { digits: 3, accent: "#00ff00" })
        g.hasValue = true
        g.value = 30
        same(g.digitColor, "#00ff00", "the accent")
        g.value = 70
        same(g.digitColor, Theme.gaugeWarn, "the zones still win")
    }

    // The unit's slot never resizes the digits: the first value, a danger
    // mark and a dropped value leave them as they were
    function test_theDigitsKeepTheirSizeAsTheValueComes() {
        const g = make(readoutC, { digits: 3, unit: "mph", dangerFrom: 85 })
        waitForRendering(g)
        const row = find(g, "digits")
        const before = { h: row.height, x: row.x, u: find(g, "unitBox").x }
        g.hasValue = true
        g.value = 50
        waitForRendering(g)
        compare(row.height, before.h, "the first value")
        g.value = 90
        verify(find(g, "dangerMark").opacity === 1, "the mark is up")
        waitForRendering(g)
        compare(row.height, before.h, "the danger mark")
        fuzzyCompare(find(g, "unitBox").x, before.u, 0.5, "the unit's box")
        g.hasValue = false
        waitForRendering(g)
        compare(row.height, before.h, "no value again")
    }

    // Eight digits on a narrow card end before the unit, inside the card
    function test_eightDigitsFitANarrowCard() {
        const g = make(readoutC, { digits: 8, width: 120, height: 64, unit: "rpm", dangerFrom: 85 })
        g.hasValue = true
        g.value = 12345678
        waitForRendering(g)
        const row = find(g, "digits")
        verify(row.x + row.width <= find(g, "unitBox").x + 0.5,
               "the digits end at " + (row.x + row.width) + ", the unit starts at " + find(g, "unitBox").x)
        verify(find(g, "unitBox").x + find(g, "unitBox").width <= g.width + 0.5, "the unit ends inside")
        verify(row.height >= 8, "and they are not too small to read: " + row.height)
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
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_theDigitsTakeTheZonesColors(data) {
        usePalette(data.name)
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

    // A reset empties the trace (a connect, a change of units), a page
    // that stops being shown starts again, and a value that is not a number
    // is never kept
    function test_theTraceStartsAgain() {
        const g = make(sparkC, { windowS: 10 })
        g.hasValue = true
        g.value = 5
        for (let i = 0; i < 5; ++i) g.takeSample()
        compare(g.history.length, 5)
        g.resetPeaks()
        compare(g.history.length, 0, "Reset peaks empties it")
        for (let i = 0; i < 5; ++i) g.takeSample()
        g.shown = false
        compare(g.history.length, 0, "a page that is no longer shown")
        g.shown = true
        compare(g.history.length, 0, "and back again, still empty")
        g.value = NaN
        g.takeSample()
        g.value = Infinity
        g.takeSample()
        compare(g.history.length, 0, "no sample of a value that is not a number")
        verify(isFinite(g.extent.lo) && isFinite(g.extent.hi), "the scale stays finite")
    }

    function test_theTraceSamplesOnlyWhenItsPageIsShown() {
        const g = make(sparkC, {})
        g.hasValue = true
        g.value = 10
        g.shown = false
        wait(350)
        compare(g.history.length, 0, "a page that is not the one shown takes none")
        g.shown = true
        tryVerify(() => g.history.length > 2, 2000, "and takes them once shown")
    }

    // About one point per 1.5 px, the newest kept, a flat range a midline
    function test_theTraceHasAboutOnePointPer1_5Px() {
        const g = make(sparkC, { windowS: 120 })
        g.hasValue = true
        for (let i = 0; i < 1200; ++i) {
            g.value = 50 + 30 * Math.sin(i / 20)
            g.takeSample()
        }
        const points = g.tracePoints(300, 80)
        verify(points.length <= 300 / 1.5 + 2, points.length + " points for 300 px")
        verify(points.length > 100, "still a trace: " + points.length)
        fuzzyCompare(points[points.length - 1].x, 300, 0.01, "the newest at the right edge")
        for (const pt of points)
            verify(pt.y >= 0 && pt.y <= 80 && isFinite(pt.y), "a point inside the box")
        const flat = make(sparkC, { windowS: 10, from: 50, to: 50 })
        flat.hasValue = true
        flat.value = 50
        flat.takeSample()
        flat.takeSample()
        for (const pt of flat.tracePoints(300, 80))
            compare(pt.y, 40, "a flat range draws a midline")
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
        for (const n of Theme.paletteNames)
            for (const k of [{ t: "segments", c: segmentsC }, { t: "seven-segment", c: readoutC },
                             { t: "sparkline", c: sparkC }])
                for (const s of [{ w: 300, h: 110 }, { w: 120, h: 64 }])
                    rows.push({ tag: k.t + " " + s.w + "x" + s.h + " " + n, c: k.c, w: s.w, h: s.h, theme: n })
        return rows
    }
    function test_everyNewKindLoadsAtTwoSizesInEachTheme(data) {
        usePalette(data.theme)
        const g = make(data.c, { width: data.w, height: data.h })
        g.hasValue = true
        g.value = g.dangerFrom + 1   // in the danger zone: the mark's slot is in use
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
                    if (r.y > g.height + 1)
                        past.push((c.objectName || c.toString()) + " ends below, at " + r.y.toFixed(0))
                }
                walk(c)
            }
        }
        walk(g)
        compare(past.join("; "), "", "nothing past the edges at " + data.w + " x " + data.h)
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
        waitForRendering(g)
        verify(mark.visible, "the mark shows")
        same(mark.color, Theme.gaugeTick, "no zone's color")
        if (data.tag === "bar") {
            const bar = find(g, "bar")
            fuzzyCompare(mark.x + mark.width / 2, bar.x + 0.7 * bar.width, 1, "at 70 % along the bar")
        } else {
            fuzzyCompare(mark.x + mark.width / 2, g.px(g.angleOf(70), g.zoneRadius), 1, "at the peak's angle, x")
            fuzzyCompare(mark.y + mark.height / 2, g.py(g.angleOf(70), g.zoneRadius), 1, "at the peak's angle, y")
        }
        g.resetPeaks()
        verify(!mark.visible, "Reset peaks clears it")
    }

    // Standing up, the peak's mark is as far up as its value; at either end
    // it stays inside the track
    function test_theVerticalBarsPeakMarkIsAsHighAsItsValue() {
        const g = make(barC, { vertical: true, width: 100, height: 260, showPeak: true })
        g.hasValue = true
        g.value = 70
        g.windowHigh = 70
        tryVerify(() => !isNaN(g.peakValue), 2000, "a peak is held")
        waitForRendering(g)
        const bar = find(g, "bar")
        const mark = find(g, "peakMark")
        fuzzyCompare(mark.y + mark.height / 2, bar.y + 0.3 * bar.height, 1, "70 % up")
        g.resetPeaks()
        g.value = 100
        g.windowHigh = 100
        tryVerify(() => g.peakValue === 100, 2000, "a peak at the top")
        waitForRendering(g)
        verify(mark.y >= bar.y - 0.5, "not above the track")
        const flat = make(barC, { showPeak: true })
        flat.hasValue = true
        flat.value = 100
        flat.windowHigh = 100
        tryVerify(() => flat.peakValue === 100, 2000, "a horizontal peak at the end")
        waitForRendering(flat)
        const fm = findChild(flat, "peakMark")
        verify(fm.x + fm.width <= findChild(flat, "bar").x + findChild(flat, "bar").width + 0.5, "not past the track's end")
    }

    // A bar too short for ten segments draws as many as fit, none narrower
    // than the least width
    function test_aShortBarDrawsAsManySegmentsAsFit() {
        const g = make(barC, { vertical: true, width: 100, height: 130 })
        g.hasValue = true
        g.value = 50
        waitForRendering(g)
        const segments = all(g, "segment")
        verify(segments.length >= 2 && segments.length < 10, "fewer than ten: " + segments.length)
        for (const s of segments)
            verify(s.height >= g.segmentWidth - 0.01, "a segment is " + s.height + " long")
        const tall = make(barC, { vertical: true, width: 100, height: 260 })
        tall.hasValue = true
        waitForRendering(tall)
        verify(all(tall, "segment").length >= 10, "ten or more when they fit")
    }

    // The sparkline's trace does not move as its scale labels change width
    function test_theSparklinesTraceStaysPutWhenALabelGainsADigit() {
        const g = make(sparkC, { windowS: 10 })
        const box = find(g, "scaleBox")
        const empty = box.width
        g.history = [9, 9.5]
        compare(box.width, empty, "two short labels")
        g.history = [9, 100]
        compare(box.width, empty, "a three-digit label")
    }
}
