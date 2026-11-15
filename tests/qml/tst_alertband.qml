// File/layer: tests/qml/tst_alertband.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.alertband

import QtQuick
import QtTest
import App

// The alert band over a ListModel of ConditionModel's roles, its clock
// Theme.now's; one case through the bridge's own model on mock://demo
TestCase {
    id: testCase
    name: "AlertBand"
    when: windowShown

    // Theme.now's ms while a case runs
    property real clock: 1700000000000

    function usePalette(name) {
        const night = Theme.nightPalettes.indexOf(name) >= 0
        Theme.night = night ? "on" : "off"
        if (night)
            Theme.nightStyle = name === "nightRed" ? "red" : "amber"
        else
            Theme.look = name
    }
    function palettes() {
        return Theme.paletteNames.map(p => ({ tag: p, palette: p }))
    }

    Component { id: rowsC; ListModel {} }
    Component { id: bandC; AlertBand { width: 380 } }

    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        usePalette("dark")
        clock = 1700000000000
        Theme.now = () => new Date(testCase.clock)
    }
    function cleanup() {
        Theme.now = () => new Date()
        usePalette("dark")
        if (Bridge.connected)
            Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 10000)
        Bridge.saveConditions('{"havne_conditions":1,"conditions":[]}')
    }

    function row(key, level, more) {
        return Object.assign({ key: key, name: "Alert " + key, enabled: true, level: level,
                               banner: true, live: true, state: "off", stale: false,
                               unavailable: false, fired: 0, value: 100, unit: "°C",
                               decimals: 0, staleSinceMs: -1 }, more || {})
    }
    function made(rows, props) {
        const m = createTemporaryObject(rowsC, testCase)
        for (const r of rows)
            m.append(r)
        const b = createTemporaryObject(bandC, testCase.parent,
                                        Object.assign({ conditions: m }, props || {}))
        verify(b, "the band is made")
        settle(b)
        return { model: m, band: b }
    }
    // The band's own Qt.callLater, then a look at the clock now
    function settle(b) {
        wait(0)
        b.recompute()
    }
    // Row i fires: ON, its count up, at the clock's time
    function fire(t, i) {
        t.model.setProperty(i, "state", "on")
        t.model.setProperty(i, "fired", t.model.get(i).fired + 1)
        settle(t.band)
    }
    function off(t, i) {
        t.model.setProperty(i, "state", "off")
        settle(t.band)
    }
    function keys(b) { return b.rows.map(r => r.key).join(",") }
    function find(item, name) {
        const found = findChild(item, name)
        verify(found, name + " is there")
        return found
    }
    function same(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function test_noAlertShowsNoBand() {
        const t = made([row("a", "warning"), row("b", "danger")])
        verify(!t.band.shown, "nothing is ON")
        verify(!t.band.visible, "and nothing is drawn")
    }

    function test_dangerStandsAboveWarningThenTheNewest() {
        const t = made([row("w1", "warning"), row("d", "danger"), row("w2", "warning")])
        fire(t, 0)
        clock += 100
        fire(t, 2)
        compare(keys(t.band), "w2,w1", "the newest warning first")
        clock += 100
        fire(t, 1)
        compare(keys(t.band), "d,w2,w1", "a danger above newer warnings")
        compare(find(t.band, "alertBandName").text, "Alert d")
        clock += 100
        off(t, 0)
        fire(t, 0)
        compare(keys(t.band), "d,w1,w2", "a warning fired again is the newest")
    }

    function test_moreCountsTheOtherAlerts() {
        const t = made([row("a", "warning"), row("b", "warning"), row("c", "danger")])
        fire(t, 0)
        const more = find(t.band, "alertBandMore")
        verify(!more.visible, "one alert: no count")
        fire(t, 1)
        fire(t, 2)
        compare(t.band.more, 2)
        verify(more.visible)
        compare(more.text, "+2 more")
    }

    function test_aBriefFiringStaysThreeSeconds() {
        const t = made([row("a", "warning")])
        const start = clock
        fire(t, 0)
        clock = start + 1000
        off(t, 0)
        verify(t.band.shown, "OFF a second after its firing, still shown")
        const timer = find(t.band, "alertFloorTimer")
        verify(timer.running, "a timer for the floor's end")
        compare(timer.interval, 2000, "at the firing's 3 s")
        clock = start + 2999
        t.band.recompute()
        verify(t.band.shown, "shown to the floor's last ms")
        clock = start + 3000
        t.band.recompute()
        verify(!t.band.shown, "gone at 3 s from the firing")
        verify(!timer.running)
    }

    // A state that turns ON before its row reads live (one change bringing
    // both, the state first) is a firing still: its floor runs from then
    function test_aStateOnBeforeLiveStartsItsFloor() {
        const t = made([row("a", "warning")])
        fire(t, 0)
        off(t, 0)
        clock += 10000
        settle(t.band)
        verify(!t.band.shown, "an old firing, long past its floor")
        t.model.setProperty(0, "live", false)
        t.model.setProperty(0, "state", "on")
        t.model.setProperty(0, "live", true)
        settle(t.band)
        verify(t.band.shown)
        clock += 1000
        off(t, 0)
        verify(t.band.shown, "OFF a second on: inside the floor of the ON it went live in")
    }

    function test_anAlertStaysWhileOn() {
        const t = made([row("a", "warning")])
        fire(t, 0)
        clock += 60000
        t.band.recompute()
        verify(t.band.shown, "ON a minute on, still shown")
        verify(!find(t.band, "alertFloorTimer").running, "no floor to wait for while ON")
    }

    function test_aStaleAlertSaysNothingNewRead() {
        const t = made([row("a", "danger")])
        fire(t, 0)
        const stale = find(t.band, "alertBandStale")
        verify(!stale.visible, "fresh: no line")
        const read = new Date(2026, 10, 15, 8, 30, 5).getTime()
        t.model.setProperty(0, "staleSinceMs", read)
        t.model.setProperty(0, "stale", true)
        settle(t.band)
        verify(stale.visible)
        compare(stale.text, "nothing new read since 08:30:05")
        t.model.setProperty(0, "staleSinceMs", -1)
        settle(t.band)
        compare(stale.text, "nothing new read", "no time when none is known")
    }

    function test_onlyABannerAlertShows_data() {
        return [{ tag: "no banner", more: { banner: false } },
                { tag: "no level", more: { level: "" } },
                { tag: "turned off", more: { enabled: false } },
                { tag: "not available", more: { unavailable: true } },
                { tag: "not judged", more: { live: false } }]
    }
    function test_onlyABannerAlertShows(data) {
        const t = made([row("a", "warning", data.more)])
        fire(t, 0)
        verify(!t.band.shown, data.tag)
    }

    function test_aDisconnectEmptiesTheBand() {
        const t = made([row("a", "danger")])
        fire(t, 0)
        verify(t.band.shown)
        t.model.setProperty(0, "live", false)
        t.model.setProperty(0, "fired", 0)
        t.model.setProperty(0, "state", "off")
        settle(t.band)
        verify(!t.band.shown, "no session, no band, inside the floor too")
    }

    function test_theLevelsDifferByWordAndMark_data() { return palettes() }
    function test_theLevelsDifferByWordAndMark(data) {
        usePalette(data.palette)
        const t = made([row("w", "warning"), row("d", "danger")])
        fire(t, 0)
        const mark = find(t.band, "alertBandMark")
        const word = find(t.band, "alertBandLevel")
        verify(!mark.visible, "a warning has no mark")
        compare(word.text, "WARNING")
        same(t.band.color, Theme.alertWarnFill, "a warning's fill")
        same(word.color, Theme.textOnAlertWarn, "a warning's words")
        same(find(t.band, "alertBandName").color, Theme.textOnAlertWarn, "the name")
        fire(t, 1)
        verify(mark.visible, "a danger has the mark")
        compare(word.text, "DANGER")
        same(t.band.color, Theme.alertDangerFill, "a danger's fill")
        same(word.color, Theme.textOnAlertDanger, "a danger's words")
        same(mark.color, Theme.textOnAlertDanger, "the mark in the words' color")
        same(mark.cutout, Theme.alertDangerFill, "its point cut in the fill")
        same(t.band.border.color, Theme.hairline, "a hairline edge")
        compare(t.band.radius, Theme.radius)
    }

    function test_aLongNameElidesAndMoreStays() {
        const long = "A very long condition name that can't fit beside its value and count"
        const t = made([row("a", "warning", { name: long }), row("b", "warning")],
                       { width: 320 })
        fire(t, 1)
        clock += 10
        fire(t, 0)
        const name = find(t.band, "alertBandName")
        const more = find(t.band, "alertBandMore")
        verify(name.truncated, "the name elides first")
        verify(more.visible)
        const right = more.mapToItem(t.band, more.width, 0).x
        verify(right <= t.band.width + 0.5, "the count ends at " + right + " in " + t.band.width)
    }

    function test_theBandIsOneAccessibleNode() {
        const t = made([row("w", "warning"), row("d", "danger", { value: 112 })])
        fire(t, 0)
        fire(t, 1)
        compare(t.band.Accessible.role, Accessible.AlertMessage)
        compare(t.band.Accessible.name, "Danger: Alert d, at 112 °C, and 1 more")
        compare(find(t.band, "alertBandValue").text, "at 112 °C", "the firing's value, worded so")
        for (const name of ["alertBandLevel", "alertBandName", "alertBandValue", "alertBandMore"])
            verify(find(t.band, name).Accessible.ignored, name + " is no node of its own")
    }

    // The bridge's model on the demo: a banner alert on its RPM reaches the band
    function test_theDemosRpmAlertReachesTheBand() {
        Theme.now = () => new Date()
        const saved = Bridge.saveConditions(
            '{"havne_conditions":1,"conditions":[{"key":"rpm","name":"Engine on",' +
            '"when":{"signal":["id:1001"],"unit":"RPM","above":-1},' +
            '"alert":{"level":"danger","banner":true}}]}')
        verify(saved.ok, saved.words)
        const b = createTemporaryObject(bandC, testCase.parent)
        verify(!b.shown, "no session")
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        tryVerify(() => b.shown, 10000, "the alert reaches the band")
        compare(find(b, "alertBandName").text, "Engine on")
        verify(b.danger)
        Bridge.disconnectSession()
        tryVerify(() => !b.shown, 10000, "the band empties at the disconnect")
    }
}
