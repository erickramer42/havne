// File/layer: tests/qml/tst_gauges.qml | Qt Quick Test, QML tier
// Testing: the gauges' colors come from Theme's roles (ADR-0057 section 9):
//          switching the theme moves every gauge kind and Home's logo, the
//          zone colors and the danger mark past dangerFrom (and not below
//          it), the danger zone's heavier outline on the bar. No session.

import QtQuick
import QtTest
import App

TestCase {
    id: testCase
    name: "Gauges"
    when: windowShown

    // Each kind with a range whose zones are warm from 60 and past danger
    // from 80, the value set by the case
    Component { id: analogC; AnalogGauge { width: 300; height: 300; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: barC;    BarGauge    { width: 300; height: 72;  from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: digitalC; DigitalGauge { width: 300; height: 120; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: ringC;   RingGauge   { width: 240; height: 220; from: 0; to: 100; warnFrom: 60; dangerFrom: 80 } }
    Component { id: shiftC;  ShiftLightBar { width: 400; height: 40; from: 0; to: 100 } }
    Component { id: homeC;   HomePage    { width: 400; height: 800 } }

    readonly property var kinds: [
        { tag: "analog", c: analogC }, { tag: "bar", c: barC },
        { tag: "digital", c: digitalC }, { tag: "ring", c: ringC }]

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
            for (const name of Theme.themeNames)
                rows.push({ tag: k.tag + ", " + name, kind: k, theme: name })
        return rows
    }
    function test_everyKindTakesItsColorsFromTheme(data) {
        Theme.theme = data.theme
        const g = gaugeOf(data.kind, 30)
        same(g.accent, Theme.gaugeValue, "the lit color")
        same(find(g, "readout").color, Theme.gaugeReadout, "a value in the normal zone")
        g.value = 70
        same(find(g, "readout").color, Theme.gaugeWarn, "past the warning")
        g.value = 90
        same(find(g, "readout").color, Theme.gaugeDanger, "past the danger")
        // The same gauge follows a switch of the theme
        const other = Theme.themeNames[(Theme.themeNames.indexOf(data.theme) + 1) % Theme.themeNames.length]
        Theme.theme = other
        same(g.accent, Theme.gaugeValue, "after the switch to " + other)
        same(find(g, "readout").color, Theme.gaugeDanger, "the danger readout after the switch")
        g.value = 30
        same(find(g, "readout").color, Theme.gaugeReadout, "the normal readout after the switch")
    }

    // The needle's literal is a role now
    function test_theNeedleIsARole() {
        for (const name of Theme.themeNames) {
            Theme.theme = name
            same(gaugeOf(kinds[0], 10).needleColor, Theme.gaugeNeedle, name)
        }
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
        compare(mark.Accessible.name, "Danger")
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

    // Dark renders as it did: the colors the gauges had as literals
    function test_darkRendersAsItDid() {
        Theme.theme = "dark"
        const analog = gaugeOf(kinds[0], 30)
        same(analog.needleColor, "#ff5a5f", "the needle")
        same(analog.accent, Qt.lighter("#6d4aff", 1.3), "the lit color")
        same(analog.zoneColor(70, "red"), "#ffd166", "the warning zone")
        same(analog.zoneColor(90, "red"), "#f87171", "the danger zone")
        same(find(analog, "readout").color, "#e8e8f0", "the readout")
        // The status dot, the low shift lights and Home's logo keep the
        // green
        same(Theme.success, "#4ade80", "success")
        const digital = gaugeOf(kinds[2], 30)
        const dot = find(digital, "statusDot")
        same(dot.color, "#4ade80", "the digital gauge's live dot")
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
        home.visible = false
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
        for (const name of Theme.themeNames) {
            Theme.theme = name
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
        home.visible = false   // the idle animation runs only while Home shows
        const logo = find(home, "homeLogo")
        logo.rpm = 800
        for (const name of Theme.themeNames) {
            Theme.theme = name
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
        compare(g.formatScale(1000), "1,000".replace(",", Qt.locale().groupSeparator))
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

    // The shift lights flash at 3 Hz or under
    function test_theShiftLightsFlashAtThreeHertzOrUnder() {
        const g = createTemporaryObject(shiftC, testCase.parent)
        let flasher = null
        for (const o of g.data)
            if (o.hasOwnProperty("interval")) flasher = o
        verify(flasher, "the flasher timer")
        verify(2 * flasher.interval >= 1000 / 3, "a cycle is two toggles: " + flasher.interval)
    }

    // The danger mark is hidden from assistive technology while it is off
    function test_theDangerMarkIsIgnoredWhileItIsOff() {
        const g = gaugeOf(kinds[0], 10)
        const mark = find(g, "dangerMark")
        compare(mark.Accessible.ignored, true, "value below danger")
        g.value = 90
        tryCompare(mark.Accessible, "ignored", false)
    }
}
