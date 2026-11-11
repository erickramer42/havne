// File/layer: tests/qml/tst_liveview.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.liveview (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest
TestCase {
    id: testCase
    name: "LiveView"

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

    Component {
        id: pageComponent
        // Tall enough that every mock row has a delegate
        LiveViewPage { width: 900; height: 1000 }
    }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
    }

    // Live's store, for the first pause's hint
    Settings {
        id: liveStore
        category: "live"
    }
    // The test window's size before a case grew it, or null
    property var grownFrom: null

    function cleanup() {
        if (grownFrom) {
            testCase.parent.Window.window.width = grownFrom[0]
            testCase.parent.Window.window.height = grownFrom[1]
            grownFrom = null
        }
        Bridge.unitSystem = 0
        Bridge.liveSignalModel.filter.clear()
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Harness.clearFakeKnobs()
        Bridge.deleteUnusedSignalFiles([])
        Theme.night = "off"
        Theme.nightStyle = "amber"
        Theme.look = "dark"
    }

    function sameColor(actual, expected, what) {
        verify(Qt.colorEqual(actual, expected), what + ": " + actual + ", expected " + expected)
    }

    function connectMock(page) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const list = findChild(page, "signalList")
        verify(list.count > 0, "the mock set decodes signals")
        tryVerify(() => list.itemAtIndex(list.count - 1) !== null, 2000, "every row has a delegate")
        return list
    }

    function swatch(list, row) {
        const s = findChild(list.itemAtIndex(row), "swatch")
        verify(s, "row " + row + " has a swatch")
        return s
    }

    Component {
        id: sizedPageComponent
        LiveViewPage {}
    }

    // The chart paints again when the theme changes, so its traces and
    // chrome take the new palette with no session moving it
    function test_theChartPaintsAgainOnANewTheme() {
        const page = createTemporaryObject(pageComponent, testCase)
        const chart = findChild(page, "chart")
        verify(chart, "the chart is there")
        tryVerify(() => chart.available, 2000, "the canvas is ready")
        const spy = createTemporaryObject(spyComponent, testCase, { target: chart })
        usePalette("nightRed")
        spy.wait(2000)
        verify(spy.count >= 1, "painted again for the night theme")
        const before = spy.count
        usePalette("nightAmber")
        tryVerify(() => spy.count > before, 2000, "and again for the amber style")
        usePalette("dark")
    }

    Component {
        id: spyComponent
        SignalSpy { signalName: "painted" }
    }

    // F on a desktop's keyboard maximizes the chart and gives the list
    // back; typed into Find a signal it's a letter of the filter and
    // leaves the chart as it was. In the test's window, which the keys go to
    function test_fTogglesTheChartButNotInTheFilter() {
        if (Theme.mobile)
            skip("F is a desktop's key")
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectMock(page)
        const field = findChild(page, "filterText")
        verify(field && field.visible, "the filter shows while connected")
        field.forceActiveFocus()
        verify(field.activeFocus, "the filter has the focus")
        const before = page.chartMaximized
        keyClick("f")
        compare(field.text, "f", "F goes into the filter")
        compare(page.chartMaximized, before, "and leaves the chart")
        Bridge.liveSignalModel.filter.clear()
        field.focus = false
        page.forceActiveFocus()
        verify(!field.activeFocus, "the page has the focus")
        keyClick("f")
        compare(page.chartMaximized, !before, "F on the page toggles the chart")
        keyClick("f")
        compare(page.chartMaximized, before, "and back")
    }

    // Finding a signal among many (ADR-0050): typed words narrow the
    // list, a group's box and Plotted only narrow it further, Clear
    // brings every row back, and the status line counts what's shown
    function test_theFilterNarrowsTheList() {
        // In the window: the filter's popup opens in its overlay
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const list = connectMock(page)
        const all = list.count
        const bar = findChild(page, "liveFilter")
        verify(bar && bar.visible, "the filter shows while connected")
        // Edited as typing edits it, keeping the field's binding
        const field = findChild(bar, "filterText")
        field.insert(0, "temp")
        field.textEdited()
        tryCompare(list, "count", 3)
        verify(page.statusText().indexOf("Showing 3 of " + all + " signals") === 0,
               page.statusText())

        mouseClick(findChild(bar, "filterButton"))
        tryVerify(() => bar.options.opened, 2000, "the filter's options open")
        const boxes = bar.options.contentItem
        for (const code of [1, 2, 4, 6])
            verify(findChild(boxes, "group_" + code), "a box for group " + code)
        verify(!findChild(boxes, "group_3"), "no box for a group the list lacks")
        // A box's own handler, as a tap runs it
        findChild(boxes, "group_2").toggled()
        tryCompare(list, "count", 1)
        tryVerify(() => list.itemAtIndex(0) !== null && list.itemAtIndex(0).name === "Intake Air Temp",
                  2000, "the row left is the intake air's")
        verify(findChild(bar, "filterButton").checked, "a group chosen checks the button")

        findChild(boxes, "filterClear").clicked()
        tryCompare(list, "count", all)
        compare(field.text, "")
        verify(!findChild(boxes, "group_2").checked, "Clear unticks the group")

        const plottedOnly = findChild(boxes, "plottedOnly")
        plottedOnly.toggle()
        plottedOnly.toggled()
        compare(list.count, all, "the mock plots every row")
        Bridge.signalModel.toggleSubscribed(0)
        tryCompare(list, "count", all - 1)
        verify(page.statusText().indexOf("Showing " + (all - 1) + " of " + all) === 0,
               page.statusText())
        Bridge.signalModel.toggleSubscribed(0)
    }

    // On a phone the field says "Find" and the status line drops the 10 Hz
    // clause, since the long forms are cut at 384 dp; the field's
    // accessible name keeps the whole phrase
    function test_aPhoneGetsTheShortStrings() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const list = connectMock(page)
        const all = list.count
        const bar = findChild(page, "liveFilter")
        const field = findChild(bar, "filterText")
        compare(field.placeholderText, "Find a signal")
        verify(page.statusText().indexOf("Plotting " + all + " of " + all + " signals") === 0,
               page.statusText())
        Theme.compact = true
        try {
            compare(field.placeholderText, "Find")
            compare(field.Accessible.name, "Find a signal by its name or unit")
            compare(page.statusText(), all + " of " + all + " plotted")
            field.insert(0, "temp")
            field.textEdited()
            tryCompare(list, "count", 3)
            compare(page.statusText(), "3 of " + all + " shown · " + all + " plotted")
        } finally {
            Theme.compact = false
        }
    }

    // On a short page (a small window, a large zoom) the list gives up
    // rows, down to one, so the chart stays whole on the page at its
    // minimum height or more; a tall page shows every row
    function test_chartStaysOnAShortPage_data() {
        return [{ tag: "330", h: 330 }, { tag: "450", h: 450 },
                { tag: "600", h: 600 }, { tag: "1200", h: 1200 }]
    }

    function test_chartStaysOnAShortPage(data) {
        const page = createTemporaryObject(sizedPageComponent, testCase,
                                           { width: 800, height: data.h })
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const list = findChild(page, "signalList")
        const chart = findChild(page, "chartFrame")
        verify(list.count > 3, "the mock set has more rows than a short page holds")
        // The layout settles after the connect fills the list
        const bottom = () => chart.mapToItem(page, 0, chart.height).y
        const settled = () => list.height >= page.rowHeight - 0.5
                              && chart.height >= page.chartMinHeight - 0.5
                              && bottom() <= page.height - Theme.pageMargin + 0.5
        let ok = settled()
        for (let i = 0; i < 20 && !ok; ++i) {
            wait(50)
            ok = settled()
        }
        verify(ok, "list " + list.height + " (one row at least), chart " + chart.height
               + " (its minimum at least), ending at " + bottom() + " of " + page.height)
        if (data.h === 1200)
            compare(list.height, list.count * page.rowHeight, "every row shows")
    }

    // The items painted past the page's right edge (a Text by its
    // painted width): the page scrolls in neither direction, so on a
    // phone they can't be seen
    function pastTheEdge(item, page, out) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i]
            if (!child.visible || child.width <= 0)
                continue
            const painted = child.elide !== undefined && !child.clip
                            ? Math.max(child.width, child.contentWidth) : child.width
            const right = child.mapToItem(page, painted, 0).x
            if (right > page.width + 0.5)
                out.push((child.objectName || child.text || String(child))
                         + " at " + Math.round(right))
            pastTheEdge(child, page, out)
        }
        return out
    }

    // The narrowest phone, 320 px: the rows, the window's choices and
    // the chart all lie inside the page
    function test_everyItemFitsAPhonesWidth() {
        const page = createTemporaryObject(sizedPageComponent, testCase,
                                           { width: 320, height: 700 })
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const list = findChild(page, "signalList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "a row has a delegate")
        waitForRendering(page)
        const out = pastTheEdge(page, page, [])
        compare(out.length, 0, "past 320 px: " + out.join(", "))
    }

    // The chart's window: four choices, 30 s at first, the picked one
    // checked and kept by the page
    function test_theWindowIsPickedAboveTheChart() {
        const page = createTemporaryObject(pageComponent, testCase)
        verify(page)
        const before = page.chartWindowS
        for (const s of page.chartWindows)
            verify(findChild(page, "chartWindow_" + s), "a button for " + s + " s")
        findChild(page, "chartWindow_60").clicked()
        compare(page.chartWindowS, 60)
        verify(findChild(page, "chartWindow_60").checked)
        verify(!findChild(page, "chartWindow_10").checked)
        findChild(page, "chartWindow_" + before).clicked()
        compare(page.chartWindowS, before, "put back as it was")
    }

    function test_colorComesFromTheRow() {
        const page = createTemporaryObject(pageComponent, testCase)
        const n = Theme.seriesColors.length
        for (let row = 0; row < 3 * n; ++row)
            sameColor(page.seriesColor(row), Theme.seriesColors[row % n], "row " + row)
    }

    // Rows within the palette draw solid; past it, rows reuse its colors,
    // and no two rows that share a color share a dash
    function test_dashesTellSharedColorsApart() {
        const page = createTemporaryObject(pageComponent, testCase)
        const n = Theme.seriesColors.length
        for (let row = 0; row < n; ++row)
            compare(page.seriesDash(row).length, 0, "row " + row + " draws solid")
        for (let a = 0; a < 3 * n; ++a) {
            for (let b = a + n; b < 3 * n; b += n)
                verify(JSON.stringify(page.seriesDash(a)) !== JSON.stringify(page.seriesDash(b)),
                       "rows " + a + " and " + b + " share a color and a dash")
        }
    }

    function test_swatchesMatchTheirRows() {
        const page = createTemporaryObject(pageComponent, testCase)
        const list = connectMock(page)
        for (let row = 0; row < list.count; ++row) {
            const s = swatch(list, row)
            sameColor(s.trace, page.seriesColor(row), "row " + row + " trace")
            sameColor(s.color, s.trace, "row " + row + " fill")
            sameColor(s.border.color, s.trace, "row " + row + " border")
            compare(s.opacity, 1.0)
        }
    }

    // Unplotted, a row's swatch keeps its color as an outline, and the
    // other rows keep theirs
    function test_unplottedRowHasAHollowSwatch() {
        const page = createTemporaryObject(pageComponent, testCase)
        const list = connectMock(page)
        verify(list.count > 2)

        Bridge.signalModel.toggleSubscribed(1)
        const s = swatch(list, 1)
        sameColor(s.color, "transparent", "unplotted fill")
        sameColor(s.border.color, page.seriesColor(1), "unplotted border")
        compare(s.opacity, 0.5)
        sameColor(swatch(list, 2).color, page.seriesColor(2), "row 2 fill")

        Bridge.signalModel.toggleSubscribed(1)
        sameColor(s.color, page.seriesColor(1), "plotted again")
        compare(s.opacity, 1.0)
    }

    // A text row (a uds-generic TEXT def, which the mock answers each
    // tick) shows its text where a number would go and can't be plotted;
    // the number row beside it formats as usual
    function test_textRowShowsTextAndCannotPlot() {
        const page = createTemporaryObject(pageComponent, testCase)
        compare(Bridge.connectSession("mock://demo?profile=uds-generic&did=F190:text,1E1C:u16"),
                "", "uds-generic connects")
        const list = findChild(page, "signalList")
        compare(list.count, 2, "one row per DID")
        tryVerify(() => list.itemAtIndex(1) !== null, 2000, "both rows have delegates")
        const textRow = list.itemAtIndex(0)
        const numberRow = list.itemAtIndex(1)
        verify(textRow.isText, "F190:text is a text row")
        verify(!numberRow.isText, "1E1C:u16 is a number row")
        tryVerify(() => textRow.hasValue && numberRow.hasValue, 3000, "the mock answered both DIDs")

        const textValue = findChild(textRow, "valueLabel")
        verify(textValue.text.startsWith("MOCK F190 #"), "text shown: " + textValue.text)
        compare(textValue.text, textRow.text)
        verify(!findChild(textRow, "plotCheck").enabled, "a text row can't be plotted")
        verify(!textRow.subscribed)

        const numberValue = findChild(numberRow, "valueLabel")
        verify(/^\d+/.test(numberValue.text.trim()), "a number shown: " + numberValue.text)
        verify(findChild(numberRow, "plotCheck").enabled, "a number row can be plotted")
    }

    // An enum row (ADR-0051) shows its label where the number would go:
    // a signal file's one enum on 1E1C's first byte, every raw value
    // labeled, so whatever the mock answers has its word
    function test_anEnumRowShowsItsLabel() {
        const page = createTemporaryObject(pageComponent, testCase)
        const values = {}
        for (let raw = 0; raw < 256; ++raw)
            values[String(raw)] = "v" + raw
        const file = {
            havne_signals: 1, name: "Enum test", source: { name: "t" },
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "mode", name: "Mode", read: { service: "22", number: "1E1C" },
                        field: { byte: 0, bits: 8 }, kind: "enum", values: values }]
        }
        const r = Bridge.importSignalText(JSON.stringify(file))
        verify(r.ok, "the file loads: " + r.words)
        verify(Bridge.useSignalFiles([r.sha256])[0].ok, "its profile registers")
        compare(Bridge.connectSession("mock://demo?profile=" + r.profile), "",
                "the file's profile connects")
        const list = findChild(page, "signalList")
        compare(list.count, 1, "the file's one signal")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        const row = list.itemAtIndex(0)
        tryVerify(() => row.hasValue, 3000, "the mock answered the DID")
        const label = findChild(row, "valueLabel")
        compare(label.text, "v" + Math.round(row.value), "the label, not the number")
        compare(label.text, row.label)
        // The value face with tabular digits, as the gauges' readouts
        compare(label.font.family, Theme.valueFamily)
        compare(label.font.features.tnum, 1)
    }

    // The rows whose PID the ECU's bitmaps leave out are hidden, with
    // one line saying how many and why; the Dashboard's model keeps them
    function test_hidesThePidsTheEcuLacks() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        Harness.armFakeKnob("ECUIO_FAKE_WITHHELD_PIDS", "0A,5C")
        // In the window's content, so the note's visibility is its own
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem)
        const note = findChild(page, "hiddenNote")
        verify(page.visible, "the page shows")
        verify(!note.visible, "no note before the ECU answers")
        const uri = "j2534://" + Harness.fakeDllPath + "?identify=0&diagnose=0"
        compare(Bridge.connectSession(uri), "", uri + " connects")
        const list = findChild(page, "signalList")
        const all = Bridge.signalModel.rowCount()
        tryVerify(() => note.visible, 5000, "the note shows")
        compare(note.text, "2 signals hidden: the ECU doesn't send them")
        compare(list.count, all - 2, "Live lists the rest")
        compare(Bridge.signalModel.rowCount(), all, "the model keeps every row")
        for (let r = 0; r < list.count; ++r) {
            list.positionViewAtIndex(r, ListView.Contain)
            const row = list.itemAtIndex(r)
            verify(row, "row " + r + " has a delegate")
            verify(row.name !== "Engine Oil Temp" && row.name !== "Fuel Pressure",
                   "row " + r + " is " + row.name)
        }
    }

    // ADR-0051 section 3: a DID the ECU refuses says so on its row, the
    // code by name, and one it answers shows its value
    function test_aRefusedDidSaysSo() {
        if (Harness.fakeDllPath === "")
            skip("needs the fake J2534 DLL (Windows)")
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem)
        const uri = "j2534://" + Harness.fakeDllPath
                    + "?profile=uds-generic&did=1E1C:u16,F1FE:u16&identify=0&diagnose=0"
        compare(Bridge.connectSession(uri), "", uri + " connects")
        const list = findChild(page, "signalList")
        tryCompare(list, "count", 2, 5000)
        const valueOf = name => {
            for (let r = 0; r < list.count; ++r) {
                list.positionViewAtIndex(r, ListView.Contain)
                const row = list.itemAtIndex(r)
                if (row && row.name === name)
                    return findChild(row, "valueLabel").text
            }
            return "no row " + name
        }
        tryVerify(() => valueOf("DID F1FE") === "Refused: requestOutOfRange (7F 22 31)", 5000,
                  "DID F1FE says it was refused: " + valueOf("DID F1FE"))
        // 0x1234, in the locale's digits
        tryVerify(() => valueOf("DID 1E1C").replace(/[^0-9]/g, "") === "4660", 5000,
                  "DID 1E1C shows its value: " + valueOf("DID 1E1C"))

        // A phone's width: the words don't fit beside the name, so the
        // row reads the reply alone, and the name stays whole
        page.width = 360
        tryVerify(() => valueOf("DID F1FE") === "Refused (7F 22 31)", 5000,
                  "a narrow row's refusal: " + valueOf("DID F1FE"))
        let name = null
        for (let r = 0; r < list.count; ++r) {
            const row = list.itemAtIndex(r)
            if (row && row.name === "DID F1FE")
                name = row
        }
        verify(name !== null)
        const label = findChild(name, "valueLabel")
        verify(!label.truncated, "the short form fits")
        page.width = 1000
        tryVerify(() => valueOf("DID F1FE") === "Refused: requestOutOfRange (7F 22 31)", 5000,
                  "wide again, the words: " + valueOf("DID F1FE"))
    }

    function test_bannerText_data() {
        return [
            { tag: "quiet", state: "quiet", episodes: 0, code: 0,
              text: "Vehicle not responding: quiet bus; check the ignition" },
            { tag: "requests", state: "requests", episodes: 1, code: 10,
              text: "Requests failing: check the vehicle connection and ignition" },
            { tag: "fault", state: "fault", episodes: 3, code: 0,
              text: "Adapter fault: 3 episode(s), retrying with backoff" },
            { tag: "stopped", state: "stopped", episodes: 1, code: 16,
              text: "Replay stopped: the recording changed after it was opened" },
            { tag: "degraded", state: "degraded", episodes: 0, code: 42,
              text: "Link degraded (error 42)" },
            { tag: "unknown", state: "a later state", episodes: 0, code: 43,
              text: "Link degraded (error 43)" },
        ]
    }
    function test_bannerText(data) {
        const page = createTemporaryObject(pageComponent, testCase)
        compare(page.bannerText(data.state, data.episodes, data.code), data.text)
    }

    // A page in the test window, grown to the page's size so a pointer
    // lands on the chart; cleanup() puts the size back
    function pageInGrownWindow() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const win = page.Window.window
        grownFrom = [win.width, win.height]
        win.width = page.width
        win.height = page.height
        tryVerify(() => win.width >= page.width && win.height >= page.height, 5000, "the window grown")
        verify(waitForPolish(win), "the grown window laid out")
        return page
    }

    // The plotted history spans at least ms
    function waitForHistory(ms) {
        tryVerify(() => {
            const s = Bridge.signalModel.chartData(0, 0)
            return s.length > 0 && s[0].samples.length >= 6
                   && s[0].samples[s[0].samples.length - 2] - s[0].samples[0] >= ms
        }, 20000, "a history of " + ms + " ms to pause on")
    }

    // A drag in steps, as a pointer moves (mouseDrag's two moves don't carry
    // a DragHandler past its threshold)
    function dragAcross(frame, dx) {
        const x = frame.width / 2, y = frame.height / 2
        mousePress(frame, x, y)
        const step = dx > 0 ? 10 : -10
        for (let d = step; Math.abs(d) <= Math.abs(dx); d += step)
            mouseMove(frame, x + d, y, 10)
        mouseRelease(frame, x + dx, y)
    }

    // The pause keeps each plotted signal's history as it stood, times
    // counted back from its newest sample; the session goes on and the
    // chart holds still, until Live follows now again
    function test_thePauseKeepsTheHistoryAsItStood() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectMock(page)
        const chart = findChild(page, "chart")
        const live = findChild(page, "liveButton")
        verify(!live.visible, "no Live button while the chart follows")
        waitForHistory(500)
        const before = Bridge.signalModel.chartData(0, 0)
        page.pause()
        verify(page.paused, "paused")
        compare(page.snapshot.length, before.length, "a series for each plotted signal")
        let newest = -Infinity
        for (let i = 0; i < before.length; ++i) {
            const sp = before[i].samples, kept = page.snapshot[i].samples
            compare(kept.length, sp.length, "every sample of row " + before[i].row)
            if (sp.length < 2)
                continue
            compare(kept[0] + page.snapshotEndMs, sp[0], "the first sample's time")
            compare(kept[kept.length - 2] + page.snapshotEndMs, sp[sp.length - 2], "the last's")
            newest = Math.max(newest, sp[sp.length - 2])
        }
        compare(page.snapshotEndMs, newest, "the pause is at the newest sample")
        verify(live.visible && live.highlighted, "Live shows, primary")
        verify(page.statusText().indexOf("Paused · ") === 0, page.statusText())

        tryVerify(() => {
            const s = Bridge.signalModel.chartData(0, 0)
            return s[0].samples[s[0].samples.length - 2] > page.snapshotEndMs + 300
        }, 5000, "the session goes on")
        for (const s of chart.fetch(400)) {
            verify(s.samples.length === 0 || s.samples[s.samples.length - 2] <= 0,
                   "the chart holds at the pause: row " + s.row)
        }
        live.clicked()
        verify(!page.paused, "Live follows now again")
        compare(page.snapshot, null, "the snapshot dropped")
        verify(!live.visible, "and goes")
        verify(page.statusText().indexOf("Paused") < 0, page.statusText())
    }

    // A drag pauses and pans back through the snapshot; a drag pushed past
    // its right edge follows now again
    function test_aDragPausesPansAndResumesAtTheEdge() {
        const page = pageInGrownWindow()
        compare(Bridge.connectSession("replay://" + Harness.replayFixtures
                                      + "/2026-10-25-engine-start?profile=obd2&speed=max"), "",
                "the replay connects")
        const kept = page.chartWindowS
        findChild(page, "chartWindow_10").clicked()
        waitForHistory(25000)
        const frame = findChild(page, "chartFrame")
        dragAcross(frame, 200)
        verify(page.paused, "a drag pauses")
        verify(page.viewEndMs < -1000, "and pans back: " + page.viewEndMs)
        const panned = page.viewEndMs
        dragAcross(frame, 80)
        verify(page.paused && page.viewEndMs < panned, "a second drag pans on: " + page.viewEndMs)
        dragAcross(frame, -600)
        verify(!page.paused, "a drag past the right edge follows now again")
        findChild(page, "chartWindow_" + kept).clicked()
    }

    // Space pauses and resumes on a desktop, but types a space into Find a
    // signal; a tap on the paused chart reads each trace's value at the
    // cursor into its label
    function test_spaceAndTheCursor() {
        if (Theme.mobile)
            skip("Space is a desktop's key")
        const page = pageInGrownWindow()
        connectMock(page)
        waitForHistory(1500)
        const field = findChild(page, "filterText")
        field.focus = false
        page.forceActiveFocus()
        keyClick(Qt.Key_Space)
        verify(page.paused, "Space pauses")
        keyClick(Qt.Key_Space)
        verify(!page.paused, "and resumes")
        field.forceActiveFocus()
        keyClick(" ")
        compare(field.text, " ", "a space goes into the filter")
        verify(!page.paused, "and leaves the chart following")
        Bridge.liveSignalModel.filter.clear()
        field.focus = false
        page.forceActiveFocus()
        keyClick(Qt.Key_Space)
        verify(page.paused, "paused again")

        const frame = findChild(page, "chartFrame")
        const chart = findChild(page, "chart")
        tryVerify(() => chart.drawn !== null, 2000, "the paused chart drawn")
        // Halfway into the snapshot, where every trace has samples
        const w = page.chartWindowS * 1000
        const x = Math.round(chart.x + (page.snapshotStartMs / 2 + w) / w * (chart.drawn.plotW - 1))
        // Taken before the tap: the labels it fills change the plot's width
        const tapped = page.timeAt(x)
        mouseClick(frame, x, frame.height / 2)
        verify(!isNaN(page.cursorMs), "a tap places the cursor")
        compare(page.cursorMs, tapped, "where the tap was")
        verify(page.statusText().indexOf("cursor at −") > 0, page.statusText())
        let read = 0
        for (const s of page.snapshot) {
            const sp = s.samples
            let best = 0
            for (let i = 0; i + 1 < sp.length; i += 2) {
                if (Math.abs(sp[i] - page.cursorMs) < Math.abs(sp[best] - page.cursorMs))
                    best = i
            }
            if (sp.length < 2 || Math.abs(sp[best] - page.cursorMs) > page.staleMs)
                continue
            tryVerify(() => chart.drawn && chart.drawn.traces.some(tr => tr.row === s.row
                                                                    && tr.labelMs !== undefined),
                      2000, "row " + s.row + " read at the cursor")
            const tr = chart.drawn.traces.find(t => t.row === s.row)
            compare(tr.labelMs, sp[best], "row " + s.row + "'s label reads the nearest sample")
            compare(tr.label, page.valueText(sp[best + 1], s.decimals, s.unit),
                    "row " + s.row + "'s value")
            ++read
        }
        verify(read > 0, "some trace has a sample at the cursor")
    }

    // The snapshot goes with what it was taken of: a unit change, leaving
    // the page, a disconnect
    function test_thePauseDropsWithItsSession() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectMock(page)
        waitForHistory(300)
        page.pause()
        verify(page.paused)
        Bridge.unitSystem = 1
        verify(!page.paused, "a unit change drops it")
        Bridge.unitSystem = 0
        page.pause()
        verify(page.paused)
        page.visible = false
        verify(!page.paused, "leaving the page drops it")
        page.visible = true
        page.pause()
        verify(page.paused)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected, 5000, "disconnected")
        verify(!page.paused, "a disconnect drops it")
        compare(page.snapshot, null)
    }

    // The first pause of all names Start log, once: not at the next pause,
    // nor on a page made later
    function test_theFirstPauseHintShowsOnce() {
        liveStore.setValue("pauseHintSeen", false)
        liveStore.sync()
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectMock(page)
        waitForHistory(300)
        const hint = findChild(page, "pauseHint")
        verify(!hint.visible, "no hint while live")
        page.pause()
        verify(hint.visible, "the first pause shows it")
        compare(hint.text, "Start log records the session for the viewer")
        page.resume()
        verify(!hint.visible, "gone at resume")
        page.pause()
        verify(!hint.visible, "not at the second pause")
        page.resume()
        const later = createTemporaryObject(pageComponent, testCase.parent)
        later.pause()
        verify(later.paused)
        verify(!findChild(later, "pauseHint").visible, "nor on a page made later")
    }

    // The empty state: plain words, and a Connect that isn't a second
    // primary beside the header's
    function test_theEmptyStateOffersAConnectInPlainWords() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        compare(page.statusText(), "Connect a session to read live values")
        const button = findChild(page, "liveConnect")
        verify(button && button.visible, "Connect… is offered")
        verify(!button.highlighted, "secondary: the header holds the primary")
        compare(button.text, "Connect…")
    }
}
