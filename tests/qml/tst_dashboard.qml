// File/layer: tests/qml/tst_dashboard.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboard (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

TestCase {
    id: testCase
    name: "Dashboard"

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

    Component { id: hubComponent; SignalHub {} }
    Component { id: pageComponent; DashboardPage { width: 1280; height: 800 } }
    Component { id: fieldComponent; TextField { text: "abc" } }
    Component { id: readOnlyFieldComponent; TextField { text: "abc"; readOnly: true } }
    // An item that takes Right when it has the focus
    Component {
        id: keyTakerComponent
        Item {
            property int rights: 0
            Keys.onRightPressed: ++rights
        }
    }
    // The page's own store, to put each case's back
    Component {
        id: storeComponent
        Settings {
            category: "dashboards"
            property string phone: ""
            property string desktop: ""
            property int phonePage: 0
            property int desktopPage: 0
        }
    }

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

    // The refs the gauge labeled `label` looks up
    function refsOf(label) {
        return Bridge.gaugeSignals[gaugeSignals[label][0]]
    }

    // A binding error in the app's QML fails the test
    // The page's Settings holds a write half a second and stores it when
    // the page goes, so the store is put back once the last case's pages
    // are gone, through setValue, which writes at once
    function init() {
        wait(0)
        const store = createTemporaryObject(storeComponent, testCase)
        store.setValue("phone", "")
        store.setValue("desktop", "")
        store.setValue("phonePage", 0)
        store.setValue("desktopPage", 0)
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        failOnWarning(/^Model size of /)   // a Repeater handed a negative count
    }

    function cleanup() {
        Theme.night = "off"
        Theme.look = "dark"
        Theme.nightFrom = "19:00"
        Theme.nightUntil = "07:00"
        Theme.now = () => new Date()
        Theme.shortLayout = false
        Bridge.unitSystem = 0
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Bridge.useSignalFiles([])
        Bridge.deleteUnusedSignalFiles([])
        Harness.clearFakeKnobs()
    }

    // A page with these properties, showing `doc` when one is given (a
    // document passed as a property at creation would reach the page as
    // a variant list, not a JS array). In the window, an item's `visible`
    // is its own; under the TestCase it reads false
    function pageOf(props, doc, inWindow) {
        const page = createTemporaryObject(pageComponent,
                                           inWindow ? testCase.Window.window.contentItem
                                                    : testCase, props)
        if (doc)
            page.doc = doc
        return page
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
        const r = Bridge.importSignalText(JSON.stringify({
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
        Bridge.useSignalFiles([r.sha256])
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
            compare(all[label].note, "not in this profile: " + label, label)
            compare(all[label].hasValue, false, label)
        }
    }

    // A gauge binds by ref, never by name: under a signal file extending
    // obd2 whose own defs take the tach's label ("Engine speed") and the
    // name obd2 and the mock give its def ("RPM"), the tach reads obd2's
    // def by its ID, and the file's defs are theirs by key
    function test_aGaugeBindsByRefNotByName() {
        const r = Bridge.importSignalText(JSON.stringify({
            havne_signals: 1, name: "Named alike", source: { name: "t" },
            extends: "obd2",
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [
                { key: "rpm_too", name: "Engine speed",
                  read: { service: "22", number: "D100" }, field: { byte: 0, bits: 8 } },
                { key: "rpm_mock", name: "RPM",
                  read: { service: "22", number: "D101" }, field: { byte: 0, bits: 8 } }
            ]
        }))
        verify(r.ok, r.words)
        Bridge.useSignalFiles([r.sha256])
        const page = createTemporaryObject(pageComponent, testCase)
        const hub = createTemporaryObject(hubComponent, testCase)
        const tach = gauges(page)["Engine speed"]
        connect("replay://" + Harness.replayFixtures
                + "/2026-10-25-engine-start?profile=" + r.profile + "&speed=100")
        compare(hub.byRef["key:rpm_too"].name, "Engine speed")
        compare(hub.byRef["key:rpm_mock"].name, "RPM")
        const rpm = hub.lookup(refsOf("Engine speed"))
        compare(rpm.ref, "id:7003", "obd2's def, by its ID")
        tryVerify(() => tach.hasValue, 5000, "the tach reads it")
        compare(tach.valueText,
                Number(rpm.value).toLocaleString(Qt.locale(), 'f', rpm.decimals))
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
            compare(all[label].note, "not in this profile: " + label, label)
            compare(all[label].hasValue, false, label)
            compare(all[label].valueText, "—", label)
        }

        // Back to a profile that has them: the same gauges read again
        cleanup()
        connect("mock://demo")
        tryVerify(() => Object.keys(gaugeSignals).every(label => all[label].hasValue),
                  3000, "the same gauges read again")
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

    // ---- The renderer (ADR-0057 sections 3 and 4)

    // A document of the built-in page and `n - 1` more like it
    function pagesDoc(n) {
        const d = DashboardBook.builtIn("desktop")
        for (let i = 1; i < n; ++i)
            d.pages.push({ name: "Page " + (i + 1),
                           cells: JSON.parse(JSON.stringify(d.pages[0].cells)) })
        return d
    }

    // A one-page document of these cells
    function docOf(cells) {
        const d = DashboardBook.builtIn("desktop")
        d.pages = [{ name: "Test", cells: cells }]
        return d
    }

    // Every gauge on the page, each page's own
    function allGauges(page) {
        const found = []
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.sweeping !== undefined && child.valueText !== undefined)
                    found.push(child)
                walk(child)
            }
        }
        walk(page)
        return found
    }

    // The gauge a cell's Loader made, or null
    function gaugeOf(cell) {
        return findChild(cell, "gauge").item
    }

    // Every cell's gauge Loader, on every page
    function loaders(page) {
        const found = []
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i]
                if (child.objectName === "gauge" && child.status !== undefined)
                    found.push(child)
                walk(child)
            }
        }
        walk(page)
        return found
    }
    function allLoaded(page) {
        return loaders(page).every(l => l.status === Loader.Ready)
    }

    // Section 4's grid: the phone's 6 columns upright and 12 on its
    // side, the desktop's one per 220 px from 2 to 12, each cell where
    // DashboardBook.place puts it, a row a quarter of a column (the
    // desktop's 2 x 8 tach square), and no two cells overlapping
    function test_theGridFollowsTheForm_data() {
        return [
            { tag: "a desktop window", form: "desktop", w: 1280, h: 800, columns: 6 },
            { tag: "a narrow desktop window", form: "desktop", w: 500, h: 900, columns: 2 },
            { tag: "a desktop window under 2 columns", form: "desktop", w: 300, h: 900, columns: 2 },
            { tag: "a wide desktop window", form: "desktop", w: 1900, h: 1400, columns: 8 },
            { tag: "a 4K desktop window", form: "desktop", w: 3840, h: 2000, columns: 12 },
            { tag: "a desktop window over 12 columns", form: "desktop", w: 5000, h: 1400,
              columns: 12 },
            { tag: "a phone upright", form: "phone", w: 384, h: 832, columns: 6 },
            { tag: "a phone on its side", form: "phone", w: 832, h: 384, columns: 12 }
        ]
    }
    function test_theGridFollowsTheForm(data) {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { form: data.form, width: data.w, height: data.h })
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 2000, "the page is laid out")
        compare(view.columns, data.columns)
        const cells = page.pages[0].cells
        const boxes = DashboardBook.place(cells, data.columns)
        const pitchX = view.columnWidth + page.cellGap
        const shown = []
        for (let i = 0; i < cells.length; ++i) {
            const cell = findChild(page, "dashCell_0_" + i)
            verify(cell, "cell " + i)
            fuzzyCompare(cell.x, boxes[i].x * pitchX, 1e-6, "cell " + i + " x")
            fuzzyCompare(cell.y, boxes[i].y * view.pitch, 1e-6, "cell " + i + " y")
            fuzzyCompare(cell.width, boxes[i].w * view.columnWidth + (boxes[i].w - 1) * page.cellGap,
                         1e-6, "cell " + i + " width")
            verify(cell.x + cell.width <= view.gridWidth + 1e-6, "cell " + i + " in the grid")
            shown.push(cell)
        }
        for (let i = 0; i < shown.length; ++i) {
            for (let j = i + 1; j < shown.length; ++j) {
                const a = shown[i], b = shown[j]
                verify(a.x + a.width <= b.x + 1e-6 || b.x + b.width <= a.x + 1e-6
                       || a.y + a.height <= b.y + 1e-6 || b.y + b.height <= a.y + 1e-6,
                       "cells " + i + " and " + j + " don't overlap")
            }
        }
        const tach = findChild(page, "dashCell_0_1")
        const tallest = cells.reduce((m, c) => Math.max(m, c.h), 1)
        const cap = (Math.max(160, view.height - page.gridTop - page.margin) + page.cellGap) / tallest
        fuzzyCompare(view.pitch, Math.min(pitchX / 4, cap), 1e-6,
                     "a row is a quarter of a column, unless the tallest cell wouldn't fit")
        if (data.form === "desktop")
            fuzzyCompare(tach.height, tach.width, 1e-6, "the tach is square")
    }

    // The phone's dial and its first readout share the row under the
    // shift lights
    function test_thePhonesDialsShareARow() {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { form: "phone", width: 384, height: 832 })
        const tach = findChild(page, "dashCell_0_1")
        const speedo = findChild(page, "dashCell_0_2")
        tryVerify(() => tach.width > 0, 2000, "the grid lays the cells out")
        compare(tach.y, speedo.y, "side by side")
        verify(tach.x < speedo.x)
    }

    // The desktop's pages as tabs, and Left and Right; one page outside
    // edit mode has neither
    function tabOf(page, i) {
        return findChild(page, "dashPageTab_" + i)
    }
    function test_thePageStripAndKeys() {
        const page = pageOf({}, pagesDoc(3), true)
        const host = findChild(page, "dashPageStripHost")
        const swipe = findChild(page, "dashPages")
        verify(host.visible, "the strip shows with three pages")
        const strip = findChild(page, "dashPageStrip")
        verify(strip, "the strip")
        verify(!findChild(page, "dashIndicator").visible, "no indicator on the desktop")
        verify(!swipe.interactive, "the desktop's pages don't swipe")
        compare(tabOf(page, 0).text, "Engine")
        compare(tabOf(page, 2).text, "Page 3")
        verify(!tabOf(page, 3), "a tab per page")
        verify(!findChild(page, "dashPageAdd").visible, "no + tab outside edit mode")
        compare(page.pageIndex, 0)
        verify(tabOf(page, 0).checked)

        verify(waitForPolish(page.Window.window), "laid out")
        mouseClick(tabOf(page, 2))
        compare(page.pageIndex, 2)
        tryCompare(swipe, "currentIndex", 2)
        verify(tabOf(page, 2).checked)
        verify(!tabOf(page, 0).checked)

        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 2, "Right on the last page stays")
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 1)
        tryCompare(swipe, "currentIndex", 1)
        keyClick(Qt.Key_Left)
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 0, "Left on the first page stays")

        // With a tab focused the keys still page, one step each, and the
        // focus moves to the shown page's tab
        tabOf(page, 0).forceActiveFocus()
        verify(tabOf(page, 0).activeFocus)
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 1, "one step from a focused tab")
        verify(tabOf(page, 1).activeFocus, "the focus follows to the shown tab")
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 0)
        verify(tabOf(page, 0).activeFocus)
        tabOf(page, 0).focus = false

        // A focused text field keeps its keys, at its text's end too
        const field = createTemporaryObject(fieldComponent, testCase.Window.window.contentItem)
        field.forceActiveFocus()
        verify(field.activeFocus)
        field.cursorPosition = field.text.length
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 0, "a text field's Right isn't the page's")
        field.cursorPosition = 0
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 0, "nor its Left at its start")
        field.focus = false
        const readOnly = createTemporaryObject(readOnlyFieldComponent,
                                               testCase.Window.window.contentItem)
        readOnly.forceActiveFocus()
        verify(readOnly.activeFocus)
        readOnly.cursorPosition = readOnly.text.length
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 0, "a read-only field's Right isn't the page's either")
        readOnly.focus = false
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 1, "the field let go")

        // A page added keeps the one shown
        page.doc = pagesDoc(4)
        compare(page.pageIndex, 1)
        tryCompare(swipe, "count", 4)
        compare(swipe.currentIndex, 1, "the page shown stays shown")

        // With more than one page the keys are the page's, ahead of
        // the item that has the focus
        const taker = createTemporaryObject(keyTakerComponent, testCase.Window.window.contentItem)
        taker.forceActiveFocus()
        verify(taker.activeFocus)
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 2)
        compare(taker.rights, 0, "the page took Right")

        // A document of fewer pages keeps the index on one of them, and
        // with one the keys go on to the item that has the focus
        page.doc = pagesDoc(1)
        compare(page.pageIndex, 0)
        verify(!host.visible, "one page has no strip")
        page.startEditing()
        verify(host.visible, "edit mode shows it with one page")
        verify(findChild(page, "dashPageAdd").visible, "with the + tab")
        page.stopEditing()
        verify(!host.visible, "Done hides it again")
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 0)
        compare(taker.rights, 1, "one page leaves Right to the focused item")
    }

    // The phone's pages swipe, with the indicator above the tab bar
    function test_thePhonesPagesSwipe() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, pagesDoc(3), true)
        const swipe = findChild(page, "dashPages")
        const indicator = findChild(page, "dashIndicator")
        verify(swipe.interactive)
        verify(indicator.visible)
        verify(!findChild(page, "dashPageStrip"), "no strip on the phone")
        compare(indicator.count, 3)
        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 0, "the phone has no page keys")

        swipe.incrementCurrentIndex()
        compare(page.pageIndex, 1, "a swipe moves the page")
        compare(indicator.currentIndex, 1)
        page.goTo(2)
        tryCompare(swipe, "currentIndex", 2)
        page.doc = pagesDoc(1)
        verify(!indicator.visible, "one page has no indicator")
    }

    // Each form keeps the page it last showed; one the document no
    // longer has is its last page
    function test_theLastPageShownIsKept() {
        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", DashboardBook.serialize(pagesDoc(3)))
        const first = pageOf({}, null, false)
        compare(first.pages.length, 3, "the stored document")
        first.goTo(2)
        const store = findChild(first, "dashStore")
        compare(store.desktopPage, 2)
        compare(store.phonePage, 0, "the phone's is its own")
        first.destroy()
        wait(0)
        compare(Number(stored.value("desktopPage")), 2, "stored as the page goes")

        const again = pageOf({}, null, false)
        compare(again.pageIndex, 2, "the same page")
        tryCompare(findChild(again, "dashPages"), "currentIndex", 2)
        const phone = pageOf({ form: "phone", width: 384, height: 832 }, null, false)
        compare(phone.pageIndex, 0, "the phone's document has one page")
        phone.destroy()
        wait(0)

        // The phone's page goes under its own key
        stored.setValue("phone", DashboardBook.serialize(pagesDoc(3)))
        const phoneMany = pageOf({ form: "phone", width: 384, height: 832 }, null, false)
        phoneMany.goTo(1)
        const phoneStore = findChild(phoneMany, "dashStore")
        compare(phoneStore.phonePage, 1, "the phone's page")
        compare(phoneStore.desktopPage, 2, "the desktop's left as it was")
        phoneMany.destroy()
        wait(0)
        compare(Number(stored.value("phonePage")), 1, "stored as the page goes")
        compare(Number(stored.value("desktopPage")), 2)

        stored.setValue("desktopPage", 7)
        const past = pageOf({}, null, false)
        compare(past.pageIndex, 2, "past the end: the last page")
    }

    // A stored document this version can't use: the built-in layout,
    // and a line saying why until it's dismissed
    function test_aStoredDocumentThatCantBeUsedSaysSo_data() {
        return [
            { tag: "damaged", text: "{", words: "couldn't be read" },
            { tag: "newer", text: JSON.stringify({ format: 3, pages: [] }),
              words: "a newer version of Havne" }
        ]
    }
    function test_aStoredDocumentThatCantBeUsedSaysSo(data) {
        const plain = pageOf({}, null, true)
        verify(!findChild(plain, "dashProblem").visible, "nothing stored, nothing to say")
        compare(plain.problem, "none")
        plain.destroy()

        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", data.text)
        const page = pageOf({}, null, true)
        const line = findChild(page, "dashProblem")
        compare(page.problem, data.tag)
        verify(line.visible, "the line shows")
        verify(findChild(page, "dashProblemText").text.indexOf(data.words) >= 0,
               findChild(page, "dashProblemText").text)
        compare(JSON.stringify(page.doc), JSON.stringify(DashboardBook.builtIn("desktop")),
                "the built-in layout")
        mouseClick(findChild(page, "dashProblemDismiss"))
        verify(!line.visible, "dismissed")
        compare(stored.value("desktop"), data.text, "the stored text is kept")
    }

    // Dismissing hides the line for that text; a different stored text
    // is a different problem and shows it again
    function test_aChangedStoredTextShowsItsProblemAgain() {
        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", "{")
        const page = pageOf({}, null, true)
        const line = findChild(page, "dashProblem")
        verify(line.visible, "the line shows")
        mouseClick(findChild(page, "dashProblemDismiss"))
        verify(!line.visible, "dismissed")
        findChild(page, "dashStore").desktop = "["
        compare(page.problem, "damaged")
        verify(line.visible, "another damaged text shows the line again")
    }

    // A kind this version doesn't draw is an empty card saying so,
    // under the cell's label; the eight it draws load their gauge
    function test_aKindThisVersionDoesntDrawIsAnEmptyCard_data() {
        return [
            { tag: "unknown", kind: "gizmo", drawn: false },
            { tag: "segments", kind: "segments", drawn: true },
            { tag: "seven-segment", kind: "seven-segment", drawn: true },
            { tag: "sparkline", kind: "sparkline", drawn: true },
            { tag: "analog", kind: "analog", drawn: true },
            { tag: "ring", kind: "ring", drawn: true },
            { tag: "bar", kind: "bar", drawn: true },
            { tag: "digital", kind: "digital", drawn: true },
            { tag: "shift-light", kind: "shift-light", drawn: true }
        ]
    }
    function test_aKindThisVersionDoesntDrawIsAnEmptyCard(data) {
        const page = pageOf({}, docOf([{ kind: data.kind, w: 2, h: 4, label: "Odd", refs: ["id:1001"],
                          unit: "RPM" }]), true)
        const cell = findChild(page, "dashCell_0_0")
        verify(cell, "the cell is kept")
        const empty = findChild(cell, "unknownKind")
        compare(empty.visible, !data.drawn)
        compare(gaugeOf(cell) !== null, data.drawn)
        if (!data.drawn)
            compare(empty.text, "a gauge this version of Havne doesn't have")
        else
            compare(gaugeOf(cell).label, "Odd")
    }

    // Step 5's kinds and options reach their gauges: the cells' count and
    // curve, the digits, the sparkline's window, the bar standing up and
    // without its ticks, and the held peak on every kind
    function test_theNewKindsTakeTheirOptions_data() {
        return [
            { tag: "segments", kind: "segments", options: { cells: 12, rise: 2, peak: true },
              check: g => g.count === 12 && g.rise === 2 && g.showPeak },
            { tag: "seven-segment", kind: "seven-segment", options: { digits: 3 },
              check: g => g.count === 3 && !g.showPeak },
            { tag: "sparkline", kind: "sparkline", options: { window: 60 },
              check: g => g.span === 60 && g.historyLength === 600 },
            { tag: "a vertical bar", kind: "bar", options: { vertical: true, marks: false, peak: true },
              check: g => g.vertical && !g.marks && g.showPeak },
            { tag: "a bar's defaults", kind: "bar", options: {},
              check: g => !g.vertical && g.marks && !g.showPeak },
            { tag: "a ring's peak", kind: "ring", options: { peak: true }, check: g => g.showPeak },
            { tag: "a dial's peak", kind: "analog", options: { peak: true }, check: g => g.showPeak }
        ]
    }
    function test_theNewKindsTakeTheirOptions(data) {
        const page = pageOf({}, docOf([{ kind: data.kind, w: 2, h: 4, label: "Odd", refs: ["id:1001"],
                          unit: "RPM", options: data.options }]), true)
        const cell = findChild(page, "dashCell_0_0")
        verify(cell, "the cell is there")
        const g = gaugeOf(cell)
        verify(g !== null, "the gauge loads")
        verify(data.check(g), "the options reach the gauge")
    }

    // A sparkline samples only while its page is the one shown: the other
    // page's takes none (a SwipeView keeps it visible), the page left starts
    // its trace again, and the one reached starts sampling
    function test_aSparklineOnAnotherPageTakesNoSamples() {
        const cell = label => ({ kind: "sparkline", w: 2, h: 3, label: label,
                                 refs: refsOf("Engine speed"), unit: "RPM", options: { window: 10 } })
        const d = docOf([cell("First")])
        d.pages.push({ name: "Second", cells: [cell("Second")] })
        const page = pageOf({}, d, true)
        connect("mock://demo")
        const a = gaugeOf(findChild(page, "dashCell_0_0"))
        const b = gaugeOf(findChild(page, "dashCell_1_0"))
        verify(a && b, "both load (the neighbor loads at once)")
        tryVerify(() => a.history.length > 3, 5000, "the shown page samples")
        wait(300)
        compare(b.history.length, 0, "the other page takes none")
        page.goTo(1)
        tryVerify(() => b.history.length > 3, 5000, "the page reached samples")
        tryVerify(() => a.history.length === 0 || a.history.length <= 1, 2000, "the page left starts again")
    }

    // The header's line about the log (Bridge.logNotice): a refusal of
    // Start log and an end the user didn't ask for show in words, whole
    // at a phone's width, until Dismiss or the next log
    function test_theLogsNoticeShowsInTheHeaderUntilDismissed_data() {
        return [{ tag: "phone", form: "phone", width: 384 },
                { tag: "desktop", form: "desktop", width: 1280 }]
    }
    function test_theLogsNoticeShowsInTheHeaderUntilDismissed(data) {
        Bridge.clearLogNotice()
        const page = pageOf({ form: data.form, width: data.width, height: 800 }, undefined, true)
        const line = findChild(page, "dashLogNotice")
        const words = findChild(page, "dashLogNoticeText")
        verify(line && words, "the line is there")
        verify(!line.visible, "nothing to say")

        // A refusal: no session
        compare(Bridge.startLog(), false)
        compare(Bridge.logNotice, "Log not started: not connected")
        tryVerify(() => line.visible && words.width > 0, 2000, "the line shows")
        compare(words.text, Bridge.logNotice)
        compare(words.truncated, false, "whole at " + data.width + " px")
        verify(words.mapToItem(page, 0, 0).y > 0, "in the header, under the title row")
        findChild(page, "dashLogNoticeDismiss").clicked()
        compare(Bridge.logNotice, "")
        tryVerify(() => !line.visible, 2000, "Dismiss hides it")

        // A log the user's Disconnect closes, told; the next log clears it
        connect("mock://demo")
        compare(Bridge.startLog(), true)
        compare(Bridge.logNotice, "")
        verify(!line.visible, "a running log says nothing")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.connected, 5000, "disconnect ends")
        compare(Bridge.logNotice, "Log ended")
        tryVerify(() => line.visible, 2000, "the end shows")
        compare(words.text, "Log ended")
        connect("mock://demo")
        compare(Bridge.startLog(), true)
        compare(Bridge.logNotice, "", "the next log clears it")
        tryVerify(() => !line.visible, 2000)
        Bridge.stopLog()
        compare(Bridge.logNotice, "", "Stop log says nothing")
    }

    // Section 6: a cell over a row of another unit takes the row's
    // range and no zones, and its note names the unit it expected
    function test_aCellOverAnotherUnitTakesTheRowsRange() {
        const page = pageOf({}, docOf([{ kind: "analog", w: 2, h: 8, label: "Wrong", refs: ["id:1001"],
                          unit: "°C",
                          options: { from: 0, to: 130, majorStep: 10, dangerFrom: 115 } }]), true)
        const hub = createTemporaryObject(hubComponent, testCase)
        const g = gaugeOf(findChild(page, "dashCell_0_0"))
        compare(g.to, 130, "no row yet: the cell's own scale")
        compare(g.dangerFrom, 115)
        connect("mock://demo")
        const rpm = hub.lookup(["id:1001"])
        compare(g.note, "waiting for data (expected °C)")
        compare(g.from, rpm.rangeLow)
        compare(g.to, rpm.rangeHigh)
        verify(isNaN(g.dangerFrom), "no zones under another unit")
        compare(g.unit, "RPM")
        tryVerify(() => g.hasValue, 3000, "the value shows")
        compare(g.note, "")
    }

    // An enumeration's row shows its label on a digital gauge
    function test_anEnumerationShowsItsLabel() {
        const values = {}
        for (let raw = 0; raw < 256; ++raw)
            values[String(raw)] = "v" + raw
        const r = Bridge.importSignalText(JSON.stringify({
            havne_signals: 1, name: "Modes", source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "mode", name: "Mode", read: { service: "22", number: "1E1C" },
                        field: { byte: 0, bits: 8 }, kind: "enum", values: values }]
        }))
        verify(r.ok, r.words)
        Bridge.useSignalFiles([r.sha256])
        const page = pageOf({}, docOf([{ kind: "digital", w: 2, h: 4, label: "Mode", refs: ["key:mode"] }]), true)
        const hub = createTemporaryObject(hubComponent, testCase)
        const g = gaugeOf(findChild(page, "dashCell_0_0"))
        connect("mock://demo?profile=" + r.profile)
        tryVerify(() => g.hasValue, 5000, "the mode arrives")
        const entry = hub.byRef["key:mode"]
        verify(entry.isEnum)
        verify(entry.label !== "", "the row has a label")
        compare(g.valueText, entry.label)
    }

    // A value that stops coming dims (Theme.staleMs), and a session's
    // end clears it
    function test_aValueThatStopsComingDims() {
        const page = createTemporaryObject(pageComponent, testCase)
        const tach = gauges(page)["Engine speed"]
        connect("replay://" + Harness.replayFixtures
                + "/2026-10-25-engine-start?profile=obd2&speed=100")
        tryVerify(() => tach.hasValue, 5000, "the tach reads")
        tryVerify(() => tach.stale, 20000, "the replay's end leaves it stale")
        Bridge.disconnectSession()
        tryVerify(() => !tach.stale && !tach.hasValue, 5000, "no session, no value")
    }

    // The ECU's own answers: a PID its bitmaps leave out, and a DID it
    // refuses (the fake ECU, through the fake J2534 DLL)
    function test_theEcusAnswersAreTheNotes() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const r = Bridge.importSignalText(JSON.stringify({
            havne_signals: 1, name: "Refusals", source: { name: "t" }, extends: "obd2",
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "gone", name: "Gone", read: { service: "22", number: "F1FE" },
                        field: { byte: 0, bits: 8 } }]
        }))
        verify(r.ok, r.words)
        Bridge.useSignalFiles([r.sha256])
        Harness.armFakeKnob("ECUIO_FAKE_WITHHELD_PIDS", "0A,5C")
        const doc = DashboardBook.builtIn("desktop")
        doc.pages[0].cells.push({ kind: "digital", w: 2, h: 4, label: "Gone",
                                  refs: ["key:gone"] })
        const page = pageOf({}, doc, false)
        const all = gauges(page)
        const gone = gaugeOf(findChild(page, "dashCell_0_" + (doc.pages[0].cells.length - 1)))
        compare(Bridge.connectSession("j2534://" + Harness.fakeDllPath + "?profile="
                                      + r.profile + "&identify=0&diagnose=0"), "")
        tryCompare(all["Oil temp"], "note", "not sent by this ECU", 5000)
        tryCompare(gone, "note", "refused (7F 22 31)", 5000)
        tryVerify(() => all["Coolant"].hasValue, 5000, "the rest read")
    }

    // The sweep plays on the dials and the shift lights, and Reset
    // peaks clears every page's
    function test_theSweepAndThePeaksReachEveryPage() {
        const page = pageOf({}, pagesDoc(2), true)
        tryVerify(() => allLoaded(page), 10000, "the second page loads")
        const every = allGauges(page)
        connect("mock://demo")
        const swept = every.filter(g => g.sweeping)
        verify(swept.length > 0, "the sweep plays")
        for (let p = 0; p < 2; ++p) {
            const cells = page.pages[p].cells
            for (let i = 0; i < cells.length; ++i) {
                const g = gaugeOf(findChild(page, "dashCell_" + p + "_" + i))
                const dial = cells[i].kind !== "bar" && cells[i].kind !== "digital"
                compare(g.sweeping, dial, "page " + p + " " + cells[i].label)
            }
        }
        tryVerify(() => every.every(g => !g.sweeping), 5000, "the sweep ends")
        const tachs = every.filter(g => g.label === "Engine speed")
        compare(tachs.length, 2, "a tach on each page")
        tryVerify(() => tachs.every(g => !isNaN(g.peakValue)), 5000, "each holds a peak")
        mouseClick(findChild(page, "resetPeaks"))
        verify(tachs.every(g => isNaN(g.peakValue)), "both cleared")
    }

    // A page whose gauges load after the sweep was requested sweeps them
    // as they load while the sweep is on, and shows them as they are once
    // it is over. The last page holds one dial, so it has one gauge to watch
    function test_aGaugeLoadedAfterTheSweepSweeps_data() {
        return [
            { tag: "the sweep is on", sweptAt: "now", sweeps: true },
            { tag: "the sweep is over", sweptAt: "never", sweeps: false }
        ]
    }
    function test_aGaugeLoadedAfterTheSweepSweeps(data) {
        const d = pagesDoc(4)
        d.pages[3].cells = d.pages[3].cells.filter(c => c.kind === "analog").slice(0, 1)
        const page = pageOf({}, d, true)
        const loader = findChild(findChild(page, "dashCell_3_0"), "gauge")
        verify(loader.asynchronous, "the last page loads later")
        // Set before the event loop runs, so the load is still to come
        page.sweepMs = 600000
        page.sweptAt = data.sweptAt === "now" ? Date.now() : 0
        tryCompare(loader, "status", Loader.Ready, 10000)
        if (data.sweeps) {
            verify(loader.item.sweeping, "the gauge sweeps as it loads")
        } else {
            wait(100)
            verify(!loader.item.sweeping, "the gauge shows its value")
        }
    }

    // A page and its neighbors load at once, so a swipe never drags in
    // an empty card; a page two or more away loads between frames
    function test_theNeighborsOfTheShownPageLoadAtOnce() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, pagesDoc(5), true)
        const at = p => findChild(findChild(page, "dashCell_" + p + "_0"), "gauge")
        for (let p = 0; p < 5; ++p)
            compare(at(p).asynchronous, p > 1, "page " + p + " from page 0")
        compare(at(0).status, Loader.Ready, "the shown page")
        compare(at(1).status, Loader.Ready, "the next page, at once")
        page.goTo(2)
        for (let p = 0; p < 5; ++p)
            compare(at(p).asynchronous, p === 0 || p === 4, "page " + p + " from page 2")
        for (let p = 1; p <= 3; ++p)
            tryCompare(at(p), "status", Loader.Ready, 5000, "page " + p)
    }

    // A full document (every page, every cell) draws its first frame
    // with only the near pages' gauges made; the rest load between
    // frames, and a page moved to stops loading later
    function test_aFullDocumentsOtherPagesLoadLater() {
        const d = DashboardBook.builtIn("desktop")
        const base = d.pages[0].cells
        d.pages = []
        for (let p = 0; p < DashboardBook.maxPages; ++p) {
            const cells = []
            for (let i = 0; i < DashboardBook.maxCells; ++i)
                cells.push(JSON.parse(JSON.stringify(base[i % base.length])))
            d.pages.push({ name: "Page " + (p + 1), cells: cells })
        }
        compare(DashboardBook.documentProblem(d), "", "a document the book takes")
        const page = pageOf({}, d, true)
        const at = (p, i) => findChild(findChild(page, "dashCell_" + p + "_" + i), "gauge")
        for (let i = 0; i < DashboardBook.maxCells; ++i)
            compare(at(0, i).status, Loader.Ready, "the shown page's cell " + i)
        const last = DashboardBook.maxPages - 1
        verify(at(last, 0).asynchronous, "a far page's loads later")
        verify(at(5, 0).asynchronous, "so does a middle one's")
        page.goTo(5)
        verify(!at(5, 0).asynchronous, "the page moved to loads at once")
        for (let i = 0; i < DashboardBook.maxCells; ++i)
            tryCompare(at(5, i), "status", Loader.Ready, 10000, "the page moved to, cell " + i)
        tryVerify(() => allLoaded(page), 30000, "every page loads")
        compare(loaders(page).length, DashboardBook.maxPages * DashboardBook.maxCells)
    }

    // The lit parts take Theme's gaugeValue in every theme, not a color
    // derived from the primary
    function test_gaugesTakeTheirValueColorFromTheTheme() {
        for (const name of Theme.paletteNames) {
            usePalette(name)
            const page = createTemporaryObject(pageComponent, testCase)
            const tach = gauges(page)["Engine speed"]
            verify(Qt.colorEqual(tach.accent, Theme.gaugeValue),
                   name + ": " + tach.accent + ", expected " + Theme.gaugeValue)
        }
        usePalette("dark")
    }

    // A ref that names an inherited object key is no signal
    function test_hubIgnoresInheritedKeys() {
        const hub = createTemporaryObject(hubComponent, testCase)
        compare(hub.lookup(["constructor"]), null)
        compare(hub.lookup(["toString", "__proto__"]), null)
    }

    // ---- The Night switch (ADR-0058 section 6)

    // The switch: one per page, at the end of the desktop's link state or
    // of the phone's tool row
    function nightSwitchOf(page) {
        return findChild(page, "nightSwitch")
    }

    function test_theSwitchFlipsTheModeInOnAndOff() {
        const page = pageOf({ form: "desktop" }, null, true)
        const sw = nightSwitchOf(page)
        verify(sw, "the switch is on the page")
        compare(sw.text, "Night")
        compare(sw.checked, false, "off")
        sw.clicked()
        compare(Theme.night, "on")
        compare(Theme.nightActive, true)
        compare(sw.checked, true, "checked while night is on")
        sw.clicked()
        compare(Theme.night, "off")
        compare(sw.checked, false)
    }

    function test_theSwitchHoldsAnOverrideUnderASchedule() {
        const page = pageOf({ form: "desktop" }, null, true)
        const sw = nightSwitchOf(page)
        Theme.now = () => new Date(2026, 10, 10, 18, 0, 0, 0)
        Theme.night = "schedule"
        Theme.refreshNight()
        compare(sw.checked, false, "the schedule says off at 18:00")
        sw.clicked()
        compare(Theme.night, "schedule", "the mode is kept")
        compare(Theme.nightOverride, "on")
        compare(sw.checked, true, "checked follows nightActive")
        compare(sw.Accessible.name, "Night is on until 07:00. Turn it off")
        sw.clicked()
        compare(Theme.nightOverride, "")
        compare(sw.checked, false)
        compare(sw.Accessible.name, "Night is off until 19:00. Turn it on")
    }

    // The desktop's switch ends its link state; the phone's the tool row
    function test_theSwitchSitsAtTheEndOfItsRow_data() {
        return [
            { tag: "desktop", form: "desktop", short: false, shown: "linkInRow",
              empty: "toolRowNight" },
            { tag: "phone upright", form: "phone", short: false, shown: "toolRowNight",
              empty: "linkInRow" },
            { tag: "phone on its side", form: "phone", short: true, shown: "toolRowNight",
              empty: "linkInRow" }
        ]
    }
    function test_theSwitchSitsAtTheEndOfItsRow(data) {
        Theme.shortLayout = data.short
        const page = pageOf({ form: data.form }, null, true)
        connect("mock://demo")
        const loader = findChild(page, data.shown)
        tryVerify(() => loader.active && loader.item, 3000, data.shown + " is shown")
        const sw = nightSwitchOf(page)
        verify(sw, "the switch exists")
        if (data.shown === "linkInRow") {
            compare(sw.parent, loader.item, "in the link state's row")
            compare(Array.from(loader.item.children).indexOf(sw), loader.item.children.length - 1,
                    "at the link state's end")
        } else {
            compare(loader.item, sw, "the tool row's own")
            const row = loader.parent
            const shown = Array.from(row.children).filter(c => c.visible)
            compare(shown[shown.length - 1], loader, "at the row's end")
        }
        verify(!findChild(page, data.empty).active, "the other place is empty")
    }

    // ---- The phone's tool row and joined panel (ADR-0057's appendix,
    // section 3)

    function test_thePhonesToolRow_data() {
        return [{ tag: "upright", short: false }, { tag: "on its side", short: true }]
    }
    function test_thePhonesToolRow(data) {
        Theme.shortLayout = data.short
        const page = pageOf({ form: "phone", width: 384, height: 832 }, null, true)
        connect("mock://demo")
        tryVerify(() => findChild(page, "toolRowNight").item, 3000, "the row is laid out")
        verify(waitForPolish(page), "laid out")
        const reset = findChild(page, "resetPeaks"), edit = findChild(page, "dashEdit")
        const night = nightSwitchOf(page), start = findChild(page, "toolRowStart")
        const row = reset.parent
        // The session's shorter Drive words lay the row out again
        settleToolRow(row)
        verify(!row.children[0].visible, "no title: the tab bar names the page")
        verify(!findChild(page, "linkLabel"), "no link state: the top bar has it")
        verify(!findChild(page, "linkInRow").active, "no link state in the row")
        verify(start.x + start.width <= reset.x + 0.5, "the start of the row is free")
        const end = night.mapToItem(row, 0, 0).x + night.width
        fuzzyCompare(row.x + end, row.parent.width, 0.5, "the tools sit at the page's right")
        fuzzyCompare(edit.x - (reset.x + reset.width), 8, 0.5, "8 between the tools")
        verify(reset.x + reset.width <= edit.x + 0.5, "Reset peaks, then Edit")
        verify(edit.x + edit.width <= night.mapToItem(row, 0, 0).x + 0.5, "Edit, then Night")
        verify(reset.flat, "Reset peaks is quiet")
        verify(!edit.flat && !edit.highlighted, "Edit is secondary")
        verify(night.checkable, "Night is a state")
        fuzzyCompare(row.height, Theme.touchTarget, 0.5, "one touch target tall")
        // Editing replaces Edit with its bar; the rest of the row stays
        page.startEditing()
        verify(!edit.visible, "Edit hides while editing")
        verify(reset.visible && night.visible, "the rest stay")
        page.stopEditing()
    }

    function test_theDesktopKeepsItsTitleAndLinkState() {
        const page = pageOf({ form: "desktop" }, null, true)
        connect("mock://demo")
        const row = findChild(page, "resetPeaks").parent
        verify(row.children[0].visible, "the title")
        compare(row.children[0].text, qsTr("Dashboard"))
        tryVerify(() => findChild(page, "linkInRow").item, 3000, "the link state")
        verify(!findChild(page, "toolRowStart").visible, "no empty start")
    }

    function jointDoc() {
        const cell = (w, h, kind, label) => ({
            kind: kind || "digital", w: w, h: h, label: label, refs: ["id:1001"], unit: "RPM",
            options: { from: 0, to: 8000 }
        })
        return { format: 2, pages: [{ name: "P", cells: [
            cell(3, 12, "digital", "A"), cell(3, 12, "digital", "B"),
            cell(6, 6, "digital", "C"), cell(3, 12, "blank", "")
        ] }] }
    }
    function seamsOf(page) {
        const grid = findChild(page, "dashPanelSurface").parent
        return Array.from(grid.children).filter(c => c.objectName === "dashSeam")
    }

    function test_thePhoneIsOnePanel() {
        checkOnePanel(pageOf({ form: "phone", width: 384, height: 832 }, jointDoc(), true))
    }

    // A blank cell draws nothing, so the panel's surface shows there; the
    // panel ends at the last row used
    function test_aJointPanelEndsWithItsLastRow() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, jointDoc(), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        const surface = findChild(page, "dashPanelSurface")
        fuzzyCompare(surface.height, view.rows * view.pitch, 1e-6, "the height of its rows")
    }

    // The cell held up covers what it crosses; a blank has no fill to show
    function test_aLiftedCellIsOpaqueOnThePanel() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, jointDoc(), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        const c = findChild(page, "dashCell_0_0"), blank = findChild(page, "dashCell_0_3")
        page.held = 0
        tryVerify(() => c.lifted, 1000, "lifted")
        compare(c.color, Theme.surface, "the held cell is opaque")
        compare(c.radius, Theme.radiusCard, "a card while held")
        page.held = 3
        tryVerify(() => blank.lifted, 1000, "lifted")
        compare(Qt.colorEqual(blank.color, "transparent"), true, "a blank stays empty")
        page.cancelHold()
        compare(Qt.colorEqual(c.color, "transparent"), true, "put back")
    }

    // The phone's panel's checks, for either form
    function checkOnePanel(page) {
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        compare(page.cellGap, 0, "the cells meet")
        const surface = findChild(page, "dashPanelSurface")
        verify(surface.visible, "one surface")
        compare(surface.radius, Theme.radiusCard)
        compare(surface.border.width, page.hairlineWidth, "a hairline edge")
        compare(surface.border.color, Theme.hairline)
        compare(surface.color, Theme.surface)
        verify(!surface.gradient, "no gradient")
        for (const i of [0, 1, 2]) {
            const c = findChild(page, "dashCell_0_" + i)
            compare(c.radius, 0, "cell " + i + " has no radius of its own")
            compare(c.border.width, 0, "cell " + i + " has no border of its own")
            compare(Qt.colorEqual(c.color, "transparent"), true, "cell " + i + " has no fill")
            verify(!c.gradient, "cell " + i + " has no gradient")
        }
        const a = findChild(page, "dashCell_0_0"), b = findChild(page, "dashCell_0_1")
        fuzzyCompare(a.x + a.width, b.x, 1e-6, "neighbors meet across")
        const c2 = findChild(page, "dashCell_0_2")
        fuzzyCompare(a.y + a.height, c2.y, 1e-6, "neighbors meet down")
        // One line per seam, the book's, on the boundary between the cells
        const lines = seamsOf(page)
        const seams = DashboardBook.seams(view.placed, view.boxes, view.columns)
        compare(lines.length, seams.length, "a line for each seam")
        verify(lines.length > 0, "there are lines")
        for (let i = 0; i < lines.length; ++i) {
            const s = seams[i], l = lines[i]
            compare(Qt.colorEqual(l.color, Theme.hairline), true, "hairline color")
            const at = (s.vertical ? s.x * view.columnWidth : s.y * view.pitch)
            const center = (s.vertical ? l.x + l.width / 2 : l.y + l.height / 2)
            verify(Math.abs(center - at) <= page.hairlineWidth, "line " + i + " on its boundary")
            const run = s.length * (s.vertical ? view.pitch : view.columnWidth)
            fuzzyCompare(s.vertical ? l.height : l.width, run, 1e-6, "line " + i + " spans its run")
        }
    }

    // The desktop's gauges are one panel too, as the phone's
    function test_theDesktopIsOnePanelToo() {
        const page = pageOf({ form: "desktop", width: 1280, height: 800 }, jointDoc(), true)
        checkOnePanel(page)
        const edit = findChild(page, "dashEdit")
        verify(!edit.compact, "a wide window's Edit isn't compact")
        compare(edit.horizontalPadding, 8, "Edit keeps a button's padding")
    }

    // Unequal heights in a row: outside edit mode the shorter cell fills
    // the empty squares under it, and the stored layout stays
    function fitDoc(columns, blankUnder) {
        const cell = (w, h, kind, label) => ({
            kind: kind, w: w, h: h, label: label, refs: ["id:1001"], unit: "RPM",
            options: { from: 0, to: 8000 }
        })
        const head = [cell(2, 6, "digital", "short"), cell(columns - 2, 7, "digital", "tall")]
        const under = cell(columns, 3, "digital", "under")
        return { format: 2, pages: [{ name: "P", cells: blankUnder
            ? head.concat([cell(2, 1, "blank", ""), under])
            : head.concat([under, cell(2, 3, "blank", "")]) }] }
    }
    function test_aShortCellFillsTheEmptySquaresUnderIt_data() {
        return [{ tag: "6 columns", w: 384, h: 832, columns: 6, blankUnder: false },
                { tag: "12 columns", w: 832, h: 384, columns: 12, blankUnder: false },
                { tag: "6 columns, a blank under it", w: 384, h: 832, columns: 6, blankUnder: true },
                { tag: "12 columns, a blank under it", w: 832, h: 384, columns: 12, blankUnder: true }]
    }
    function test_aShortCellFillsTheEmptySquaresUnderIt(data) {
        const page = pageOf({ form: "phone", width: data.w, height: data.h },
                            fitDoc(data.columns, data.blankUnder), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        compare(view.columns, data.columns)
        const short = findChild(page, "dashCell_0_0")
        const blank = findChild(page, data.blankUnder ? "dashCell_0_2" : "dashCell_0_3")
        const drawn = data.blankUnder ? 6 : 7
        fuzzyCompare(short.height, drawn * view.pitch, 1e-6, "down to the next cell, never a blank")
        fuzzyCompare(blank.height, (data.blankUnder ? 1 : 3) * view.pitch, 1e-6, "a blank stays its size")
        compare(DashboardBook.seams(view.placed, view.boxes, view.columns).length, seamsOf(page).length)
        if (!data.blankUnder)
            verify(!seamsOf(page).some(l => l.width > l.height && Math.abs(l.y - 6 * view.pitch) < 1
                                            && l.x < view.columnWidth),
                   "no line closes off a strip under it")
        page.held = 0
        tryVerify(() => Math.abs(short.height - 6 * view.pitch) < 1e-6, 1000, "a held cell's true size")
        page.cancelHold()
        fuzzyCompare(short.height, drawn * view.pitch, 1e-6, "back after the hold")
        page.startEditing()
        fuzzyCompare(short.height, 6 * view.pitch, 1e-6, "edit mode's true size")
        compare(page.doc.pages[0].cells[0].h, 6, "the layout is unchanged")
    }
    // The desktop's short cell fills the empty squares under it too, and
    // the stored layout stays
    function test_theDesktopStretchesAShortCellToo() {
        const page = pageOf({ form: "desktop", width: 1280, height: 800 }, fitDoc(6, false), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        compare(view.columns, 6)
        const short = findChild(page, "dashCell_0_0")
        fuzzyCompare(short.height, 7 * view.pitch, 1e-6, "down to the next cell")
        page.held = 0
        tryVerify(() => Math.abs(short.height - 6 * view.pitch) < 1e-6, 1000, "a held cell's true size")
        page.cancelHold()
        page.startEditing()
        fuzzyCompare(short.height, 6 * view.pitch, 1e-6, "edit mode's true size")
        compare(page.doc.pages[0].cells[0].h, 6, "the layout is unchanged")
        page.stopEditing()
    }

    // The strip's shown tab meets the panel: the panel's top at the
    // strip's foot, the tab's sheet a hairline over it, the strip painted
    // over the pages; an edit panel open sits a gap lower
    function test_thePanelMeetsTheStrip() {
        const page = pageOf({ form: "desktop", width: 1280, height: 800 }, pagesDoc(2), true)
        const surface = findChild(page, "dashPanelSurface")
        const host = findChild(page, "dashPageStripHost")
        tryVerify(() => surface.height > 0 && host.height > 0, 3000, "laid out")
        verify(waitForPolish(page.Window.window), "laid out")
        verify(page.stripMeetsPanel)
        compare(page.gridTop, 0)
        const foot = host.mapToItem(null, 0, host.height).y
        verify(Math.abs(surface.mapToItem(null, 0, 0).y - foot) <= page.hairlineWidth,
               "the panel starts at the strip's foot: " + surface.mapToItem(null, 0, 0).y + " and " + foot)
        const sheet = findChild(findChild(page, "dashPageTab_0"), "tabSheet")
        verify(sheet.visible, "the shown tab's sheet")
        fuzzyCompare(sheet.mapToItem(null, 0, sheet.height).y, foot + page.hairlineWidth, 1e-6,
                     "a hairline past the strip, over the panel's top line")
        verify(!findChild(findChild(page, "dashPageTab_1"), "tabSheet").visible, "only the shown one")
        verify(findChild(page, "dashPageStripHost").parent.z > findChild(page, "dashPages").z,
               "the strip paints over the pages")
        compare(surface.topLeftRadius, 0, "the first tab runs straight into the panel")
        page.goTo(1)
        tryVerify(() => findChild(page, "dashPageTab_1").checked, 1000, "the second page")
        compare(findChild(page, "dashPanelSurface").topLeftRadius, Theme.radiusCard,
                "the first page's corner keeps its radius")
        const second = findChild(findChild(page, "dashPage_1"), "dashPanelSurface")
        verify(second, "the second page's surface")
        compare(second.topLeftRadius, Theme.radiusCard, "under another tab the corner is round")

        page.goTo(0)
        page.select(0)
        compare(page.panel, "cell")
        verify(!page.stripMeetsPanel, "an edit panel doesn't meet the strip")
        compare(page.gridTop, page.topGap)
        const panel = findChild(page, "dashCellPanel")
        tryVerify(() => panel && panel.height > 0, 3000, "the panel")
        fuzzyCompare(panel.mapToItem(null, 0, 0).y - foot, page.topGap, 1, "a gap under the strip")
        page.closePanel()

        page.doc = pagesDoc(1)
        verify(!host.visible)
        compare(page.gridTop, page.topGap, "one page: the header's gap")
    }

    // A row of tabs wider than the strip scrolls to the shown one, and the
    // wheel moves it
    function test_aLongStripScrollsToTheShownTab() {
        const page = pageOf({ form: "desktop", width: 560, height: 760 }, pagesDoc(10), true)
        const view = findChild(page, "dashPageStripView")
        tryVerify(() => view.width > 0, 3000, "laid out")
        verify(view.contentWidth > view.width, "ten tabs are wider than the strip")
        compare(view.contentX, 0)
        page.goTo(9)
        const last = findChild(page, "dashPageTab_9")
        tryVerify(() => last.x >= view.contentX - 0.5 && last.x + last.width <= view.contentX + view.width + 0.5,
                  2000, "the shown tab scrolled into view")
        page.goTo(0)
        tryCompare(view, "contentX", 0, 2000, "and back")
        // Near the strip's start: the test window is narrower than the page
        mouseWheel(view, 20, view.height / 2, 0, -120)
        tryVerify(() => view.contentX > 0, 1000, "the wheel moves the row")

        // Tabs at their widest keep the row's width when the shown one turns
        // bold, so the page change alone brings the tab into view
        const d = pagesDoc(10)
        for (let i = 0; i < d.pages.length; ++i)
            d.pages[i].name = "A page name long enough to be cut " + i
        page.doc = d
        page.goTo(0)
        tryCompare(view, "contentX", 0, 2000, "at the start")
        const width = view.contentWidth
        page.goTo(8)
        const eighth = findChild(page, "dashPageTab_8")
        tryVerify(() => eighth.x >= view.contentX - 0.5
                        && eighth.x + eighth.width <= view.contentX + view.width + 0.5,
                  2000, "the shown tab scrolled into view")
        compare(view.contentWidth, width, "the row kept its width")
    }

    // A long name is cut at the tab's widest and read whole
    function test_aLongNameElidesAndIsReadWhole() {
        const d = pagesDoc(2)
        const name = "A page name thirty-four chars long"
        d.pages[1].name = name
        const page = pageOf({ form: "desktop", width: 1280, height: 800 }, d, true)
        const t = findChild(page, "dashPageTab_1")
        tryVerify(() => t.width > 0, 3000, "laid out")
        compare(t.width, 220, "at the widest")
        verify(t.contentItem.truncated, "the name is cut")
        compare(t.Accessible.name, name, "and read whole")
        verify(findChild(page, "dashPageTab_0").width >= 64, "a short name keeps the least width")
    }

    // A Digits card counts from its range; its own option wins
    function test_aDigitsCardsCountComesFromItsRange_data() {
        return [{ tag: "from the range", options: { from: 0, to: 100 }, count: 3 },
                { tag: "the option wins", options: { from: 0, to: 100, digits: 6 }, count: 6 }]
    }
    function test_aDigitsCardsCountComesFromItsRange(data) {
        const doc = { format: 2, pages: [{ name: "P", cells: [
            { kind: "seven-segment", w: 3, h: 6, label: "D", refs: ["id:1001"], unit: "RPM",
              options: data.options }] }] }
        const page = pageOf({ form: "phone", width: 384, height: 832 }, doc, true)
        tryVerify(() => allLoaded(page) && loaders(page)[0].item, 3000, "its gauge loads")
        compare(loaders(page)[0].item.count, data.count)
    }

    // Each hairline is a whole number of the display's pixels, and sits on them
    function test_aHairlineIsWholePixels_data() {
        const rows = []
        for (const form of ["phone", "desktop"])
            for (const z of [{ tag: "100 %", zoom: 1 }, { tag: "125 %", zoom: 1.25 },
                             { tag: "75 %", zoom: 0.75 }, { tag: "110 %", zoom: 1.1 },
                             { tag: "67 %", zoom: 0.67 }])
                rows.push({ tag: form + ", " + z.tag, zoom: z.zoom, form: form })
        return rows
    }
    function test_aHairlineIsWholePixels(data) {
        Theme.zoom = data.zoom
        const page = pageOf(data.form === "phone" ? { form: "phone", width: 384, height: 832 }
                                                  : { form: "desktop", width: 1280, height: 800 },
                            jointDoc(), true)
        const view = findChild(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        const ratio = (Screen.devicePixelRatio || 1) * Theme.zoom
        const onGlass = page.hairlineWidth * ratio
        fuzzyCompare(onGlass, Math.round(onGlass), 1e-9, "whole pixels on the glass: " + onGlass)
        verify(onGlass >= 1, "at least one")
        compare(findChild(page, "dashPanelSurface").border.width, page.hairlineWidth, "the edge matches")
        const lines = seamsOf(page)
        verify(lines.length > 0, "there are lines")
        for (const l of lines) {
            const p = l.mapToItem(null, 0, 0)
            const at = (l.width < l.height ? p.x : p.y) * ratio
            fuzzyCompare(at, Math.round(at), 1e-6, "a line on a whole pixel: " + at)
        }
        Theme.zoom = 1
    }

    Component { id: spyComponent; SignalSpy {} }

    // The desktop's link state drops the address where its row leaves it
    // no room (a narrow window): the header carries the address. Under 60
    // px the dot and the label fade rather than read "L...e"
    function test_theLinkStateDropsTheAddressInANarrowRow() {
        const page = pageOf({ form: "desktop", width: 1000, height: 600 }, null, true)
        connect("mock://demo")
        const loader = findChild(page, "linkInRow")
        tryVerify(() => loader.item, 3000, "the link state")
        const label = findChild(page, "linkLabel")
        tryCompare(label, "text", "Live · mock://demo", 5000)
        verify(label.roomForAddress)
        // The label takes what the title and the tools leave: widen from
        // 380 until it holds a word but not the address
        let w = 360
        do {
            page.width = (w += 20)
            wait(50)
        } while (w < 800 && !(label.width >= 60 && label.width < 150))
        verify(label.width >= 60 && label.width < 150, "a narrow label with room for a word at " + w
               + ": " + label.width)
        compare(label.text, "Live")
        compare(label.opacity, 1, "shown")
        page.width = 300
        tryVerify(() => label.width < 60, 3000, "no room for a word")
        compare(label.opacity, 0, "faded")
        compare(findChild(page, "linkDot").opacity, 0, "the dot with it")
        page.width = 1000
        tryCompare(label, "text", "Live · mock://demo", 3000)
        compare(label.opacity, 1)
    }

    function settleToolRow(row) {
        tryVerify(() => !isPolishScheduled(row) && !isPolishScheduled(row.parent), 3000,
                  "the tool row laid out")
    }

    // At every width around the one where Reset peaks gives way, the row
    // ends inside its bar and keeps Reset peaks while it fits, whatever
    // the font's widths
    function test_theToolRowFitsItsBarAtEveryWidth_data() {
        return [{ tag: "no session", uri: "" }, { tag: "a session", uri: "mock://demo" }]
    }
    function test_theToolRowFitsItsBarAtEveryWidth(data) {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, null, true)
        if (data.uri !== "")
            connect(data.uri)
        tryVerify(() => findChild(page, "toolRowNight").item, 3000, "the Night switch")
        const reset = findChild(page, "resetPeaks"), row = reset.parent, bar = row.parent
        settleToolRow(row)
        const around = Math.round(row.toolsWidth + page.width - bar.width)
        let shown = 0, dropped = 0
        for (let w = around - 16; w <= around + 16; ++w) {
            page.width = w
            settleToolRow(row)
            const ends = Array.from(row.children).filter(c => c.visible).map(c => c.x + c.width)
            const end = row.x + Math.max(...ends)
            verify(end <= bar.width + 0.5, "at " + w + " the row ends at " + end + " of " + bar.width
                   + (reset.visible ? ", Reset peaks shown" : ""))
            if (reset.visible) {
                ++shown
            } else {
                // and gives way only when it wouldn't fit
                const need = row.implicitWidth + row.spacing + Math.ceil(reset.implicitWidth)
                verify(need > bar.width + 0.5, "at " + w + " Reset peaks fits: " + need + " of " + bar.width)
                ++dropped
            }
        }
        verify(shown > 0 && dropped > 0, "the sweep crosses the width: " + shown + " shown, " + dropped + " dropped")
    }

    // A row too narrow for every tool drops Reset peaks, and edit mode's
    // Pages row offers it at its end, whole and inside the page
    function test_aNarrowToolRowMovesResetPeaksIntoThePagesRow() {
        // The phone form: the desktop's Pages flow keeps its unwrapped
        // width in a page made alone this narrow
        const page = pageOf({ form: "phone", width: 1000, height: 600 }, null, true)
        const reset = findChild(page, "resetPeaks"), inRow = findChild(page, "moreResetPeaks")
        const bar = reset.parent.parent
        verify(waitForPolish(page), "laid out")
        verify(bar.width >= page.toolRowFull, "a wide bar: " + bar.width)
        verify(reset.visible, "Reset peaks in the row")
        page.startEditing()
        page.moreShown = true
        verify(!inRow.visible, "and not in the Pages row")
        page.stopEditing()
        page.width = 240
        tryVerify(() => bar.width < page.toolRowFull, 3000, "a narrow bar")
        verify(!reset.visible, "Reset peaks gives way")
        page.startEditing()
        page.moreShown = true
        verify(waitForPolish(page), "the Pages row laid out")
        verify(inRow.visible, "the Pages row offers it")
        verify(inRow.flat, "quiet, as in the row")
        compare(inRow.text, "Reset peaks")
        const shown = Array.from(inRow.parent.children).filter(c => c.visible)
        compare(shown[shown.length - 1], inRow, "at the row's end")
        verify(inRow.width >= inRow.implicitWidth - 0.5, "whole: " + inRow.width + " of " + inRow.implicitWidth)
        verify(inRow.mapToItem(page, inRow.width, 0).x <= page.width + 0.5,
               "inside the page's width: " + inRow.mapToItem(page, inRow.width, 0).x)
        verify(inRow.mapToItem(page, 0, inRow.height).y <= page.height + 0.5,
               "inside the page's height: " + inRow.mapToItem(page, 0, inRow.height).y)
        const spy = createTemporaryObject(spyComponent, testCase,
                                          { target: page, signalName: "peaksResetRequested" })
        inRow.clicked()
        compare(spy.count, 1, "and it resets the peaks")
    }

    // A stored accent of "info" (an older layout's blue) draws as the
    // value's own color; "success" keeps its green. No document changes
    function test_aStoredInfoAccentDrawsAsTheValueColor() {
        const cell = (label, accent) => ({ kind: "digital", w: 2, h: 4, label: label,
                                           refs: refsOf("Engine speed"), unit: "RPM",
                                           options: { from: 0, to: 8000, accent: accent } })
        const page = pageOf({}, docOf([cell("Blue", "info"), cell("Green", "success")]), true)
        tryVerify(() => allLoaded(page), 5000, "the gauges load")
        const blue = gaugeOf(findChild(page, "dashCell_0_0"))
        const green = gaugeOf(findChild(page, "dashCell_0_1"))
        verify(blue && green, "both load")
        verify(Qt.colorEqual(blue.accent, Theme.gaugeValue), "info draws as the value: " + blue.accent)
        verify(Qt.colorEqual(green.accent, Theme.success), "success stays: " + green.accent)
        compare(page.doc.pages[0].cells[0].options.accent, "info", "the stored word stays")
    }
}
