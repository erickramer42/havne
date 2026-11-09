// File/layer: app/qml/Theme.qml | Qt6 Quick singleton, UI layer
// Testing: QML: QmlTest.theme, QmlTest.settings, QmlTest.dashboard,
//          QmlTest.liveview, QmlTest.log, QmlTest.phone (test_qml); Manual smoke
pragma Singleton
import QtCore
import QtQuick

// Central palette: three palettes (dark, nightAmber, nightRed), the one in
// `active` read through the roles below. main.cpp colors the platform's own
// window (Android's background, Windows' title bar) from bg, surface and
// text when the palette changes. Dark is the default. The two night
// palettes (amber and orange, or red, on near black) keep the light low for
// a car's cabin after dark; red is the danger color there too, so danger is
// told from the rest by more than hue (GaugeBase: a mark beside the readout
// and a heavier zone stroke). In them nothing outshines the text: no trace
// and no readout is brighter than it.
// Text holds 4.5:1 or more against the surfaces it sits on in each theme,
// except text on the selected-row color (primary), held to Dark's own 4.2:1
// (a lighter primary would be a bright row in a dark cabin); the readouts,
// the zone colors and the trace colors have their floors too
// (tests/qml/tst_theme.qml measures each).
QtObject {
    id: theme
    // ---- The look, Night and its style (ADR-0058). Three controls give
    // the palette on screen, `active`, which every role reads through:
    // the look (Dark, the only one so far), Night (off, on, or on a
    // schedule) and the style Night takes (amber or red). All are kept
    // between runs under the `ui` settings and read here, when the
    // singleton is made, so the first frame already has the palette. A
    // value outside its list reads as the default.
    property string look: "dark"
    property string night: "off"
    property string nightStyle: "amber"
    // The schedule's window, 24-hour local times "HH:MM"; from is inclusive,
    // until exclusive, and the window may cross midnight
    property string nightFrom: "19:00"
    property string nightUntil: "07:00"
    // A tap on the switch in schedule mode: "on" or "off" until the next
    // boundary (nightOverrideUntil, ms since the epoch). Any change to
    // Night, From or Until clears it
    property string nightOverride: ""
    property double nightOverrideUntil: 0
    // The clock, a seam: a test replaces it
    property var now: () => new Date()
    // Whether night is shown now; set by refreshNight() on every change of
    // the settings above, every 30 s and when the application is active again
    property bool shownNight: false
    readonly property bool nightActive: shownNight
    readonly property string active: nightActive ? (clampStyle(nightStyle) === "red" ? "nightRed" : "nightAmber")
                                                 : clampLook(look)

    readonly property var lookNames: ["dark"]
    readonly property var nightModes: ["off", "on", "schedule"]
    readonly property var nightStyles: ["amber", "red"]
    readonly property var paletteNames: ["dark", "nightAmber", "nightRed"]
    function clampLook(name) { return lookNames.indexOf(name) >= 0 ? name : "dark" }
    function clampNight(name) { return nightModes.indexOf(name) >= 0 ? name : "off" }
    function clampStyle(name) { return nightStyles.indexOf(name) >= 0 ? name : "amber" }

    // What the stored `theme` of v0.7.0 becomes: Night and its style
    function migrateTheme(name) {
        return name === "lowGlare" ? { night: "on", nightStyle: "amber" }
             : name === "night"    ? { night: "on", nightStyle: "red" }
             :                       { night: "off", nightStyle: "amber" }
    }

    // "HH:MM" to minutes after midnight, or -1 when it isn't a time
    function minutesOf(text) {
        const m = /^([01]\d|2[0-3]):([0-5]\d)$/.exec(String(text))
        return m === null ? -1 : Number(m[1]) * 60 + Number(m[2])
    }
    // The window holds a time: on at from, off at until, across midnight
    // too; a pair that isn't two different times holds none
    function inWindow(date, from, until) {
        const f = minutesOf(from), u = minutesOf(until)
        if (f < 0 || u < 0 || f === u)
            return false
        const m = date.getHours() * 60 + date.getMinutes()
        return f < u ? (m >= f && m < u) : (m >= f || m < u)
    }
    // The next time after `date` the window opens or closes
    function nextBoundary(date, from, until) {
        let best = null
        for (const t of [from, until]) {
            const m = minutesOf(t)
            if (m < 0)
                continue
            const d = new Date(date.getTime())
            d.setHours(Math.floor(m / 60), m % 60, 0, 0)
            if (d.getTime() <= date.getTime())
                d.setDate(d.getDate() + 1)
            if (best === null || d.getTime() < best.getTime())
                best = d
        }
        return best
    }

    function nightNow() {
        const mode = clampNight(night)
        if (mode !== "schedule")
            return mode === "on"
        if (nightOverride === "on" || nightOverride === "off")
            return nightOverride === "on"
        return inWindow(now(), nightFrom, nightUntil)
    }
    // Recompute what is shown; an override past its expiry goes first
    function refreshNight() {
        if (nightOverride !== "" && now().getTime() >= nightOverrideUntil)
            clearOverride()
        shownNight = nightNow()
    }
    function clearOverride() {
        nightOverride = ""
        nightOverrideUntil = 0
    }
    // The switch: On and Off flip the mode; a schedule gets an override
    // to the opposite of what it shows, until its next boundary, and a
    // second tap clears it
    function toggleNight() {
        const mode = clampNight(night)
        if (mode === "on") {
            night = "off"
        } else if (mode === "off") {
            night = "on"
        } else if (nightOverride !== "") {
            clearOverride()
        } else {
            const when = now()
            const boundary = nextBoundary(when, nightFrom, nightUntil)
            nightOverride = shownNight ? "off" : "on"
            nightOverrideUntil = boundary === null ? when.getTime() : boundary.getTime()
        }
        refreshNight()
    }
    // When the shown state next changes, for the switch and Settings: "On
    // until 07:00" while night is on, "Off until 19:00" while it is off.
    // In On and Off there is no time to say
    function nightWords() {
        const mode = clampNight(night)
        if (mode !== "schedule")
            return mode === "on" ? qsTr("On") : qsTr("Off")
        return nightActive ? qsTr("On until %1").arg(nightUntil) : qsTr("Off until %1").arg(nightFrom)
    }
    // Settings' page under Scheduled: the window, or the words while a tap
    // holds it
    function scheduleWords() {
        return nightOverride !== "" ? nightWords() : qsTr("Scheduled %1 to %2").arg(nightFrom).arg(nightUntil)
    }
    // Settings' list row
    function nightSummary() {
        const mode = clampNight(night)
        if (mode === "off")
            return qsTr("Night off")
        if (mode === "on")
            return clampStyle(nightStyle) === "red" ? qsTr("Night on, red") : qsTr("Night on, amber")
        return nightOverride !== "" ? qsTr("Night %1").arg(nightWords().toLowerCase())
                                    : qsTr("Night scheduled %1 to %2").arg(nightFrom).arg(nightUntil)
    }
    // The switch's accessible words: the state, then what a tap does
    function nightSwitchName() {
        const state = clampNight(night) === "schedule" ? nightWords().toLowerCase()
                                                       : nightActive ? qsTr("on") : qsTr("off")
        return qsTr("Night is %1. %2").arg(state).arg(nightActive ? qsTr("Turn it off") : qsTr("Turn it on"))
    }

    property Settings store: Settings {
        category: "ui"
        property string look: ""
        property string night: ""
        property string nightStyle: ""
        property string nightFrom: ""
        property string nightUntil: ""
        property string nightOverride: ""
        property double nightOverrideUntil: 0
        // v0.7.0's one theme: read once to migrate, never written
        property string theme: ""
    }
    property bool loading: true
    // qmllint disable missing-property
    // The settings as they were stored: the one a window made now reads. A
    // value outside its list, a bad time or an expired override gives the
    // default; with no Night stored, the v0.7.0 theme is migrated
    function loadStored(src) {
        loading = true
        if (src.night === "") {
            const m = migrateTheme(src.theme)
            night = m.night
            nightStyle = m.nightStyle
        } else {
            night = clampNight(src.night)
            nightStyle = clampStyle(src.nightStyle)
        }
        look = clampLook(src.look)
        const okTimes = minutesOf(src.nightFrom) >= 0 && minutesOf(src.nightUntil) >= 0
                        && src.nightFrom !== src.nightUntil
        nightFrom = okTimes ? src.nightFrom : "19:00"
        nightUntil = okTimes ? src.nightUntil : "07:00"
        if ((src.nightOverride === "on" || src.nightOverride === "off")
                && src.nightOverrideUntil > now().getTime()) {
            nightOverride = src.nightOverride
            nightOverrideUntil = src.nightOverrideUntil
        } else {
            clearOverride()
        }
        loading = false
        // Writes every key, so a migrated one is kept in its new form
        store.look = look
        store.night = night
        store.nightStyle = nightStyle
        store.nightFrom = nightFrom
        store.nightUntil = nightUntil
        store.nightOverride = nightOverride
        store.nightOverrideUntil = nightOverrideUntil
        refreshNight()
    }
    Component.onCompleted: loadStored(store)
    onLookChanged: if (!loading) { store.look = look; refreshNight() }
    onNightChanged: if (!loading) { clearOverride(); store.night = night; refreshNight() }
    onNightStyleChanged: if (!loading) store.nightStyle = nightStyle
    onNightFromChanged: if (!loading) { clearOverride(); store.nightFrom = nightFrom; refreshNight() }
    onNightUntilChanged: if (!loading) { clearOverride(); store.nightUntil = nightUntil; refreshNight() }
    onNightOverrideChanged: if (!loading) store.nightOverride = nightOverride
    onNightOverrideUntilChanged: if (!loading) store.nightOverrideUntil = nightOverrideUntil
    // qmllint enable missing-property

    // A boundary takes effect within 30 s: a tick compares two times, with
    // no arithmetic about the next boundary for a sleep or a clock change
    // to break
    property Timer ticker: Timer {
        interval: 30000
        repeat: true
        running: true
        onTriggered: theme.refreshNight()
    }
    // A suspended phone's timers don't fire, so the app coming back to the
    // front shows the time it is now. The source is a seam for a test
    property QtObject stateSource: Qt.application
    property Connections wake: Connections {
        target: theme.stateSource
        function onStateChanged() {
            // qmllint disable missing-property
            if (theme.stateSource.state === Qt.ApplicationActive)
                theme.refreshNight()
            // qmllint enable missing-property
        }
    }

    // The analog dial's bezel, face, needle and hub shades from its face
    // color, for a theme with no hand-picked ones
    function shadesOf(face) {
        return { bezelHi: Qt.lighter(face, 2.1), bezelLo: Qt.lighter(face, 1.6),
                 faceHi: Qt.lighter(face, 1.45), faceLo: Qt.darker(face, 1.9),
                 needleBase: Qt.lighter(face, 1.7), hubHi: Qt.lighter(face, 2.5) }
    }

    readonly property var palettes: ({
        dark: {
            // Near-neutral graphite; the accent is the text color as a fill
            bg: "#141416", surface: "#1b1b1e", elevated: "#26262b", elevatedLight: "#323238",
            outline: "#65656f",   // control borders; 3:1 on bg
            primary: "#ebebee", textOnPrimary: "#141416", text: "#ebebee", textDim: "#9a9aa3",
            success: "#4ade80", warning: "#ffd166", danger: "#f87171", info: "#60a5fa",
            gaugeFace: "#1b1b1e", gaugeValue: "#ebebee",
            shades: { bezelHi: "#3a3a3f", bezelLo: "#2c2c31", faceHi: "#252529", faceLo: "#0e0e10",
                      needleBase: "#2a2a2e", hubHi: "#4b4b52" },
            gaugeReadout: "#ebebee", gaugeNeedle: "#ff5a5f", gaugeTick: "#ebebee",
            gaugeWarn: "#ffd166", gaugeDanger: "#f87171",
            // One per hue family, so traces stay distinct on the dark surface
            series: ["#4aa3ff",   // blue
                     "#31d08c",   // green
                     "#5dd9d0",   // aqua
                     "#f0e442",   // yellow
                     "#e39ac9",   // reddish purple
                     "#ffa94d",   // orange
                     "#f2f2f2",   // near-white
                     "#b08cff",   // violet
                     "#a3e635",   // lime
                     "#e879f9"]   // fuchsia
        },
        nightAmber: {
            bg: "#0d0a07", surface: "#18120c", elevated: "#261c12", elevatedLight: "#31241a",
            outline: "#85694a", primary: "#7f440d", textOnPrimary: "#f3c895", text: "#f3c895", textDim: "#b58f64",
            success: "#f0a53a", warning: "#ff8a3a", danger: "#ff4a38", info: "#d2a86a",
            gaugeFace: "#1a130c", gaugeValue: "#f0a53a",
            shades: shadesOf("#1a130c"),
            gaugeReadout: "#f3c895", gaugeNeedle: "#ff7a2e", gaugeTick: "#e9bf8a",
            gaugeWarn: "#ff8a3a", gaugeDanger: "#ff4a38",
            // Warm, told apart by lightness and saturation as much as hue
            series: ["#f0b45a", "#ff4000", "#e6c200", "#9e5d47", "#d17d00",
                     "#9e9147", "#ffb299", "#d9c27a", "#ff794d", "#b89c00"]
        },
        nightRed: {
            bg: "#080404", surface: "#130909", elevated: "#201010", elevatedLight: "#2b1616",
            outline: "#934a44", primary: "#7a241c", textOnPrimary: "#ff9484", text: "#ff9484", textDim: "#c46a5e",
            success: "#e8584a", warning: "#ff8a4a", danger: "#ff3030", info: "#d98a78",
            gaugeFace: "#150909", gaugeValue: "#ff5a4a",
            shades: shadesOf("#150909"),
            gaugeReadout: "#ff9484", gaugeNeedle: "#ff4a3a", gaugeTick: "#f08a7a",
            gaugeWarn: "#ff8a4a", gaugeDanger: "#ff3030",
            series: ["#ee9a7c", "#ff0000", "#9e5b18", "#ff7373", "#9e7777",
                     "#ff8000", "#e08a4a", "#d1431f", "#b86253", "#d9a3a3"]
        }
    })
    readonly property var colors: palettes[active]

    readonly property color bg:             colors.bg
    readonly property color surface:        colors.surface
    readonly property color elevated:       colors.elevated
    readonly property color elevatedLight:  colors.elevatedLight
    readonly property color outline:        colors.outline
    readonly property color primary:        colors.primary
    // The label on a primary fill: the page color in Dark (primary is the
    // text color there), the text color at night (the fill stays dim)
    readonly property color textOnPrimary:      colors.textOnPrimary
    readonly property color text:           colors.text
    readonly property color textDim:        colors.textDim
    readonly property color success:        colors.success
    readonly property color warning:        colors.warning
    readonly property color danger:         colors.danger
    readonly property color info:           colors.info
    // A hairline: the outline at 55 % over the surface, the rule between
    // cells of a panel and a panel's edge
    readonly property color hairline:       Qt.tint(surface, Qt.alpha(outline, 0.55))
    // Corner radii: controls, and cards and panels
    readonly property int radius:           4
    readonly property int radiusCard:       6

    // The gauges' roles (ADR-0057 section 9): no gauge draws a literal color.
    // The face under the dial and the cards; value: the lit segments, the
    // progress arc and the trace; readout: the value's text; needle; the
    // ticks and their numerals; warn and danger: the zones and a readout
    // past them
    readonly property color gaugeFace:      colors.gaugeFace
    readonly property color gaugeValue:     colors.gaugeValue
    readonly property color gaugeReadout:   colors.gaugeReadout
    readonly property color gaugeNeedle:    colors.gaugeNeedle
    readonly property color gaugeTick:      colors.gaugeTick
    readonly property color gaugeWarn:      colors.gaugeWarn
    readonly property color gaugeDanger:    colors.gaugeDanger
    // The analog dial's own shades (bezelHi, bezelLo, faceHi, faceLo,
    // needleBase, hubHi): Dark's are fixed colors
    readonly property var gaugeShade:       colors.shades

    // A phone's density: Main.qml sets dense when the layout (after the
    // zoom) is narrower than 720 px or shorter than 480 px, a phone
    // upright or on its side, or a window as small. Margins, spacing and
    // control heights then shrink, so a page shows more of itself at
    // 100 %: the test phone's run of v0.4.0 read best at 75 %.
    property bool dense: false
    // Shorter than 480 px: a phone on its side, where Live's and the
    // viewer's charts start maximized (Main.qml sets it with dense)
    property bool shortLayout: false
    // Narrower than 720 px: a phone upright, where the tab bar shows
    // (Main.qml sets it)
    property bool compact: false
    readonly property int spacing:          dense ? 6 : 8
    readonly property int pageMargin:       dense ? 10 : 16
    readonly property int controlHeight:    dense ? 32 : 36
    readonly property int rowHeight:        dense ? 36 : 44
    readonly property int toolBarHeight:    dense ? 40 : 48
    readonly property int sidebarWidth:     dense ? 150 : 220
    // The fonts: Barlow for text and captions, Barlow Semi Condensed Bold
    // for values at 17 sp and above, both bundled as Qt resources (SIL OFL
    // 1.1, THIRD_PARTY_NOTICES). Digits in a value use tabular figures
    // (`font.features: { "tnum": 1 }`, `valueFeatures`) so a reading's
    // width doesn't jump as its digits change. The family names are
    // literals, not `FontLoader.name`: a backend that reports a file's
    // style-qualified name (FreeType: "Barlow Semi Condensed Bold") would
    // otherwise make the family match only that one file. Until a file
    // loads, or if it can't, the platform's own font stands in; main.cpp
    // sets the application font on the same condition (Regular loaded)
    property FontLoader barlowRegular: FontLoader { source: "../fonts/Barlow-Regular.ttf" }
    property FontLoader barlowMedium: FontLoader { source: "../fonts/Barlow-Medium.ttf" }
    property FontLoader barlowSemiBold: FontLoader { source: "../fonts/Barlow-SemiBold.ttf" }
    property FontLoader barlowBold: FontLoader { source: "../fonts/Barlow-Bold.ttf" }
    property FontLoader barlowCondensedBold: FontLoader { source: "../fonts/BarlowSemiCondensed-Bold.ttf" }
    readonly property string fontFamily: barlowRegular.status === FontLoader.Ready ? "Barlow" : ""
    readonly property string valueFamily: barlowCondensedBold.status === FontLoader.Ready
                                          ? "Barlow Semi Condensed" : fontFamily
    readonly property var valueFeatures: ({ "tnum": 1 })

    // The type scale: five sizes, in px. Secondary words, a bar's labels
    // and monospace payloads are fontCaption; a body Label that sets no
    // size keeps the platform's. The gauges' own sizes scale with the
    // gauge and are not steps.
    readonly property int fontCaption:      12
    readonly property int fontBody:         14
    readonly property int fontEmphasis:     16
    readonly property int fontTitle:        20
    readonly property int fontHero:         28
    // The least a tap target measures each way on a phone (Material's
    // 48 dp), on the glass: in the layout's units, which the zoom scales,
    // a zoom under 1 asks for more, so a target never shrinks below it;
    // elsewhere a control's own height. A control may draw smaller
    // inside it.
    readonly property real touchTarget:     mobile || compact ? 48 * Math.max(1, 1 / zoom)
                                                              : controlHeight
    // Between a bar's targets and the window's edge, on the glass too, so
    // a tap low on the tab bar never lands on the system's Back or Home
    // beside it
    readonly property real barGap:          8 * Math.max(1, 1 / zoom)

    // A phone or tablet: touch only, no keyboard shortcuts, no file
    // manager for the app's own folders
    readonly property bool mobile: Qt.platform.os === "android" || Qt.platform.os === "ios"

    // A value this old is stale: Live dims its row and the chart breaks
    // its trace across the gap, and the Dashboard dims its gauge. Core's
    // age (ECUIO_VALUE_STALE_US), the one past which a computed signal's
    // input gives no value, so what the UI dims and what core refuses
    // agree. 3 s is generous against a full rotation of the requests, at
    // most the engine's 100 ms wait each, so about 1 s for the obd2 set.
    readonly property int staleMs:          Bridge.valueStaleMs

    // The whole UI's zoom, set with Ctrl + / Ctrl - / Ctrl 0 or from
    // Settings. Main.qml lays the UI out at the window's size divided by
    // it and scales the result, so every page reflows as if the window
    // were that size, and stores it between runs.
    property real zoom: 1.0
    readonly property var zoomSteps: [0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0]
    function clampZoom(z) {
        const v = Number(z)
        return isFinite(v) && v > 0 ? Math.max(zoomSteps[0], Math.min(zoomSteps[zoomSteps.length - 1], v))
                                    : 1.0
    }
    // To the next step up (direction > 0) or down from the current zoom
    function stepZoom(direction) {
        if (direction > 0) {
            for (const s of zoomSteps)
                if (s > zoom + 0.001) { zoom = s; return }
        } else {
            for (let i = zoomSteps.length - 1; i >= 0; --i)
                if (zoomSteps[i] < zoom - 0.001) { zoom = zoomSteps[i]; return }
        }
    }

    // Developer mode, Settings' switch, off until switched on; Main.qml
    // stores it with the zoom. Off, the app offers only real devices and
    // the Log page shows the activity log alone. On, it adds the mock://
    // preset and the Log page's raw frame table with its bridge counters.
    property bool developerMode: false

    // Fixed-pitch family, so hex columns and digits keep their width row
    // to row. "monospace" is a fontconfig and Android alias; Windows and
    // the Apple platforms don't resolve it.
    readonly property string monoFamily: Qt.platform.os === "windows" ? "Consolas"
                                       : Qt.platform.os === "osx" || Qt.platform.os === "ios"
                                         ? "Menlo"
                                         : "monospace"

    // Chart trace colors, per palette (each palette's `series`): one per hue
    // family on Dark so traces stay distinct on the dark surface, warm in
    // the night palettes, none brighter than their text. LiveView keys them by model row. Dark has no red:
    // it is the danger color, so a trace drawn in it would read as an alarm.
    readonly property var seriesColors: colors.series
    // A trace's style by its row (Live's model row, a recording's signal
    // index): rows past the palette reuse its colors with a dashed trace,
    // so a chart still tells them apart (a list's swatch shows the color
    // only)
    readonly property var traceDashes: [[], [8, 4], [2, 3]]
    function seriesColor(row: int): color {
        return seriesColors[row % seriesColors.length]
    }
    function seriesDash(row: int): var {
        return traceDashes[Math.floor(row / seriesColors.length) % traceDashes.length]
    }
    // A second recording's trace of a row: the next pattern along from the
    // row's own, so the two read apart however the row is drawn
    function seriesCompareDash(row: int): var {
        return traceDashes[(Math.floor(row / seriesColors.length) + 1) % traceDashes.length]
    }

    // Link state -> color. Whitelists the healthy and the explained
    // states; fault, degraded and any future state fall to danger.
    function linkColor(state: string): color {
        switch (state) {
        case "ok":              return success
        case "quiet":
        case "requests":
        case "stopped":         return warning
        case "disconnected":    return textDim
        default:                return danger
        }
    }

    // Link state -> short status label (header pill, Home card). Unknown
    // states fall to "Link degraded", mirroring linkColor. LiveView's
    // banner keeps its own longer messages, which carry the episode count
    // and error code.
    // The same states in one short word, for a header with no room
    function linkShortLabel(state: string): string {
        switch (state) {
        case "ok":              return qsTr("Live")
        case "fault":           return qsTr("Fault")
        case "requests":        return qsTr("Failing")
        case "quiet":           return qsTr("Quiet")
        case "stopped":         return qsTr("Stopped")
        case "disconnected":    return qsTr("Offline")
        default:                return qsTr("Degraded")
        }
    }
    function linkLabel(state: string): string {
        switch (state) {
        case "ok":              return qsTr("Session active")
        case "fault":           return qsTr("Adapter fault")
        case "requests":        return qsTr("Requests failing")
        case "quiet":           return qsTr("Link quiet")
        case "stopped":         return qsTr("Replay stopped")
        case "disconnected":    return qsTr("No session")
        default:                return qsTr("Link degraded")
        }
    }
}
