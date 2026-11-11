// File/layer: tests/qml/tst_theme.qml | Qt Quick Test, QML tier
// Testing: Theme's link-state color and label for every state the
//          bridge publishes and an unknown one, the chart trace
//          palette, the stale age as core's, and the window's color
//          scheme following the look. No session.

import QtCore
import QtQuick
import QtTest
import App
import AppTest
TestCase {
    id: testCase
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

    // The one word a tight header shows in the pill
    function test_linkShortLabel() {
        const words = { ok: "Live", fault: "Fault", requests: "Failing", quiet: "Quiet",
                        stopped: "Stopped", disconnected: "No session", degraded: "Degraded",
                        "a later state": "Degraded" }
        for (const state of Object.keys(words))
            compare(Theme.linkShortLabel(state), words[state], state)
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

    // A trace in the danger color would read as an alarm, so in a look no
    // series color is a saturated hue within 20 degrees of it (the night
    // palettes are warm by design)
    function test_seriesColorsLeaveOutTheDangerHue_data() {
        return Theme.lookNames.map(n => ({ tag: n, name: n }))
    }
    function test_seriesColorsLeaveOutTheDangerHue(data) {
        usePalette(data.name)
        const danger = Qt.color(Theme.danger).hsvHue
        for (const c of Theme.seriesColors) {
            const color = Qt.color(c)
            if (color.hsvSaturation < 0.2) continue   // grays and near-white
            const d = Math.abs(color.hsvHue - danger)
            const degrees = Math.min(d, 1 - d) * 360
            verify(degrees > 20, c + " is " + degrees.toFixed(0) + "° from the danger color in " + data.name)
        }
    }

    // ---- Palettes: the looks Dark (the default) and Daylight, and Night's two

    // A palette by its name: a look is Night off and that look, a night
    // palette Night on in its style
    function usePalette(name) {
        const night = Theme.nightPalettes.indexOf(name) >= 0
        Theme.night = night ? "on" : "off"
        if (night)
            Theme.nightStyle = name === "nightRed" ? "red" : "amber"
        else
            Theme.look = name
    }
    function cleanup() {
        Theme.stateSource = Qt.application
        Theme.now = () => new Date()
        Theme.night = "off"
        Theme.nightStyle = "amber"
        Theme.nightFrom = "19:00"
        Theme.nightUntil = "07:00"
        Theme.look = "dark"
    }

    readonly property var roles: [
        "bg", "surface", "elevated", "elevatedLight", "outline", "primary", "textOnPrimary",
        "hairline", "text", "textDim", "success", "warning", "danger", "info",
        "gaugeValue", "gaugeReadout", "gaugeNeedle",
        "gaugeTick", "gaugeWarn", "gaugeDanger"]

    function test_paletteNamesAndClamps() {
        compare(Theme.paletteNames.join(","), "dark,nightAmber,nightRed,daylight")
        compare(Theme.lookNames.join(","), "dark,daylight")
        compare(Theme.nightPalettes.join(","), "nightAmber,nightRed")
        compare(Theme.look, "dark", "dark until chosen")
        compare(Theme.night, "off")
        compare(Theme.nightStyle, "amber")
        compare(Theme.active, "dark")
        compare(Theme.clampNight("schedule"), "schedule")
        compare(Theme.clampNight("nonsense"), "off", "an unknown mode reads as off")
        compare(Theme.clampNight(undefined), "off")
        compare(Theme.clampStyle("red"), "red")
        compare(Theme.clampStyle("nonsense"), "amber", "an unknown style reads as amber")
        compare(Theme.clampLook("nonsense"), "dark", "an unknown look reads as dark")
        compare(Theme.clampLook("daylight"), "daylight")
        compare(Theme.clampLook("nightAmber"), "dark", "a night palette is not a look")
    }

    // The default theme is the near-neutral graphite: the accent is the text
    // color as a fill, and the label on it is the page color
    function test_darkIsTheGraphitePalette() {
        sameColor(Theme.bg, "#141416", "bg")
        sameColor(Theme.surface, "#1b1b1e", "surface")
        sameColor(Theme.elevated, "#26262b", "elevated")
        sameColor(Theme.elevatedLight, "#323238", "elevatedLight")
        sameColor(Theme.outline, "#65656f", "outline")
        sameColor(Theme.text, "#ebebee", "text")
        sameColor(Theme.textDim, "#9a9aa3", "textDim")
        sameColor(Theme.primary, Theme.text, "primary")
        sameColor(Theme.textOnPrimary, Theme.bg, "textOnPrimary")
        sameColor(Theme.gaugeValue, Theme.text, "gaugeValue")
        sameColor(Theme.warning, "#ffd166", "warning")
        sameColor(Theme.danger, "#f87171", "danger")
        sameColor(Theme.gaugeNeedle, "#ff5a5f", "the analog needle")
    }

    // Daylight is Paper: near-black on off-white, the panel white, the
    // accent the text color as a fill, the zones the warning and danger
    function test_daylightIsThePaperPalette() {
        usePalette("daylight")
        compare(Theme.active, "daylight")
        const want = {
            bg: "#f3f3f1", surface: "#ffffff", elevated: "#e4e4e8", elevatedLight: "#d3d3da",
            outline: "#5f5f6a", text: "#141416", textDim: "#5b5b66",
            success: "#1b7a3e", warning: "#8c5a00", danger: "#b9262b", info: "#2458b8",
            gaugeValue: "#141416", gaugeReadout: "#141416", gaugeNeedle: "#d2232a",
            gaugeTick: "#141416"
        }
        for (const r in want)
            sameColor(Theme[r], want[r], r)
        sameColor(Theme.primary, Theme.text, "primary")
        sameColor(Theme.textOnPrimary, Theme.bg, "textOnPrimary")
        sameColor(Theme.gaugeWarn, Theme.warning, "gaugeWarn")
        sameColor(Theme.gaugeDanger, Theme.danger, "gaugeDanger")
        const series = ["#1f5fc8", "#1e7d3f", "#0f7f84", "#8a6d00", "#8a3fa8", "#b85c00",
                        "#3c3c44", "#5b48c9", "#4f7a0a", "#a81f8c"]
        for (let i = 0; i < series.length; ++i)
            sameColor(Theme.seriesColor(i), series[i], "row " + i)
    }

    // The label on a primary fill: the page color in a look, the text color
    // at night; a hairline is the outline at 55 % over the surface; two radii
    function test_textOnPrimaryAndTheHairlinePerPalette_data() {
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_textOnPrimaryAndTheHairlinePerPalette(data) {
        usePalette(data.name)
        const night = Theme.nightPalettes.indexOf(data.name) >= 0
        sameColor(Theme.textOnPrimary, night ? Theme.text : Theme.bg, "textOnPrimary")
        const o = Qt.color(Theme.outline), s = Qt.color(Theme.surface)
        const mix = (a, b) => 0.55 * a + 0.45 * b
        const h = Qt.color(Theme.hairline)
        verify(Math.abs(h.r - mix(o.r, s.r)) < 0.01 && Math.abs(h.g - mix(o.g, s.g)) < 0.01
               && Math.abs(h.b - mix(o.b, s.b)) < 0.01, "hairline is " + Theme.hairline)
    }
    function test_theRadiiAreFourAndSix() {
        compare(Theme.radius, 4)
        compare(Theme.radiusCard, 6)
    }

    function test_everyThemeDefinesEveryRole_data() {
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_everyThemeDefinesEveryRole(data) {
        usePalette(data.name)
        for (const r of roles) {
            const c = Theme[r]
            verify(c !== undefined && c !== null, r + " is defined in " + data.name)
            verify(Qt.color(c).a === 1, r + " is opaque in " + data.name)
        }
    }

    // Distinct where they must be: the gauge's zones and what it sits on,
    // and the surfaces a panel is read against
    function test_theRolesAreDistinctWhereTheyMustBe_data() {
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_theRolesAreDistinctWhereTheyMustBe(data) {
        usePalette(data.name)
        const pairs = [["gaugeValue", "gaugeWarn"], ["gaugeValue", "gaugeDanger"],
                       ["gaugeWarn", "gaugeDanger"], ["surface", "gaugeReadout"],
                       ["surface", "gaugeNeedle"], ["surface", "gaugeTick"],
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
        return [{ tag: "nightAmber", name: "nightAmber" }, { tag: "nightRed", name: "nightRed" }]
    }
    function test_theNightThemesAreWarm(data) {
        usePalette(data.name)
        for (const r of ["primary", "textOnPrimary", "hairline", "text", "textDim", "success", "warning", "danger", "info",
                         "gaugeValue", "gaugeReadout", "gaugeNeedle", "gaugeTick", "gaugeWarn",
                         "gaugeDanger"]) {
            const h = Qt.color(Theme[r]).hsvHue * 360
            verify(h >= 0 && h <= 60, r + " is " + Theme[r] + ", hue " + h.toFixed(0) + " in " + data.name)
        }
    }

    // Night Red has one red: danger alone is full red, and success is a
    // quiet warm tint apart from the dim text, so Live and No session
    // differ at a glance
    function test_nightRedKeepsFullRedForDangerAlone() {
        usePalette("nightRed")
        verify(Qt.colorEqual(Theme.danger, "#ff3030"), "danger is full red: " + Theme.danger)
        verify(!Qt.colorEqual(Theme.success, Theme.danger), "success is not danger's red")
        verify(!Qt.colorEqual(Theme.success, Theme.textDim), "success is not the dim text: " + Theme.success)
        // Quieter than danger: less red, and never the full channel
        verify(Theme.success.r < Theme.danger.r, "quieter than danger: " + Theme.success)
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
    // theme: the readouts against the card or panel, the zone
    // colors a readout takes, the page text against the page, the status
    // and cold-zone colors on a panel, and a caption over a secondary fill
    function test_textContrastIsAtLeast4_5_data() {
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_textContrastIsAtLeast4_5(data) {
        usePalette(data.name)
        const rows = [["text", "bg"], ["text", "surface"], ["text", "elevated"],
                      ["textDim", "bg"], ["textDim", "surface"],
                      ["gaugeReadout", "surface"], ["gaugeTick", "surface"],
                      ["gaugeWarn", "surface"], ["gaugeDanger", "surface"],
                      ["warning", "bg"], ["danger", "bg"],
                      ["success", "surface"], ["info", "surface"], ["textDim", "elevated"]]
        for (const [fg, bgName] of rows) {
            const c = contrast(Theme[fg], Theme[bgName])
            verify(c >= 4.5, fg + " on " + bgName + " is " + c.toFixed(2) + ":1 in " + data.name)
        }
        // The label on a primary fill (a highlighted button): 4.2:1 at least,
        // a night palette's dim fill being no brighter than it was
        const textOnPrimary = contrast(Theme.textOnPrimary, Theme.primary)
        verify(textOnPrimary >= 4.2, "textOnPrimary on primary is " + textOnPrimary.toFixed(2) + ":1 in " + data.name)
        // Control borders and the lit color against what they sit on: 3:1
        for (const [fg, bgName] of [["outline", "bg"], ["gaugeValue", "surface"],
                                    ["gaugeNeedle", "surface"]]) {
            const c = contrast(Theme[fg], Theme[bgName])
            verify(c >= 3, fg + " on " + bgName + " is " + c.toFixed(2) + ":1 in " + data.name)
        }
    }

    // The chart traces' palette is the theme's: ten colors a theme, told
    // apart (3:1 against the chart's surface and page, and no two within 38
    // of each other in RGB), warm in the night themes
    function test_theSeriesPaletteIsPerTheme_data() {
        return Theme.paletteNames.map(n => ({ tag: n, name: n }))
    }
    function test_theSeriesPaletteIsPerTheme(data) {
        usePalette(data.name)
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
            if (Theme.nightPalettes.indexOf(data.name) >= 0) {
                verify(a.hsvHue * 360 <= 60, colors[i] + " is warm in " + data.name)
                // At night nothing outshines the text (ADR-0058 section 3)
                verify(lum(colors[i]) <= lum(Theme.text) + 1e-9,
                       colors[i] + " is brighter than the text in " + data.name)
            }
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

    // A mode or style that is not in its list reads as the default
    // everywhere: the colors and the one `active` name
    function test_anUnknownValueReadsAsTheDefault() {
        Theme.night = "nonsense"
        compare(Theme.active, "dark")
        sameColor(Theme.bg, "#141416", "bg")
        sameColor(Theme.gaugeValue, "#ebebee", "gaugeValue")
        Theme.night = "on"
        Theme.nightStyle = "nonsense"
        compare(Theme.active, "nightAmber", "an unknown style is amber")
        Theme.look = "nonsense"
        Theme.night = "off"
        compare(Theme.active, "dark", "an unknown look is dark")
        // Night wins over the look, and the look is back when it ends
        Theme.look = "daylight"
        compare(Theme.active, "daylight")
        Theme.nightStyle = "amber"
        Theme.night = "on"
        compare(Theme.active, "nightAmber", "Night over Daylight")
        Theme.night = "off"
        compare(Theme.active, "daylight", "Daylight again")
    }

    // The window's color scheme follows the look: a light page is Light,
    // a dark one (Dark, Night) Dark
    function test_theWindowsColorSchemeFollowsTheLook_data() {
        return [
            { tag: "dark",       name: "dark",       scheme: 2 },
            { tag: "daylight",   name: "daylight",   scheme: 1 },
            { tag: "nightAmber", name: "nightAmber", scheme: 2 },
        ]
    }
    function test_theWindowsColorSchemeFollowsTheLook(data) {
        usePalette(data.name)
        const scheme = Harness.colorSchemeFollowing(Theme)
        // The offscreen platform has no scheme to override: it reads
        // Unknown (0) whatever is set, so only a platform that takes the
        // override can fail this
        if (scheme === 0)
            skip("this platform ignores the color scheme override")
        compare(scheme, data.scheme)
    }

    // At night the readout is the text color, so nothing outshines the text
    function test_theReadoutIsTheTextAtNight_data() {
        return [{ tag: "nightAmber", name: "nightAmber" }, { tag: "nightRed", name: "nightRed" }]
    }
    function test_theReadoutIsTheTextAtNight(data) {
        usePalette(data.name)
        verify(lum(Theme.gaugeReadout) <= lum(Theme.text) + 1e-9, "no brighter than the text")
        sameColor(Theme.gaugeReadout, Theme.text, "the text color")
    }

    Component {
        id: storeComponent
        Settings {
            category: "ui"
            property string look: "unset"
            property string night: "unset"
            property string nightStyle: "unset"
            property string nightFrom: "unset"
            property string nightUntil: "unset"
            property string nightOverride: "unset"
            property double nightOverrideUntil: -1
        }
    }
    // A stored key, as a window made later would read it
    function stored(key) {
        const reader = storeComponent.createObject(null)
        const value = reader[key]
        reader.destroy()
        return value
    }

    // Each control is written to the `ui` settings as it changes
    function test_theControlsAreKept() {
        Theme.night = "on"
        tryVerify(() => stored("night") === "on", 2000, "night")
        Theme.nightStyle = "red"
        tryVerify(() => stored("nightStyle") === "red", 2000, "style")
        Theme.night = "schedule"
        Theme.nightFrom = "20:15"
        tryVerify(() => stored("nightFrom") === "20:15", 2000, "from")
        Theme.nightUntil = "06:45"
        tryVerify(() => stored("nightUntil") === "06:45", 2000, "until")
        Theme.look = "daylight"
        tryVerify(() => stored("look") === "daylight", 2000, "look")
        Theme.look = "dark"
        tryVerify(() => stored("look") === "dark", 2000, "look back")
        Theme.night = "off"
        tryVerify(() => stored("night") === "off", 2000, "and back")
    }

    // What v0.7.0's one theme becomes
    function test_theOldThemeMigrates_data() {
        return [
            { tag: "dark", old: "dark", night: "off", style: "amber" },
            { tag: "lowGlare", old: "lowGlare", night: "on", style: "amber" },
            { tag: "night", old: "night", night: "on", style: "red" },
            { tag: "unknown", old: "nonsense", night: "off", style: "amber" },
            { tag: "absent", old: undefined, night: "off", style: "amber" }
        ]
    }
    function test_theOldThemeMigrates(data) {
        const m = Theme.migrateTheme(data.old)
        compare(m.night, data.night)
        compare(m.nightStyle, data.style)
    }

    // Switching the palette moves the roles: a binding on a role follows
    function test_switchingMovesTheRoles() {
        const dark = Qt.color(Theme.bg)
        usePalette("nightRed")
        verify(!Qt.colorEqual(Theme.bg, dark), "bg moved")
        sameColor(Theme.gaugeValue, "#ff5a4a", "red's lit color")
        usePalette("nightAmber")
        sameColor(Theme.gaugeValue, "#f0a53a", "amber's lit color")
        usePalette("dark")
        sameColor(Theme.bg, dark, "back to dark")
    }

    // ---- The schedule

    // A fake clock: a local time on 2026-11-10
    function at(h, m) { return new Date(2026, 10, 10, h, m, 0, 0) }
    function tomorrowAt(h, m) { return new Date(2026, 10, 11, h, m, 0, 0) }
    function setClock(date) { Theme.now = () => date }
    function schedule(from, until) {
        Theme.night = "schedule"
        Theme.nightFrom = from
        Theme.nightUntil = until
    }

    function test_theWindowHoldsFromAndNotUntil() {
        compare(Theme.inWindow(at(19, 0), "19:00", "07:00"), true, "on at from")
        compare(Theme.inWindow(at(18, 59), "19:00", "07:00"), false, "off before from")
        compare(Theme.inWindow(at(7, 0), "19:00", "07:00"), false, "off at until")
        compare(Theme.inWindow(at(6, 59), "19:00", "07:00"), true, "on before until")
        compare(Theme.inWindow(at(23, 59), "19:00", "07:00"), true, "on before midnight")
        compare(Theme.inWindow(at(0, 0), "19:00", "07:00"), true, "on after midnight")
        compare(Theme.inWindow(at(12, 0), "19:00", "07:00"), false, "off by day")
        compare(Theme.inWindow(at(8, 0), "06:00", "09:00"), true, "a window by day")
        compare(Theme.inWindow(at(9, 0), "06:00", "09:00"), false, "off at its until")
        compare(Theme.inWindow(at(5, 59), "06:00", "09:00"), false, "off before its from")
        compare(Theme.inWindow(at(12, 0), "12:00", "12:00"), false, "from == until holds nothing")
        compare(Theme.inWindow(at(12, 0), "12:0", "13:00"), false, "a bad time holds nothing")
    }

    function test_theNextBoundary() {
        const b = Theme.nextBoundary(at(18, 0), "19:00", "07:00")
        compare(b.getHours(), 19)
        compare(b.getDate(), 10, "today")
        const c = Theme.nextBoundary(at(20, 0), "19:00", "07:00")
        compare(c.getHours(), 7)
        compare(c.getDate(), 11, "tomorrow")
        const d = Theme.nextBoundary(at(19, 0), "19:00", "07:00")
        compare(d.getHours(), 7, "after the one that has just passed")
    }

    // Mode x style x in the window x an override: the palette shown
    function test_theResolutionTable_data() {
        const rows = []
        for (const mode of ["off", "on", "schedule"])
            for (const style of ["amber", "red"])
                for (const inside of [true, false])
                    for (const over of ["", "on", "off"]) {
                        const night = mode === "on" ? true : mode === "off" ? false
                                    : over !== "" ? over === "on" : inside
                        rows.push({ tag: mode + " " + style + " " + (inside ? "in" : "out") + " " + (over || "-"),
                                    mode: mode, style: style, inside: inside, over: over,
                                    want: night ? (style === "red" ? "nightRed" : "nightAmber") : "dark" })
                    }
        return rows
    }
    function test_theResolutionTable(data) {
        setClock(data.inside ? at(22, 0) : at(12, 0))
        Theme.nightStyle = data.style
        schedule("19:00", "07:00")
        Theme.night = data.mode
        Theme.nightOverride = data.over
        Theme.nightOverrideUntil = data.over === "" ? 0 : tomorrowAt(23, 0).getTime()
        Theme.refreshNight()
        compare(Theme.active, data.want)
        compare(Theme.nightActive, data.want !== "dark")
    }

    // A tap in a schedule sets the opposite for now, a second hands back
    function test_theSwitchHoldsTheOppositeUntilTheBoundary() {
        schedule("19:00", "07:00")
        setClock(at(18, 0))
        Theme.refreshNight()
        compare(Theme.nightActive, false, "the schedule says off at 18:00")
        Theme.toggleNight()
        compare(Theme.nightOverride, "on")
        compare(Theme.nightActive, true, "the tap shows night")
        compare(Theme.nightOverrideUntil, at(19, 0).getTime(), "until the next boundary")
        Theme.toggleNight()
        compare(Theme.nightOverride, "", "a second tap clears it")
        compare(Theme.nightActive, false, "the schedule rules again")
        Theme.toggleNight()
        compare(Theme.nightOverride, "on", "a third tap holds night again, not off")
        Theme.toggleNight()
        compare(Theme.nightOverride, "")
        // the other way: in the window at 20:00
        setClock(at(20, 0))
        Theme.refreshNight()
        compare(Theme.nightActive, true)
        Theme.toggleNight()
        compare(Theme.nightOverride, "off")
        compare(Theme.nightActive, false)
        compare(Theme.nightOverrideUntil, tomorrowAt(7, 0).getTime(), "until 07:00 tomorrow")
    }

    // In On and Off the switch flips the mode itself: no override
    function test_theSwitchFlipsTheModeInOnAndOff() {
        Theme.night = "off"
        Theme.toggleNight()
        compare(Theme.night, "on")
        compare(Theme.nightActive, true)
        compare(Theme.nightOverride, "", "no override in On")
        Theme.toggleNight()
        compare(Theme.night, "off")
        compare(Theme.nightActive, false)
        compare(Theme.nightOverride, "")
    }

    // A change to Night, From or Until clears an override, so a setting
    // made in Settings shows at once; a change of style leaves it
    function test_aSettingClearsTheOverride_data() {
        return [
            { tag: "night", set: () => { Theme.night = "off"; Theme.night = "schedule" }, cleared: true },
            { tag: "from", set: () => { Theme.nightFrom = "20:00" }, cleared: true },
            { tag: "until", set: () => { Theme.nightUntil = "08:00" }, cleared: true },
            { tag: "style", set: () => { Theme.nightStyle = "red" }, cleared: false }
        ]
    }
    function test_aSettingClearsTheOverride(data) {
        schedule("19:00", "07:00")
        setClock(at(18, 0))
        Theme.refreshNight()
        Theme.toggleNight()
        compare(Theme.nightOverride, "on")
        data.set()
        compare(Theme.nightOverride === "", data.cleared)
        if (data.cleared)
            compare(Theme.nightActive, false, "the schedule's own state at 18:00")
    }

    // The override ends at the next boundary, through the tick
    function test_theOverrideEndsAtTheBoundaryThroughTheTick() {
        schedule("19:00", "07:00")
        setClock(at(18, 0))
        Theme.refreshNight()
        Theme.toggleNight()
        compare(Theme.nightActive, true)
        setClock(at(18, 59))
        Theme.ticker.triggered()
        compare(Theme.nightOverride, "on", "still held before the boundary")
        setClock(at(19, 0))
        Theme.ticker.triggered()
        compare(Theme.nightOverride, "", "gone at the boundary")
        compare(Theme.nightActive, true, "and the window is open: nothing visible changes")
        setClock(tomorrowAt(7, 0))
        Theme.ticker.triggered()
        compare(Theme.nightActive, false, "closed at 07:00")
    }

    // The tick follows the schedule with no tap at all
    function test_theTickFollowsTheClock() {
        schedule("19:00", "07:00")
        setClock(at(12, 0))
        Theme.ticker.triggered()
        compare(Theme.nightActive, false)
        setClock(at(19, 0))
        Theme.ticker.triggered()
        compare(Theme.nightActive, true)
    }
    function test_theTickRuns() {
        verify(Theme.ticker.running, "the timer runs")
        compare(Theme.ticker.interval, 30000)
        verify(Theme.ticker.repeat)
    }

    // The app coming back to the front shows the time it is
    Component {
        id: fakeStateComponent
        QtObject { property int state: Qt.ApplicationSuspended }
    }
    function test_comingBackToTheFrontRefreshes() {
        const fake = createTemporaryObject(fakeStateComponent, testCase)
        Theme.stateSource = fake
        schedule("19:00", "07:00")
        setClock(at(12, 0))
        Theme.refreshNight()
        compare(Theme.nightActive, false)
        setClock(at(22, 0))
        fake.state = Qt.ApplicationHidden
        compare(Theme.nightActive, false, "not while it stays in the background")
        fake.state = Qt.ApplicationActive
        compare(Theme.nightActive, true, "at once when it is active again")
    }

    // The words: the next change of what is shown, never the expiry
    function test_theWordsSayTheNextChange() {
        schedule("19:00", "07:00")
        setClock(at(18, 0))
        Theme.refreshNight()
        compare(Theme.nightWords(), "Off until 19:00", "the schedule, before")
        Theme.toggleNight()
        compare(Theme.nightWords(), "On until 07:00", "the tap at 18:00")
        compare(Theme.scheduleWords(), "On until 07:00", "Settings says it too while a tap holds")
        Theme.toggleNight()
        compare(Theme.scheduleWords(), "Scheduled 19:00 to 07:00", "and the window without one")
        setClock(at(20, 0))
        Theme.refreshNight()
        compare(Theme.nightWords(), "On until 07:00")
        Theme.toggleNight()
        compare(Theme.nightWords(), "Off until 19:00", "the tap at 20:00")
        compare(Theme.nightSwitchName(), "Night is off until 19:00. Turn it on")
        Theme.toggleNight()
        compare(Theme.nightSwitchName(), "Night is on until 07:00. Turn it off")
        Theme.night = "on"
        compare(Theme.nightSwitchName(), "Night is on. Turn it off")
        Theme.night = "off"
        compare(Theme.nightSwitchName(), "Night is off. Turn it on")
    }

    // The override is stored with its expiry, and its end is stored too
    function test_theOverrideIsStored() {
        schedule("19:00", "07:00")
        setClock(at(18, 0))
        Theme.refreshNight()
        Theme.toggleNight()
        tryVerify(() => stored("nightOverride") === "on", 2000, "stored")
        tryVerify(() => stored("nightOverrideUntil") === at(19, 0).getTime(), 2000, "with its expiry")
        setClock(at(19, 30))
        Theme.ticker.triggered()
        compare(Theme.nightOverride, "")
        tryVerify(() => stored("nightOverride") === "", 2000, "and the store follows its end")
    }

    // What a window made later reads (the load's rules, on a stand-in store)
    function loaded(over) {
        const base = { look: "", night: "", nightStyle: "", nightFrom: "", nightUntil: "",
                       nightOverride: "", nightOverrideUntil: 0, theme: "" }
        Theme.loadStored(Object.assign(base, over))
    }
    function test_theOldThemeIsReadOnceAndNeverNeeded() {
        loaded({ theme: "lowGlare" })
        compare(Theme.night, "on")
        compare(Theme.nightStyle, "amber")
        compare(Theme.active, "nightAmber")
        loaded({ theme: "night" })
        compare(Theme.nightStyle, "red")
        compare(Theme.active, "nightRed")
        loaded({ theme: "dark" })
        compare(Theme.night, "off")
        // a stored Night wins over a leftover theme
        loaded({ theme: "night", night: "off", nightStyle: "amber", look: "dark" })
        compare(Theme.night, "off")
        compare(Theme.active, "dark")
    }
    function test_theLoadClampsAndKeepsTheSchedule() {
        loaded({ night: "nonsense", nightStyle: "nonsense", look: "nonsense", nightFrom: "20:00", nightUntil: "06:00" })
        compare(Theme.night, "off")
        compare(Theme.nightStyle, "amber")
        compare(Theme.nightFrom, "20:00")
        compare(Theme.nightUntil, "06:00")
        loaded({ night: "schedule", nightFrom: "25:00", nightUntil: "06:00" })
        compare(Theme.nightFrom, "19:00", "a bad stored time keeps the default")
        loaded({ night: "schedule", nightFrom: "08:00", nightUntil: "08:00" })
        compare(Theme.nightFrom, "19:00", "an equal stored pair keeps the default")
        compare(Theme.nightUntil, "07:00")
        loaded({ look: "daylight" })
        compare(Theme.look, "daylight", "a stored look is read back")
        compare(Theme.active, "daylight")
        loaded({ look: "nonsense" })
        compare(Theme.look, "dark", "an unknown one is Dark")
    }
    function test_anOverrideIsReadBackAndAnExpiredOneDropped() {
        setClock(at(18, 0))
        loaded({ night: "schedule", nightOverride: "on", nightOverrideUntil: at(19, 0).getTime() })
        compare(Theme.nightOverride, "on", "kept across a restart")
        compare(Theme.nightActive, true)
        loaded({ night: "schedule", nightOverride: "on", nightOverrideUntil: at(17, 0).getTime() })
        compare(Theme.nightOverride, "", "an expired one is dropped on load")
        compare(Theme.nightActive, false)
        loaded({ night: "schedule", nightOverride: "sideways", nightOverrideUntil: at(19, 0).getTime() })
        compare(Theme.nightOverride, "", "an unknown one is dropped")
    }

    // The type choice leans on tabular figures: the bundled fonts load, and
    // `tnum` makes every digit the same width in the value family
    Component {
        id: metricsComponent
        TextMetrics {}
    }
    function widthOf(text, family, features) {
        const m = createTemporaryObject(metricsComponent, testCase)
        m.font.family = family
        m.font.pixelSize = 28
        m.font.features = features
        m.text = text
        return m.advanceWidth
    }
    function test_theBundledFontsLoad() {
        tryCompare(Theme.barlowRegular, "status", FontLoader.Ready)
        tryCompare(Theme.barlowCondensedBold, "status", FontLoader.Ready)
        // literal names, whatever name a backend reports for the file
        compare(Theme.fontFamily, "Barlow")
        compare(Theme.valueFamily, "Barlow Semi Condensed")
        verify(Theme.valueFeatures.tnum === 1)
        // the platform's font database knows both literals, which is what
        // a backend that names a file by its style would break
        const fams = Qt.fontFamilies()
        verify(fams.indexOf("Barlow") >= 0, "Barlow is a family")
        verify(fams.indexOf("Barlow Semi Condensed") >= 0, "Barlow Semi Condensed is a family")
    }
    function test_tabularFiguresGiveEveryDigitTheSameWidth() {
        const fam = Theme.valueFamily
        const digits = "0123456789"
        const w = []
        for (const d of digits)
            w.push(widthOf(d + d + d + d, fam, Theme.valueFeatures))
        for (let i = 1; i < w.length; ++i)
            compare(w[i], w[0], "digit " + i + " with tnum")
        // without the feature the figures are proportional, so the
        // comparison above measures the feature and not the font alone
        verify(widthOf("1111", fam, {}) !== widthOf("0000", fam, {}),
               "without tnum a 1 and a 0 differ in width")
    }
}
