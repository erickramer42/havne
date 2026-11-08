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

    // Section 4's grid: the phone's 2 columns upright and 4 on its
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
            { tag: "a phone upright", form: "phone", w: 384, h: 832, columns: 2 },
            { tag: "a phone on its side", form: "phone", w: 832, h: 384, columns: 4 }
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
        const pitchX = view.columnWidth + page.gap
        const shown = []
        for (let i = 0; i < cells.length; ++i) {
            const cell = findChild(page, "dashCell_0_" + i)
            verify(cell, "cell " + i)
            fuzzyCompare(cell.x, boxes[i].x * pitchX, 1e-6, "cell " + i + " x")
            fuzzyCompare(cell.y, boxes[i].y * view.pitch, 1e-6, "cell " + i + " y")
            fuzzyCompare(cell.width, boxes[i].w * view.columnWidth + (boxes[i].w - 1) * page.gap,
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
        fuzzyCompare(view.pitch, pitchX / 4, 1e-6, "a row is a quarter of a column")
        fuzzyCompare(tach.height, tach.width, 1e-6, "the tach is square")
    }

    // The phone's dials share the first row under the shift lights
    function test_thePhonesDialsShareARow() {
        const page = createTemporaryObject(pageComponent, testCase,
                                           { form: "phone", width: 384, height: 832 })
        const tach = findChild(page, "dashCell_0_1")
        const speedo = findChild(page, "dashCell_0_2")
        tryVerify(() => tach.width > 0, 2000, "the grid lays the cells out")
        compare(tach.y, speedo.y, "side by side")
        verify(tach.x < speedo.x)
    }

    // The desktop's page buttons and Left and Right; one page has neither
    function test_theDesktopsPageButtonsAndKeys() {
        const page = pageOf({}, pagesDoc(3), true)
        const buttons = findChild(page, "dashPageButtons")
        const swipe = findChild(page, "dashPages")
        verify(buttons.visible, "the buttons show with three pages")
        verify(!findChild(page, "dashIndicator").visible, "no indicator on the desktop")
        verify(!swipe.interactive, "the desktop's pages don't swipe")
        compare(findChild(page, "dashPageButton_0").text, "Engine")
        compare(findChild(page, "dashPageButton_2").text, "Page 3")
        compare(page.pageIndex, 0)
        verify(findChild(page, "dashPageButton_0").highlighted)

        mouseClick(findChild(page, "dashPageButton_2"))
        compare(page.pageIndex, 2)
        tryCompare(swipe, "currentIndex", 2)
        verify(findChild(page, "dashPageButton_2").highlighted)
        verify(!findChild(page, "dashPageButton_0").highlighted)

        keyClick(Qt.Key_Right)
        compare(page.pageIndex, 2, "Right on the last page stays")
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 1)
        tryCompare(swipe, "currentIndex", 1)
        keyClick(Qt.Key_Left)
        keyClick(Qt.Key_Left)
        compare(page.pageIndex, 0, "Left on the first page stays")

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
        verify(!buttons.visible, "one page has no buttons")
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
        verify(!findChild(page, "dashPageButtons").visible, "no buttons on the phone")
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
            { tag: "newer", text: JSON.stringify({ format: 2, pages: [] }),
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

        // A log the session's end closes, told; the next log clears it
        connect("mock://demo")
        compare(Bridge.startLog(), true)
        compare(Bridge.logNotice, "")
        verify(!line.visible, "a running log says nothing")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.connected, 5000, "disconnect ends")
        compare(Bridge.logNotice, "Log ended: disconnected")
        tryVerify(() => line.visible, 2000, "the end shows")
        compare(words.text, "Log ended: disconnected")
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
        tryCompare(all["Oil temp"], "note", "not supported by this car", 5000)
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
}
