// File/layer: tests/qml/tst_dashboard.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboard (test_qml)

import QtQuick
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "Dashboard"
    when: windowShown

    Component { id: hubComponent; SignalHub {} }
    Component { id: pageComponent; DashboardPage { width: 1280; height: 800 } }

    // Each gauge's label and its key in Bridge.gaugeSignals, with the
    // name the mock set gives the signal
    readonly property var gaugeSignals: ({
        "Engine speed": ["rpm", "RPM"],
        "Vehicle speed": ["speed", "Vehicle Speed"],
        "Coolant": ["coolant", "Coolant Temp"],
        "Oil temp": ["oil", "Engine Oil Temp"],
        "IAT": ["iat", "Intake Air Temp"],
        "MAF": ["maf", "MAF"],
        "Throttle": ["throttle", "Throttle Position"],
        "Engine load": ["load", "Engine Load"]
    })

    // The IDs the gauge labeled `label` looks up
    function idsOf(label) {
        return Bridge.gaugeSignals[gaugeSignals[label][0]]
    }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
    }

    function cleanup() {
        Bridge.unitSystem = 0
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
    }

    function connect(uri) {
        compare(Bridge.connectSession(uri), "", uri + " connects")
    }

    // The page's gauges (GaugeBase items), by label
    function gauges(page) {
        const found = {}
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.label !== undefined && child.note !== undefined
                        && child.valueText !== undefined)
                    found[child.label] = child
                walk(child)
            }
        }
        walk(page)
        for (const label in gaugeSignals)
            verify(found[label], "the Dashboard has a gauge labeled " + label)
        return found
    }

    function test_hubFindsSignalsById() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        verify(hub.count > 0)
        compare(Object.keys(hub.byId).length, hub.count, "one entry per def")
        for (const label in gaugeSignals) {
            const entry = hub.lookup(idsOf(label))
            verify(entry, label + "'s signal is in the mock set")
            compare(entry.id, idsOf(label)[0], label + ": the mock's ID")
            compare(entry.name, gaugeSignals[label][1])
        }
        // The first ID the set has wins; none of them, null
        const rpm = idsOf("Engine speed")
        verify(hub.lookup([65535, rpm[0]]) === hub.lookup(rpm))
        compare(hub.lookup([65535]), null)
        compare(hub.lookup([]), null)
        compare(hub.lookup(undefined), null)
    }

    function test_hubEntryFollowsTheModel() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        const rpm = hub.lookup(idsOf("Engine speed"))
        compare(rpm.hasValue, false, "no value before the first batch")
        compare(rpm.ageMs, -1)
        tryVerify(() => rpm.hasValue, 3000, "a value arrives")
        verify(isFinite(rpm.value))
        verify(rpm.ageMs >= 0)
    }

    // A profile switch resets the model; the hub drops the old entries
    // and picks up the new set's
    function test_hubFollowsAProfileSwitch() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        const rpm = idsOf("Engine speed")
        verify(hub.lookup(rpm))

        cleanup()
        connect("mock://demo?profile=raw")
        compare(hub.count, 0, "raw decodes nothing")
        compare(Object.keys(hub.byId).length, 0)
        compare(hub.lookup(rpm), null)

        cleanup()
        connect("mock://demo")
        verify(hub.lookup(rpm), "RPM is back with the mock set")
    }

    function test_gaugesSayThereIsNoSession() {
        const page = createTemporaryObject(pageComponent, testCase)
        const all = gauges(page)
        for (const label in gaugeSignals) {
            compare(all[label].note, "no session", label)
            compare(all[label].hasValue, false, label)
            compare(all[label].valueText, "—", label)
        }
    }

    // Each gauge waits for its signal's first value, then shows it at the
    // signal's own decimal places, in its unit
    function test_gaugesShowTheirSignals() {
        const page = createTemporaryObject(pageComponent, testCase)
        const hub = createTemporaryObject(hubComponent, testCase)
        const all = gauges(page)
        connect("mock://demo")
        // connectSession returns before the first 100 ms batch is applied
        for (const label in gaugeSignals) {
            compare(all[label].note, "waiting for data", label)
            compare(all[label].hasValue, false, label)
        }

        tryVerify(() => Object.keys(gaugeSignals).every(label => all[label].hasValue),
                  3000, "every gauge gets a value")
        for (const label in gaugeSignals) {
            const g = all[label]
            const entry = hub.lookup(idsOf(label))
            compare(g.note, "", label)
            compare(g.decimals, entry.decimals, label + " decimals")
            if (entry.unit !== "")
                compare(g.unit, entry.unit, label + " unit")
            compare(g.valueText,
                    Number(entry.value).toLocaleString(Qt.locale(), 'f', entry.decimals),
                    label + " readout")
        }
    }

    // A set that names every def otherwise (the mock's, renamed): each
    // gauge still reads its signal, found by core's def ID
    function test_aRenamedSetStillDrivesEveryGauge() {
        const profile = Harness.registerRenamedMock()
        verify(profile !== "", "the renamed set registers")
        const page = createTemporaryObject(pageComponent, testCase)
        const hub = createTemporaryObject(hubComponent, testCase)
        const all = gauges(page)
        connect("mock://demo?profile=" + profile)
        tryVerify(() => Object.keys(gaugeSignals).every(label => all[label].hasValue),
                  3000, "every gauge gets a value")
        for (const label in gaugeSignals) {
            compare(all[label].note, "", label)
            const entry = hub.lookup(idsOf(label))
            verify(entry.name !== gaugeSignals[label][1], label + ": named otherwise")
        }
    }

    // Imperial: the gauges read mph and degrees F, their scales and zones
    // follow (written in decoded units, converted through core), and a
    // change of units clears the held peaks
    function test_imperialConvertsTheGaugesAndTheirScales() {
        const page = createTemporaryObject(pageComponent, testCase)
        const all = gauges(page)
        connect("mock://demo")
        tryVerify(() => Object.keys(gaugeSignals).every(label => all[label].hasValue),
                  3000, "every gauge gets a value")
        const speedo = all["Vehicle speed"]
        const coolant = all["Coolant"]
        compare(speedo.unit, "km/h")
        compare(speedo.to, 260)
        compare(coolant.to, 130)
        tryVerify(() => !isNaN(speedo.peakValue), 3000, "the speedo holds a peak")

        Bridge.unitSystem = 1
        compare(speedo.unit, "mph")
        compare(speedo.to, 160)
        verify(isNaN(speedo.peakValue), "the old units' peak is cleared")
        compare(coolant.unit, "°F")
        fuzzyCompare(coolant.from, 32, 1e-9)
        fuzzyCompare(coolant.to, 266, 1e-9)
        fuzzyCompare(coolant.dangerFrom, 239, 1e-9)
        compare(all["MAF"].unit, "lb/min")
        fuzzyCompare(all["MAF"].to, 300 * 0.132277357312033, 1e-9)
        compare(all["Engine speed"].unit, "RPM", "a unit with no imperial form stays")
        compare(all["Throttle"].unit, "%")

        Bridge.unitSystem = 0
        compare(speedo.unit, "km/h")
        compare(coolant.to, 130)
    }

    // The engine start replayed at 100x, so each 100 ms window covers
    // 10 s of the recording: the tach holds the recording's peak, the
    // bench sheet's 1,261, though its window doesn't end on it
    function test_peakHoldKeepsAPeakInsideAWindow() {
        const page = createTemporaryObject(pageComponent, testCase)
        const tach = gauges(page)["Engine speed"]
        connect("replay://" + Harness.replayFixtures
                + "/2026-10-25-engine-start?profile=obd2&speed=100")
        tryVerify(() => Math.abs(tach.peakValue - 1261.25) < 0.01, 20000,
                  "the tach holds the recording's peak")
        wait(500)   // through the replay's end
        fuzzyCompare(tach.peakValue, 1261.25, 0.01, "nothing past the peak")
        verify(tach.lowValue < tach.peakValue)
    }

    // From a session that fed every gauge to one whose profile has none
    // of their signals: each gauge lets go of its old signal
    function test_gaugesSayTheProfileLacksTheirSignal() {
        const page = createTemporaryObject(pageComponent, testCase)
        const all = gauges(page)
        connect("mock://demo")
        tryVerify(() => Object.keys(gaugeSignals).every(label => all[label].hasValue),
                  3000, "every gauge gets a value")

        cleanup()
        connect("mock://demo?profile=raw")
        wait(300)   // past a batch or two
        for (const label in gaugeSignals) {
            compare(all[label].note, "not in profile", label)
            compare(all[label].hasValue, false, label)
            compare(all[label].valueText, "—", label)
        }
    }

    // A phone on its side: the page beside the sidebar is narrow and
    // short, and each dial is sized to the width, so it would be taller
    // than the page (docs/bench-sessions/2026-10-28-android-2.md)
    function test_aWholeDialFitsAShortPage() {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { width: 500, height: 300 })
        const tach = findChild(page, "tachCard")
        verify(tach, "the tachometer's card")
        tryVerify(() => tach.height > 0, 2000, "the grid lays the card out")
        verify(tach.height <= page.height - 2 * Theme.pageMargin,
               "the dial fits the page: " + tach.height)
    }
}
