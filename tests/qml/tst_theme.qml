// File/layer: tests/qml/tst_theme.qml | Qt Quick Test, QML tier
// Testing: Theme's link-state color and label for every state the
//          bridge publishes and an unknown one, the chart trace
//          palette, and the stale age as core's. No session.

import QtCore
import QtQuick
import QtTest
import App
TestCase {
    name: "Theme"

    function sameColor(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function test_linkColor_data() {
        return [
            { tag: "ok",           state: "ok",           color: Theme.success },
            { tag: "quiet",        state: "quiet",        color: Theme.warning },
            { tag: "requests",     state: "requests",     color: Theme.warning },
            { tag: "stopped",      state: "stopped",      color: Theme.warning },
            { tag: "disconnected", state: "disconnected", color: Theme.textDim },
            { tag: "fault",        state: "fault",        color: Theme.danger },
            { tag: "degraded",     state: "degraded",     color: Theme.danger },
            { tag: "unknown",      state: "a later state", color: Theme.danger },
        ]
    }
    function test_linkColor(data) {
        sameColor(Theme.linkColor(data.state), data.color, data.state)
    }

    function test_linkLabel_data() {
        return [
            { tag: "ok",           state: "ok",           label: "Session active" },
            { tag: "quiet",        state: "quiet",        label: "Link quiet" },
            { tag: "requests",     state: "requests",     label: "Requests failing" },
            { tag: "stopped",      state: "stopped",      label: "Replay stopped" },
            { tag: "disconnected", state: "disconnected", label: "No session" },
            { tag: "fault",        state: "fault",        label: "Adapter fault" },
            { tag: "degraded",     state: "degraded",     label: "Link degraded" },
            { tag: "unknown",      state: "a later state", label: "Link degraded" },
        ]
    }
    function test_linkLabel(data) {
        compare(Theme.linkLabel(data.state), data.label)
    }

    // The stale age is core's, through the bridge
    function test_staleMsIsCores() {
        // ECUIO_VALUE_STALE_US, 3 s: compared with the number, since
        // Theme.staleMs is bound to Bridge.valueStaleMs
        compare(Theme.staleMs, 3000)
        compare(Bridge.valueStaleMs, 3000)
    }

    function test_seriesColorsAreDistinct() {
        const colors = Theme.seriesColors
        verify(colors.length > 1)
        for (let i = 0; i < colors.length; ++i) {
            for (let j = i + 1; j < colors.length; ++j)
                verify(!Qt.colorEqual(colors[i], colors[j]),
                       "series colors " + i + " and " + j + " are both " + colors[i])
        }
    }

    // A trace in the danger color would read as an alarm, so no series
    // color is a saturated hue within 20 degrees of it
    function test_seriesColorsLeaveOutTheDangerHue() {
        const danger = Qt.color(Theme.danger).hsvHue
        for (const c of Theme.seriesColors) {
            const color = Qt.color(c)
            if (color.hsvSaturation < 0.2) continue   // grays and near-white
            const d = Math.abs(color.hsvHue - danger)
            const degrees = Math.min(d, 1 - d) * 360
            verify(degrees > 20, c + " is " + degrees.toFixed(0) + "° from the danger color")
        }
    }

    // ---- Themes: Dark (the default), Low glare and Night

    function cleanup() { Theme.theme = "dark" }

    readonly property var roles: [
        "bg", "surface", "elevated", "elevatedLight", "outline", "primary",
        "text", "textDim", "success", "warning", "danger", "info",
        "gaugeFace", "gaugeValue", "gaugeReadout", "gaugeNeedle",
        "gaugeTick", "gaugeWarn", "gaugeDanger"]

    // Dark's dial shades are the dial's old literals (the other themes'
    // derive from their face)
    function test_darkKeepsTheDialsShades() {
        const s = Theme.gaugeShade
        sameColor(s.bezelHi, "#3a3a52", "bezelHi")
        sameColor(s.bezelLo, "#2c2c42", "bezelLo")
        sameColor(s.faceHi, "#25253a", "faceHi")
        sameColor(s.faceLo, "#0e0e15", "faceLo")
        sameColor(s.needleBase, "#2a2a38", "needleBase")
        sameColor(s.hubHi, "#4b4b63", "hubHi")
        sameColor(Theme.gaugeFace, "#1b1b28", "the face")
        Theme.theme = "night"
        verify(!Qt.colorEqual(Theme.gaugeShade.bezelHi, "#3a3a52"), "night has its own")
    }

    function test_themeNames() {
        compare(Theme.themeNames.join(","), "dark,lowGlare,night")
        compare(Theme.theme, "dark", "dark until chosen")
        compare(Theme.clampTheme("night"), "night")
        compare(Theme.clampTheme("nonsense"), "dark", "an unknown name reads as dark")
        compare(Theme.clampTheme(undefined), "dark")
        compare(Theme.themeLabel("lowGlare"), "Low glare")
        compare(Theme.themeLabel("night"), "Night")
        compare(Theme.themeLabel("dark"), "Dark")
    }

    // The default theme is today's palette, unchanged
    function test_darkIsTodaysPalette() {
        sameColor(Theme.bg, "#14141c", "bg")
        sameColor(Theme.surface, "#1e1e2e", "surface")
        sameColor(Theme.text, "#e8e8f0", "text")
        sameColor(Theme.primary, "#6d4aff", "primary")
        sameColor(Theme.warning, "#ffd166", "warning")
        sameColor(Theme.danger, "#f87171", "danger")
        sameColor(Theme.gaugeNeedle, "#ff5a5f", "the analog needle's old literal")
    }

    function test_everyThemeDefinesEveryRole_data() {
        return Theme.themeNames.map(n => ({ tag: n, name: n }))
    }
    function test_everyThemeDefinesEveryRole(data) {
        Theme.theme = data.name
        for (const r of roles) {
            const c = Theme[r]
            verify(c !== undefined && c !== null, r + " is defined in " + data.name)
            verify(Qt.color(c).a === 1, r + " is opaque in " + data.name)
        }
    }

    // Distinct where they must be: the gauge's zones and its face, and
    // the surfaces a panel is read against
    function test_theRolesAreDistinctWhereTheyMustBe_data() {
        return Theme.themeNames.map(n => ({ tag: n, name: n }))
    }
    function test_theRolesAreDistinctWhereTheyMustBe(data) {
        Theme.theme = data.name
        const pairs = [["gaugeValue", "gaugeWarn"], ["gaugeValue", "gaugeDanger"],
                       ["gaugeWarn", "gaugeDanger"], ["gaugeFace", "gaugeReadout"],
                       ["gaugeFace", "gaugeNeedle"], ["gaugeFace", "gaugeTick"],
                       ["bg", "surface"], ["surface", "elevated"], ["bg", "text"],
                       ["text", "textDim"], ["warning", "danger"]]
        for (const [a, b] of pairs)
            verify(!Qt.colorEqual(Theme[a], Theme[b]), a + " and " + b + " are both " + Theme[a]
                   + " in " + data.name)
    }

    // The two night themes are warm: no green or blue in the gauge roles
    // (hue 0 to 60 degrees, red to amber), so they keep a driver's night
    // vision; the cold-zone info color is warm there too
    function test_theNightThemesAreWarm_data() {
        return [{ tag: "lowGlare", name: "lowGlare" }, { tag: "night", name: "night" }]
    }
    function test_theNightThemesAreWarm(data) {
        Theme.theme = data.name
        for (const r of ["primary", "text", "textDim", "success", "warning", "danger", "info",
                         "gaugeValue", "gaugeReadout", "gaugeNeedle", "gaugeTick", "gaugeWarn",
                         "gaugeDanger"]) {
            const h = Qt.color(Theme[r]).hsvHue * 360
            verify(h >= 0 && h <= 60, r + " is " + Theme[r] + ", hue " + h.toFixed(0) + " in " + data.name)
        }
    }

    // WCAG contrast of a text color on a background
    function lum(c) {
        const f = x => x <= 0.04045 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4)
        const k = Qt.color(c)
        return 0.2126 * f(k.r) + 0.7152 * f(k.g) + 0.0722 * f(k.b)
    }
    function contrast(a, b) {
        const x = lum(a), y = lum(b)
        return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
    }

    // Text holds 4.5:1 or more against the surfaces it sits on, in every
    // theme: the readouts against the gauge face and the card, the zone
    // colors a readout takes, and the page text against the page
    function test_textContrastIsAtLeast4_5_data() {
        return Theme.themeNames.map(n => ({ tag: n, name: n }))
    }
    function test_textContrastIsAtLeast4_5(data) {
        Theme.theme = data.name
        const rows = [["text", "bg"], ["text", "surface"], ["text", "elevated"],
                      ["textDim", "bg"], ["textDim", "surface"],
                      ["gaugeReadout", "gaugeFace"], ["gaugeReadout", "surface"],
                      ["gaugeTick", "gaugeFace"], ["gaugeWarn", "gaugeFace"],
                      ["gaugeWarn", "surface"], ["gaugeDanger", "gaugeFace"],
                      ["gaugeDanger", "surface"], ["warning", "bg"], ["danger", "bg"]]
        for (const [fg, bgName] of rows) {
            const c = contrast(Theme[fg], Theme[bgName])
            verify(c >= 4.5, fg + " on " + bgName + " is " + c.toFixed(2) + ":1 in " + data.name)
        }
        // The text on a selected row or a highlighted button (primary): the
        // default theme's own 4.2:1 at least
        const onPrimary = contrast(Theme.text, Theme.primary)
        verify(onPrimary >= 4.2, "text on primary is " + onPrimary.toFixed(2) + ":1 in " + data.name)
        // Control borders and the lit color against what they sit on: 3:1
        for (const [fg, bgName] of [["outline", "bg"], ["gaugeValue", "gaugeFace"],
                                    ["gaugeNeedle", "gaugeFace"]]) {
            const c = contrast(Theme[fg], Theme[bgName])
            verify(c >= 3, fg + " on " + bgName + " is " + c.toFixed(2) + ":1 in " + data.name)
        }
    }

    // The chart traces' palette is the theme's: ten colors a theme, told
    // apart (3:1 against the chart's surface and page, and no two within 38
    // of each other in RGB), warm in the night themes
    function test_theSeriesPaletteIsPerTheme_data() {
        return Theme.themeNames.map(n => ({ tag: n, name: n }))
    }
    function test_theSeriesPaletteIsPerTheme(data) {
        Theme.theme = data.name
        const colors = Theme.seriesColors
        compare(colors.length, 10)
        for (let i = 0; i < colors.length; ++i) {
            const a = Qt.color(colors[i])
            sameColor(Theme.seriesColor(i), colors[i], "row " + i)
            sameColor(Theme.seriesColor(i + 10), colors[i], "row " + (i + 10) + " repeats")
            verify(contrast(colors[i], Theme.bg) >= 3.5,
                   colors[i] + " on bg is " + contrast(colors[i], Theme.bg).toFixed(2) + ":1 in " + data.name)
            verify(contrast(colors[i], Theme.surface) >= 3.5,
                   colors[i] + " on surface is " + contrast(colors[i], Theme.surface).toFixed(2) + ":1 in " + data.name)
            if (data.name !== "dark")
                verify(a.hsvHue * 360 <= 60, colors[i] + " is warm in " + data.name)
            for (let j = i + 1; j < colors.length; ++j) {
                const b = Qt.color(colors[j])
                const d = 255 * Math.sqrt(Math.pow(a.r - b.r, 2) + Math.pow(a.g - b.g, 2)
                                          + Math.pow(a.b - b.b, 2))
                verify(d >= 38, colors[i] + " and " + colors[j] + " are " + d.toFixed(0)
                       + " apart in " + data.name)
            }
        }
    }

    // Dark's trace colors are the ones it always had
    function test_darkKeepsItsTraceColors() {
        const was = ["#4aa3ff", "#31d08c", "#5dd9d0", "#f0e442", "#e39ac9", "#ffa94d",
                     "#f2f2f2", "#b08cff", "#a3e635", "#e879f9"]
        for (let i = 0; i < was.length; ++i)
            sameColor(Theme.seriesColor(i), was[i], "row " + i)
    }

    // A name that is not a theme reads as dark everywhere: the colors and
    // the one `active` name the Display buttons highlight by
    function test_anUnknownNameReadsAsDark() {
        Theme.theme = "nonsense"
        compare(Theme.active, "dark")
        sameColor(Theme.bg, "#14141c", "bg")
        sameColor(Theme.gaugeValue, Qt.lighter("#6d4aff", 1.3), "gaugeValue")
        Theme.theme = "night"
        compare(Theme.active, "night")
    }

    Component {
        id: storeComponent
        Settings { category: "ui"; property string theme: "unset" }
    }
    // The stored name, as a window made later would read it
    function stored() {
        const reader = storeComponent.createObject(null)
        const name = reader.theme
        reader.destroy()
        return name
    }

    // The chosen theme is written to the `ui` settings as it changes, and
    // Theme reads it when it is made (before the first frame)
    function test_theThemeIsKept() {
        Theme.theme = "night"
        tryVerify(() => stored() === "night", 2000, "stored")
        Theme.theme = "lowGlare"
        tryVerify(() => stored() === "lowGlare", 2000, "stored again")
        Theme.theme = "dark"
        tryVerify(() => stored() === "dark", 2000, "and back")
    }

    // Switching the theme moves the roles: a binding on a role follows
    function test_switchingMovesTheRoles() {
        const dark = Qt.color(Theme.bg)
        Theme.theme = "night"
        verify(!Qt.colorEqual(Theme.bg, dark), "bg moved")
        sameColor(Theme.gaugeValue, "#ff5a4a", "night's lit color")
        Theme.theme = "lowGlare"
        sameColor(Theme.gaugeValue, "#f0a53a", "low glare's lit color")
        Theme.theme = "dark"
        sameColor(Theme.bg, dark, "back to dark")
    }
}
