// File/layer: tests/qml/tst_dashboard.qml | Qt Quick Test, QML tier
// Testing: SignalHub's name lookup over the bridge's signal model,
//          including a profile switch, and the Dashboard's Taps: which
//          signal each gauge reads and the note it shows with no
//          session, before the first value, with a value, and when the
//          profile lacks the signal (?profile=raw); imperial units on the
//          gauges, their scales and zones, the peaks cleared on a
//          change of units, and a peak held from inside a window (the
//          engine-start fixture replayed); on a phone on its side
//          (500 x 300 beside the sidebar) a whole dial fits the page.

import QtQuick
import QtTest
import QtTune
import QtTuneTest

TestCase {
    id: testCase
    name: "Dashboard"
    when: windowShown

    Component { id: hubComponent; SignalHub {} }
    Component { id: pageComponent; DashboardPage { width: 1280; height: 800 } }

    // Each gauge's label and the signal its Tap reads, under the names
    // the mock and obd2 sets share
    readonly property var gaugeSignals: ({
        "Engine speed": "RPM",
        "Vehicle speed": "Vehicle Speed",
        "Coolant": "Coolant Temp",
        "Oil temp": "Engine Oil Temp",
        "IAT": "Intake Air Temp",
        "MAF": "MAF",
        "Throttle": "Throttle Position",
        "Engine load": "Engine Load"
    })

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/QtTune\//)
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

    function test_hubFindsSignalsByName() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        verify(hub.count > 0)
        compare(Object.keys(hub.byName).length, hub.count, "one entry per signal name")
        for (const label in gaugeSignals) {
            const name = gaugeSignals[label]
            const entry = hub.lookup([name])
            verify(entry, name + " is in the mock set")
            compare(entry.name, name)
        }
        // The first name the set has wins; none of them, null
        verify(hub.lookup(["No such signal", "RPM"]) === hub.lookup(["RPM"]))
        compare(hub.lookup(["No such signal"]), null)
        compare(hub.lookup([]), null)
    }

    function test_hubEntryFollowsTheModel() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        const rpm = hub.lookup(["RPM"])
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
        verify(hub.lookup(["RPM"]))

        cleanup()
        connect("mock://demo?profile=raw")
        compare(hub.count, 0, "raw decodes nothing")
        compare(Object.keys(hub.byName).length, 0)
        compare(hub.lookup(["RPM"]), null)

        cleanup()
        connect("mock://demo")
        verify(hub.lookup(["RPM"]), "RPM is back with the mock set")
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
            const entry = hub.lookup([gaugeSignals[label]])
            compare(g.note, "", label)
            compare(g.decimals, entry.decimals, label + " decimals")
            if (entry.unit !== "")
                compare(g.unit, entry.unit, label + " unit")
            compare(g.valueText,
                    Number(entry.value).toLocaleString(Qt.locale(), 'f', entry.decimals),
                    label + " readout")
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
