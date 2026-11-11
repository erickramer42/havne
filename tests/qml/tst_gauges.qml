// File/layer: tests/qml/tst_gauges.qml | Qt Quick Test, QML tier
// Testing: the gauges' colors come from Theme's roles (ADR-0057 section 9):
//          switching the theme moves every gauge kind and Home's logo, the
//          zone colors and the danger mark past dangerFrom (and not below
//          it), the danger zone's heavier outline on the bar. No session.

import QtQuick
import QtQuick.Shapes
import QtTest
import App

TestCase {
    id: testCase
    name: "Gauges"

    // A palette by its name: Dark is Night off, the others Night on in a style
    function usePalette(name) {
        const night = Theme.nightPalettes.indexOf(name) >= 0
        Theme.night = night ? "on" : "off"
        if (night)
            Theme.nightStyle = name === "nightRed" ? "red" : "amber"
        else
            Theme.look = name
    }
    when: windowShown

    // Each kind with a range whose zones are warm from 60 and past danger
    // from 80, the value set by the case
    Component { id: analogC; AnalogGauge { width: 300; height: 300; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: barC;    BarGauge    { width: 300; height: 72;  from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: digitalC; DigitalGauge { width: 300; height: 120; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: ringC;   RingGauge   { width: 240; height: 220; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: shiftC;  ShiftLightBar { width: 400; height: 40; from: 0; to: 100 } }
    Component { id: sparkC; SparkGauge { width: 300; height: 120; from: 0; to: 100; value: 50; hasValue: true } }
    Component { id: segmentC; SegmentGauge { width: 300; height: 120; from: 0; to: 100; value: 50; hasValue: true } }
    Component { id: homeC;   HomePage    { width: 400; height: 800; animated: false } }

    readonly property var kinds: [
        { tag: "analog", c: analogC }, { tag: "bar", c: barC },
        { tag: "digital", c: digitalC }, { tag: "ring", c: ringC }]

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

    function same(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function gaugeOf(kind, value) {
        const g = createTemporaryObject(kind.c, testCase.parent)
        verify(g, kind.tag + " loads")
        g.hasValue = true
        g.value = value
        return g
    }

    // A gauge's normal-zone color and readout are Theme's, whichever theme
    function test_everyKindTakesItsColorsFromTheme_data() {
        const rows = []
        for (const k of kinds)
            for (const name of Theme.paletteNames)
                rows.push({ tag: k.tag + ", " + name, kind: k, theme: name })
        return rows
    }
    function test_everyKindTakesItsColorsFromTheme(data) {
        usePalette(data.theme)
        const g = gaugeOf(data.kind, 30)
        same(g.accent, Theme.gaugeValue, "the lit color")
        same(find(g, "readout").color, Theme.gaugeReadout, "a value in the normal zone")
        g.value = 70
        same(find(g, "readout").color, Theme.gaugeWarn, "past the warning")
        g.value = 90
        same(find(g, "readout").color, Theme.gaugeDanger, "past the danger")
        // The same gauge follows a switch of the theme
        const other = Theme.paletteNames[(Theme.paletteNames.indexOf(data.theme) + 1) % Theme.paletteNames.length]
        usePalette(other)
        same(g.accent, Theme.gaugeValue, "after the switch to " + other)
        same(find(g, "readout").color, Theme.gaugeDanger, "the danger readout after the switch")
        g.value = 30
        same(find(g, "readout").color, Theme.gaugeReadout, "the normal readout after the switch")
    }

    // The needle's literal is a role now
    function test_theNeedleIsARole() {
        for (const name of Theme.paletteNames) {
            usePalette(name)
            same(gaugeOf(kinds[0], 10).needleColor, Theme.gaugeNeedle, name)
        }
    }

    // The dial draws on what's behind it: nothing of the dial's size paints
    // a disc under the ticks
    function test_theDialHasNoBezelOrFace() {
        const g = gaugeOf(kinds[0], 30)
        for (let i = 0; i < g.children.length; ++i) {
            const c = g.children[i]
            verify(!(c instanceof Rectangle && c.width >= g.size * 0.9 && c.radius > 0),
                   "a disc of " + c.width + " px at child " + i)
            if (!(c instanceof Shape) || c.width < g.width)
                continue
            for (const p of c.data)
                verify(!(p instanceof ShapePath) || !p.fillGradient,
                       "a filled face in the shape at child " + i)
        }
    }

    // The dial grows into its item and nothing it draws passes the item's
    // edge, at any size: a small dial is held in by the progress arc's
    // minimum width, a large one by the 6 % it may grow. The sizes are
    // worked out by hand: (box - 10) / 0.89 below 176.6 px, else 1.06 * box
    function test_theDialReachesItsItem_data() {
        return [
            { tag: "a cell still sizing up", w: 0, h: 0, size: 0 },
            { tag: "under the floor's own margin", w: 8, h: 8, size: 0 },
            { tag: "100 px", w: 100, h: 100, size: 101.1236 },
            { tag: "130 px", w: 130, h: 130, size: 134.8315 },
            { tag: "130 px high and wide", w: 400, h: 130, size: 134.8315 },
            { tag: "176 px", w: 176, h: 176, size: 186.5169 },
            { tag: "180 px", w: 180, h: 180, size: 190.8 },
            { tag: "300 px", w: 300, h: 300, size: 318 }
        ]
    }
    function test_theDialReachesItsItem(data) {
        const g = createTemporaryObject(analogC, testCase,
                                        { width: data.w, height: data.h, showProgress: true,
                                          value: 100 })
        fuzzyCompare(g.size, data.size, 1e-3, "the drawing's square")
        // The glow's outer edge: the ring's radius and half the 4x stroke,
        // from the drawing's own numbers, at its 2.5 px minimum width
        const edge = g.size / 2 * 0.89 + 2 * Math.max(2.5, g.size * 0.013)
        if (g.size > 0)
            verify(edge <= Math.min(data.w, data.h) / 2 + 1e-6,
                   "the progress glow reaches " + edge + " of " + Math.min(data.w, data.h) / 2)
    }

    // The needle and the hub are flat, in the theme's colors, in every
    // palette; the needle casts no shadow
    function test_theNeedleAndHubAreFlat_data() {
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_theNeedleAndHubAreFlat(data) {
        usePalette(data.name)
        const g = gaugeOf(kinds[0], 30)
        const shapes = Array.from(g.children).filter(c => c instanceof Shape && c.width < g.width)
        compare(shapes.length, 1, "one needle, no shadow")
        const path = Array.from(shapes[0].data).filter(p => p instanceof ShapePath)[0]
        verify(!path.fillGradient, "no gradient on the needle")
        same(path.fillColor, Theme.gaugeNeedle, "the needle's color")
        const hub = findChild(g, "hub")
        verify(!hub.gradient, "no gradient on the hub")
        same(hub.color, Theme.elevatedLight, "the hub")
        same(findChild(g, "hubPin").color, Theme.gaugeNeedle, "the pin")
    }

    // The danger mark's point is cut out in the color the gauge sits on
    function test_theDangerMarksPointIsTheSurface() {
        for (const name of Theme.paletteNames) {
            usePalette(name)
            const mark = findChild(gaugeOf(kinds[0], 99), "dangerMark")
            const point = Array.from(mark.children).filter(c => c.text === "!")[0]
            same(point.color, Theme.surface, name)
        }
    }

    // A trace is its line alone: nothing fills the area under it
    function test_aTraceIsItsLineAlone_data() {
        return [{ tag: "digital", component: digitalC }, { tag: "sparkline", component: sparkC }]
    }
    function test_aTraceIsItsLineAlone(data) {
        const g = createTemporaryObject(data.component, testCase)
        const trace = findChild(g, "trace")
        verify(trace instanceof Shape, "the trace")
        let paths = 0
        for (const p of trace.data) {
            if (!(p instanceof ShapePath))
                continue
            ++paths
            verify(!p.fillGradient, "a gradient fill in " + data.tag)
            compare(p.fillColor.a, 0, "a solid fill in " + data.tag)
        }
        verify(paths > 0, "the trace has its paths")
    }

    // A lit segment is one flat color
    function test_aLitSegmentIsFlat_data() {
        return [{ tag: "bar", make: () => createTemporaryObject(barC, testCase, { value: 50, hasValue: true }) },
                { tag: "cells", make: () => createTemporaryObject(segmentC, testCase) }]
    }
    function test_aLitSegmentIsFlat(data) {
        const g = data.make()
        let segments = 0
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.objectName === "segment" || c.objectName === "cell") {
                    ++segments
                    const fill = c.children[0]
                    verify(!fill.gradient, "a gradient in " + data.tag)
                    same(fill.color, c.lit, "the " + data.tag + " segment's fill")
                }
                walk(c)
            }
        }
        walk(g)
        verify(segments > 0, "the segments were walked")
    }

    // Warning and danger differ by more than hue: a mark beside the readout
    // past dangerFrom, and none below it
    function test_theDangerMarkAppearsPastDangerFrom_data() {
        const rows = []
        for (const k of kinds)
            rows.push({ tag: k.tag, kind: k })
        return rows
    }
    function test_theDangerMarkAppearsPastDangerFrom(data) {
        const g = gaugeOf(data.kind, 30)
        const mark = find(g, "dangerMark")
        // Its slot stays (see test_theReadoutDoesNotMoveAsTheMarkAppears);
        // what appears is its opacity
        const shown = () => mark.visible && mark.opacity === 1
        verify(!shown(), "none in the normal zone")
        g.value = 79.9
        verify(!shown(), "none in the warning zone")
        g.value = 80
        verify(shown(), "at dangerFrom")
        g.value = 100
        verify(shown(), "past it")
        g.hasValue = false
        verify(!shown(), "none without a value")
    }

    // A gauge with no danger zone never shows the mark
    function test_noDangerZoneNoMark() {
        const g = gaugeOf(kinds[2], 99)
        g.dangerFrom = NaN
        verify(!find(g, "dangerMark").visible, "no danger zone: no mark and no slot")
    }

    // The readout and its unit stay where they are as the mark appears:
    // a value hovering at dangerFrom never jitters the line
    function test_theReadoutDoesNotMoveAsTheMarkAppears_data() {
        const rows = []
        for (const k of kinds)
            rows.push({ tag: k.tag, kind: k })
        return rows
    }
    function test_theReadoutDoesNotMoveAsTheMarkAppears(data) {
        const g = gaugeOf(data.kind, 80)
        g.unit = "rpm"
        g.dangerFrom = 85   // the same value, below its danger zone
        waitForRendering(g)
        const readout = find(g, "readout")
        const unit = findChild(g, "unit")
        const where = () => ({ x: readout.mapToItem(g, 0, 0).x, w: readout.width,
                               u: unit ? unit.mapToItem(g, 0, 0).x : 0 })
        const before = where()
        g.dangerFrom = 80   // now at it: the same text
        verify(find(g, "dangerMark").visible && find(g, "dangerMark").opacity === 1, "the mark is up")
        waitForRendering(g)
        const after = where()
        fuzzyCompare(after.x, before.x, 0.5, "the readout's x")
        fuzzyCompare(after.w, before.w, 0.5, "the readout's width")
        fuzzyCompare(after.u, before.u, 0.5, "the unit's x")
    }

    // A Number tile draws no dot: one that is on whenever all is well says
    // nothing (qml.md), so its title runs to the edge, fresh or stale
    function test_aNumberTileHasNoStatusDot_data() {
        return [{ tag: "fresh", stale: false }, { tag: "stale", stale: true }]
    }
    function test_aNumberTileHasNoStatusDot(data) {
        const g = gaugeOf(kinds[2], 30)
        g.label = "Air flow"
        g.stale = data.stale
        const dots = []
        let title = null
        const walk = (item) => {
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.visible && c.radius !== undefined && c.width > 0 && c.width <= 12
                        && c.radius >= c.width / 2)
                    dots.push(c)
                if (c.text === "Air flow")
                    title = c
                walk(c)
            }
        }
        walk(g)
        compare(dots.length, 0, "no round mark under 12 px")
        verify(title, "the title is there")
        fuzzyCompare(title.mapToItem(g, title.width, 0).x, g.width, 0.5, "the title runs to the edge")
    }

    // Dark renders the graphite look: the zone colors and the needle are the ones the gauges had, the lit color and readout the text color
    function test_darkRendersAsItDid() {
        usePalette("dark")
        const analog = gaugeOf(kinds[0], 30)
        same(analog.needleColor, "#ff5a5f", "the needle")
        same(analog.accent, "#ebebee", "the lit color")
        same(analog.zoneColor(70, "red"), "#ffd166", "the warning zone")
        same(analog.zoneColor(90, "red"), "#f87171", "the danger zone")
        same(find(analog, "readout").color, "#ebebee", "the readout")
        // The low shift lights and Home's logo keep the green
        same(Theme.success, "#4ade80", "success")
        const bar = createTemporaryObject(shiftC, testCase.parent)
        const hues = []
        const walk = (item) => {
            for (let i = 0; i < item.children.length; ++i) {
                if (item.children[i].objectName === "slot")
                    hues.push(item.children[i])
                walk(item.children[i])
            }
        }
        walk(bar)
        same(hues[0].hue, "#4ade80", "the first shift light")
        same(hues[8].hue, "#ffd166", "the middle")
        same(hues[14].hue, "#f87171", "the last")
        const home = createTemporaryObject(homeC, testCase.parent)
        const logo = find(home, "homeLogo")
        logo.rpm = 800
        same(logo.litColor, "#4ade80", "the logo's idle color")
    }

    // The bar's danger zone is outlined, the rest is not
    function test_theBarOutlinesItsDangerZone() {
        const g = gaugeOf(kinds[1], 50)
        const segs = []
        const walk = (item) => {
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.objectName === "segment")
                    segs.push(c)
                walk(c)
            }
        }
        walk(g)
        verify(segs.length > 4, "the segments are there")
        let outlined = 0
        for (const s of segs) {
            const dangerous = s.inDangerZone
            compare(s.border.width, dangerous ? 2 : 0)
            if (dangerous) {
                ++outlined
                verify(s.border.color.a > 0)
            }
        }
        verify(outlined > 0 && outlined < segs.length, "some, not all: " + outlined + " of " + segs.length)
    }

    // The shift lights take the roles: the low LEDs the theme's success, the
    // middle the warning, the last the danger
    function test_theShiftLightsTakeTheRoles() {
        for (const name of Theme.paletteNames) {
            usePalette(name)
            const bar = createTemporaryObject(shiftC, testCase.parent)
            verify(bar, "the strip loads")
            const slots = []
            const walk = (item) => {
                for (let i = 0; i < item.children.length; ++i) {
                    const c = item.children[i]
                    if (c.objectName === "slot")
                        slots.push(c)
                    walk(c)
                }
            }
            walk(bar)
            compare(slots.length, 15)
            same(slots[0].hue, Theme.success, name + " first")
            same(slots[8].hue, Theme.gaugeWarn, name + " middle")
            same(slots[14].hue, Theme.gaugeDanger, name + " last")
            bar.destroy()
        }
    }

    // Home's logo follows the theme
    function test_homesLogoFollowsTheTheme() {
        const home = createTemporaryObject(homeC, testCase.parent)
        verify(home, "Home loads")
        const logo = find(home, "homeLogo")
        logo.rpm = 800
        for (const name of Theme.paletteNames) {
            usePalette(name)
            same(logo.litColor, Theme.success, name + " idle")
            same(logo.color, Theme.surface, name + " panel")
        }
        logo.rpm = 5000
        same(logo.litColor, Theme.gaugeWarn, "warm")
        logo.rpm = 7000
        same(logo.litColor, Theme.gaugeDanger, "redline")
    }

    // A scale numeral keeps the decimals its value needs
    function test_scaleNumeralsKeepTheirDecimals() {
        const g = createTemporaryObject(analogC, testCase.parent,
                                        { from: 0, to: 5, majorStep: 0.5, minorStep: 0.25 })
        const texts = []
        for (let i = 0; i <= 10; ++i) {
            const t = g.formatScale(i * 0.5)
            texts.push(t)
        }
        compare(new Set(texts).size, 11, "eleven distinct numerals: " + texts)
        // The digits alone: the C locale (a CI runner's) writes no group
        // separator, though it names one
        compare(g.formatScale(1000).split(Qt.locale().groupSeparator).join(""), "1000")
        compare(g.formatScale(43.5), "43" + Qt.locale().decimalPoint + "5")
        compare(g.formatScale(95), "95")
    }

    // A step that does not divide the range draws no tick past its end
    function test_aStepThatDoesNotDivideTheRangeStopsAtTheEnd() {
        const g = createTemporaryObject(analogC, testCase.parent,
                                        { from: 0, to: 7500, majorStep: 1000, minorStep: 500 })
        compare(g.majorCount, 8, "0 to 7000")
        compare(g.minorCount, 16, "0 to 7500")
        g.to = 8000
        compare(g.majorCount, 9, "an exact multiple keeps its last tick")
    }

    // Past the shift point every light flashes in its own stage's color
    function test_theLitShiftLightsKeepTheirStagesColor() {
        const bar = createTemporaryObject(shiftC, testCase.parent)
        const slots = []
        const walk = (item) => {
            for (let i = 0; i < item.children.length; ++i) {
                if (item.children[i].objectName === "slot")
                    slots.push(item.children[i])
                walk(item.children[i])
            }
        }
        walk(bar)
        verify(slots.length > 0, "the lights")
        bar.hasValue = true
        bar.value = 120
        tryVerify(() => bar.shiftNow, 3000, "past the shift point")
        for (const s of slots)
            verify(Qt.colorEqual(s.glow, s.hue), "a light in its stage's color: " + s.glow + " for " + s.hue)
        verify(!Qt.colorEqual(slots[0].glow, Theme.info), "never the cold blue")
    }

    // The shift lights flash at 3 Hz or under
    function test_theShiftLightsFlashAtThreeHertzOrUnder() {
        const g = createTemporaryObject(shiftC, testCase.parent)
        let flasher = null
        for (const o of g.data)
            if (o.hasOwnProperty("interval")) flasher = o
        verify(flasher, "the flasher timer")
        verify(2 * flasher.interval >= 1000 / 3, "a cycle is two toggles: " + flasher.interval)
    }

    // The gauge's own name says danger; the mark is never a node of its own
    function test_theDangerIsInTheGaugesName() {
        const g = gaugeOf(kinds[0], 10)
        g.label = "Oil"
        g.unit = "deg"
        const mark = find(g, "dangerMark")
        compare(g.Accessible.name, "Oil: 10 deg")
        compare(mark.Accessible.ignored, true, "value below danger")
        g.value = 90
        compare(g.Accessible.name, "Oil: 90 deg, danger")
        compare(mark.Accessible.ignored, true, "past danger too")
        g.hasValue = false
        g.note = "not in profile"
        compare(g.Accessible.name, "Oil: not in profile")
    }

    // The Number's footer: MIN the lowest value held, MAX the highest, a
    // dash before the first value and after Reset peaks
    function test_theNumbersFooterHoldsItsLowAndPeak() {
        const g = createTemporaryObject(digitalC, testCase.parent)
        const low = find(g, "lowHold")
        const peak = find(g, "peakHold")
        compare(low.text, "MIN  \u2014", "no value yet")
        compare(peak.text, "MAX  \u2014", "no value yet")
        g.value = 30
        g.hasValue = true
        g.value = 70
        g.value = 10
        compare(low.text, "MIN  " + g.format(10))
        compare(peak.text, "MAX  " + g.format(70))
        g.resetPeaks()
        compare(low.text, "MIN  \u2014", "after Reset peaks")
        compare(peak.text, "MAX  \u2014", "after Reset peaks")
    }

    // The Number's trace keeps no value that isn't a number, as the
    // sparkline's doesn't, so one never blanks the whole trace
    function test_theNumbersTraceKeepsOnlyNumbers() {
        const g = createTemporaryObject(digitalC, testCase.parent)
        const sampler = find(g, "sampler")
        g.hasValue = true
        g.value = 40
        sampler.triggered()
        compare(g.history.length, 1)
        g.value = Infinity
        sampler.triggered()
        g.value = NaN
        sampler.triggered()
        compare(g.history.length, 1, "neither kept")
        compare(g.history[0], 40)
    }
}
