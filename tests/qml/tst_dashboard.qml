// File/layer: tests/qml/tst_dashboard.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboard (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

DashboardKit {
    id: testCase
    name: "Dashboard"

    function test_hubFindsSignalsByRef() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        verify(hub.count > 0)
        compare(Object.keys(hub.byRef).length, hub.count, "one entry per def")
        for (const label in gaugeSignals) {
            const entry = hub.lookup(refsOf(label))
            verify(entry, label + "'s signal is in the mock set")
            compare(entry.ref, refsOf(label)[0], label + ": the mock's ref")
            compare("id:" + entry.id, entry.ref, label + ": by the mock's ID")
            compare(entry.name, gaugeSignals[label][1])
        }
        // The first ref the set has wins; none of them, null
        const rpm = refsOf("Engine speed")
        verify(hub.lookup(["id:65535", rpm[0]]) === hub.lookup(rpm))
        compare(hub.lookup(["id:65535"]), null)
        compare(hub.lookup(["1001", 1001]), null, "only a ref matches")
        compare(hub.lookup([]), null)
        compare(hub.lookup(undefined), null)
    }

    // An entry carries the row's facts: the def's range (as displayed,
    // following the unit system), its kind, and Live's state
    function test_hubEntryCarriesTheRowsFacts() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        const speed = hub.lookup(refsOf("Vehicle speed"))
        verify(isFinite(speed.rangeLow) && isFinite(speed.rangeHigh))
        verify(speed.rangeLow < speed.rangeHigh)
        compare(speed.isText, false)
        compare(speed.isEnum, false)
        compare(speed.hidden, false)
        compare(speed.refusedCode, "")
        compare(speed.label, "", "no enum, no value label")
        compare(speed.unit, "km/h")
        const kmh = speed.rangeHigh
        Bridge.unitSystem = 1
        compare(speed.unit, "mph")
        fuzzyCompare(speed.rangeHigh, kmh / 1.609344, 1e-3, "the range follows")
    }

    function test_hubEntryFollowsTheModel() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        const rpm = hub.lookup(refsOf("Engine speed"))
        compare(rpm.hasValue, false, "no value before the first batch")
        compare(rpm.ageMs, -1)
        tryVerify(() => rpm.hasValue, 3000, "a value arrives")
        verify(isFinite(rpm.value))
        verify(rpm.ageMs >= 0)
    }

    // A def with no range reaches the hub as NaN at both ends, beside a
    // scaled def's finite range (a signal file's text and number)
    function test_aTextEntryHasNoRange() {
        const r = keptFile(JSON.stringify({
            havne_signals: 1, name: "Text and number", source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [
                { key: "part", name: "Part", kind: "text",
                  read: { service: "22", number: "D100" }, field: { byte: 0 } },
                { key: "temp", name: "Temp", offset: -40, unit: "°C",
                  read: { service: "22", number: "D101" }, field: { byte: 0, bits: 8 } }
            ]
        }))
        verify(r.ok, r.words)
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo?profile=" + r.profile)
        const part = hub.byRef["key:part"]
        verify(part, "the text def has its key")
        compare(part.isText, true)
        verify(isNaN(part.rangeLow) && isNaN(part.rangeHigh), "no range")
        const temp = hub.byRef["key:temp"]
        verify(temp, "the number def has its key")
        fuzzyCompare(temp.rangeLow, -40, 1e-9)
        fuzzyCompare(temp.rangeHigh, 215, 1e-9)
    }

    // A profile switch resets the model; the hub drops the old entries
    // and picks up the new set's
    function test_hubFollowsAProfileSwitch() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo")
        const rpm = refsOf("Engine speed")
        verify(hub.lookup(rpm))

        cleanup()
        connect("mock://demo?profile=raw")
        compare(hub.count, 0, "raw decodes nothing")
        compare(Object.keys(hub.byRef).length, 0)
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
            const entry = hub.lookup(refsOf(label))
            compare(g.note, "", label)
            compare(g.decimals, entry.decimals, label + " decimals")
            if (entry.unit !== "")
                compare(g.unit, entry.unit, label + " unit")
            compare(g.valueText,
                    Number(entry.value).toLocaleString(Qt.locale(), 'f', entry.decimals),
                    label + " readout")
        }
    }

    // The demo car answers Service 01 from its drive cycle: under obd2
    // each built-in gauge gets a value, Boost from MAP and baro, and none
    // reads "not sent by this ECU"
    function test_theDemoCarsGaugesGetValuesUnderObd2() {
        const page = createTemporaryObject(pageComponent, testCase)
        const all = gauges(page)
        connect("mock://demo?profile=obd2")
        for (const label in gaugeSignals) {
            tryVerify(() => all[label].hasValue, 3000, label + " gets a value")
            compare(all[label].note, "", label)
        }
    }

    // The refs the default pick takes from the page (pickRefs): one list
    // for each cell of every page, each holding the cell's refs and their
    // counterparts in the built-in sets, a blank cell's none
    function test_thePicksRefsAreEachCellsLookupRefs() {
        const page = pageOf({}, docOf([
            { kind: "digital", w: 2, h: 4, label: "Engine", refs: ["id:7003"] },
            { kind: "blank", w: 2, h: 4, label: "", refs: [] }]))
        compare(page.pickRefs.length, 2)
        for (const ref of Bridge.gaugeSignals.rpm)
            verify(page.pickRefs[0].indexOf(ref) >= 0, ref + " among " + page.pickRefs[0])
        compare(page.pickRefs[0][0], "id:7003", "the cell's own ref first")
        compare(page.pickRefs[1].length, 0, "a blank cell picks nothing")
    }

    // A gauge whose signal the session's focus leaves unasked (a log that
    // didn't tick it) says "not asked" in place of a last value that would
    // read as no new data; asked again, it shows its value. The mock asks
    // nothing, so the case marks the row as the bridge would.
    function test_aGaugeWhoseSignalIsntAskedSaysSo() {
        const page = createTemporaryObject(pageComponent, testCase)
        const hub = createTemporaryObject(hubComponent, testCase)
        const all = gauges(page)
        connect("mock://demo")
        const label = Object.keys(gaugeSignals)[0]
        tryVerify(() => all[label].hasValue, 3000, label + " gets a value")
        const name = hub.lookup(refsOf(label)).name
        Harness.markUnasked([name])
        tryCompare(all[label], "note", "not asked", 1000, label)
        compare(all[label].hasValue, false, label)
        Harness.markUnasked([])
        tryVerify(() => all[label].hasValue, 1000, label + " shows its value again")
        compare(all[label].note, "", label)
    }

    // A set registered with no signal file (the mock's, copied and
    // renamed) has no refs (ADR-0057): its rows show in Live, but no
    // gauge binds them, though they carry the mock's IDs
    function test_aSetWithNoFileHasNoRefs() {
        const profile = Harness.registerRenamedMock()
        verify(profile !== "", "the renamed set registers")
        const page = createTemporaryObject(pageComponent, testCase)
        const hub = createTemporaryObject(hubComponent, testCase)
        const all = gauges(page)
        connect("mock://demo?profile=" + profile)
        verify(hub.count > 0, "the set's rows are there")
        compare(Object.keys(hub.byRef).length, 0, "none has a ref")
        tryVerify(() => {
            for (let i = 0; i < hub.count; ++i) {
                if (hub.objectAt(i).hasValue) return true
            }
            return false
        }, 3000, "the set's rows get values")
        for (const label in gaugeSignals) {
            compare(all[label].note, "not in " + profile, label)
            compare(all[label].hasValue, false, label)
        }
    }

    // Under obd2 the built-in Battery and Lambda gauges read obd2's own
    // defs (voltage and PID 0x44's commanded ratio), by ID; Boost, which
    // obd2 has no signal for, reads nothing there
    function test_theBuiltInBatteryAndLambdaPairWithObd2() {
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("mock://demo?profile=obd2")
        const battery = hub.lookup(refsOf("Battery"))
        verify(battery, "obd2 has the battery voltage")
        compare(battery.ref, "id:7008")
        const lambda = hub.lookup(refsOf("Lambda"))
        verify(lambda, "obd2 has the commanded lambda")
        compare(lambda.ref, "id:11544")
        compare(lambda.name, "Commanded Lambda")
        const boost = hub.lookup(refsOf("Boost"))
        verify(boost, "obd2 has the computed boost")
        compare(boost.ref, "id:7011")
    }

    // A gauge binds by ref, never by name: under a signal file extending
    // obd2 whose own defs take the tach's label ("Engine speed") and a
    // name like obd2's def ("Rpm"), the tach reads obd2's def by its ID, and
    // the file's defs are theirs by key (a file's "RPM" would replace obd2's)
    function test_aGaugeBindsByRefNotByName() {
        const r = keptFile(JSON.stringify({
            havne_signals: 1, name: "Named alike", source: { name: "t" },
            extends: "obd2",
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [
                { key: "rpm_too", name: "Engine speed",
                  read: { service: "22", number: "D100" }, field: { byte: 0, bits: 8 } },
                { key: "rpm_mock", name: "Rpm",
                  read: { service: "22", number: "D101" }, field: { byte: 0, bits: 8 } }
            ]
        }))
        verify(r.ok, r.words)
        const page = createTemporaryObject(pageComponent, testCase)
        const hub = createTemporaryObject(hubComponent, testCase)
        const tach = gauges(page)["Engine speed"]
        connect("replay://" + Harness.replayFixtures
                + "/2026-10-25-engine-start?profile=" + r.profile + "&speed=100")
        compare(hub.byRef["key:rpm_too"].name, "Engine speed")
        compare(hub.byRef["key:rpm_mock"].name, "Rpm")
        const rpm = hub.lookup(refsOf("Engine speed"))
        compare(rpm.ref, "id:7003", "obd2's def, by its ID")
        tryVerify(() => tach.hasValue, 5000, "the tach reads it")
        compare(tach.valueText,
                Number(rpm.value).toLocaleString(Qt.locale(), 'f', rpm.decimals))
    }

    // A file's def that replaced a base one by name has the base's ID: a cell
    // that named the base def by ID reads the file's def, and so does one that
    // names it by key
    function test_aReplacingDefIsFoundByTheBaseIdAndByKey() {
        const r = keptFile(JSON.stringify({
            havne_signals: 1, name: "Replaces", source: { name: "t" },
            extends: "obd2",
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [
                { key: "revs", name: "RPM",
                  read: { service: "22", number: "D100" }, field: { byte: 0, bits: 8 } }
            ]
        }))
        verify(r.ok, r.words)
        const hub = createTemporaryObject(hubComponent, testCase)
        connect("replay://" + Harness.replayFixtures
                + "/2026-10-25-engine-start?profile=" + r.profile + "&speed=100")
        const byKey = hub.byRef["key:revs"]
        verify(byKey, "the file's def by its key")
        compare(byKey.id, 7003, "it has obd2's RPM ID")
        verify(hub.byRef["id:7003"] === byKey, "a cell naming obd2's RPM by ID reads it")
        compare(hub.lookup(["id:7003"]).name, "RPM")
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
        fuzzyCompare(all["MAF"].to, 500 * 0.132277357312033, 1e-9)
        compare(all["Engine speed"].unit, "RPM", "a unit with no imperial form stays")
        compare(all["Battery"].unit, "V")
        compare(all["Lambda"].unit, "\u03BB")
        compare(all["Boost"].unit, "psi")
        // -14.5 and 29 psi, the dial's ends snapped to its tick step
        compare(all["Boost"].from, -15)
        compare(all["Boost"].to, 30)

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
            compare(all[label].note, "not in raw", label)
            compare(all[label].hasValue, false, label)
            compare(all[label].valueText, "—", label)
        }

        // Back to a profile that has them: the same gauges read again
        cleanup()
        connect("mock://demo")
        tryVerify(() => Object.keys(gaugeSignals).every(label => all[label].hasValue),
                  3000, "the same gauges read again")
    }

    // A gauge without its signal names the vehicle's set that has it while
    // the session reads another, else the profile the session reads, by
    // the name the user knows it by
    function test_aGaugeWithoutItsSignalNamesWhatHasIt() {
        const file = { havne_signals: 1, name: "Extras", source: { name: "t" },
                       ecus: [{ id: "pcm", rx: "7E8" }],
                       signals: [{ key: "a", name: "A", read: { service: "22", number: "D100" },
                                   field: { byte: 0, bits: 8 } }] }
        const r = keptFile(JSON.stringify(file))
        verify(r.ok, "the file imports")
        const cellA = { kind: "digital", w: 2, h: 4, label: "A", refs: ["key:a"], unit: "", options: {} }
        const cellZ = { kind: "digital", w: 2, h: 4, label: "Z", refs: ["key:z"], unit: "", options: {} }
        const d = DashboardBook.builtIn("desktop")
        d.pages = [{ name: "Test", cells: [cellA, cellZ] }]
        const page = pageOf({}, d, true)
        page.pickedSets = [{ profile: r.profile, name: "Extras", variant: "" }]
        page.profileNames = { [r.profile]: "Extras" }
        const a = findChild(page, "dashCell_0_0")
        const z = findChild(page, "dashCell_0_1")

        connect("mock://demo")
        tryVerify(() => a.note === "needs Extras", 3000, "the vehicle's file has it: " + a.note)
        compare(z.note, "not in mock", "no set has it")

        // A set named as the session's profile is (another variant of the
        // same plugin) is named by its variant alone
        page.profileNames = { [r.profile]: "Extras", "mock": "Extras" }
        page.pickedSets = [{ profile: r.profile, name: "Extras", variant: "v2" }]
        tryVerify(() => a.note === "needs variant v2", 3000, a.note)
        page.profileNames = { [r.profile]: "Extras" }
        page.pickedSets = [{ profile: r.profile, name: "Extras", variant: "v2" }]
        tryVerify(() => a.note === "needs Extras", 3000, "no variant where the names differ: " + a.note)

        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        connect("mock://demo?profile=" + r.profile)
        tryVerify(() => z.note === "not in Extras", 3000, "the file's own name: " + z.note)
        verify(a.note.indexOf("needs") < 0 && a.note.indexOf("not in") < 0, "read here: " + a.note)
    }

    // A phone on its side: the page beside the sidebar is narrow and
    // short, and each dial is sized to the width, so it would be taller
    // than the page (docs/bench-sessions/2026-10-28-android-2.md)
    function test_aWholeDialFitsAShortPage() {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { width: 500, height: 300 })
        const tach = findChild(page, "dashCell_0_1")
        verify(tach, "the tachometer's card")
        tryVerify(() => tach.height > 0, 2000, "the grid lays the card out")
        verify(tach.height <= page.height - 2 * Theme.pageMargin,
               "the dial fits the page: " + tach.height)
    }
}
