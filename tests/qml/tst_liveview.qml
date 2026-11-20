// File/layer: tests/qml/tst_liveview.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.liveview (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

LiveViewKit {
    id: testCase
    name: "LiveView"

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

    // Live's chart holds its label column, crowded drawing and ranges from
    // paint to paint, and lets them go at a new window, a resume and the
    // session's end
    function test_theChartHoldsItsLayoutWhileASessionRuns() {
        const page = createTemporaryObject(pageComponent, testCase)
        const chart = findChild(page, "chart")
        verify(chart.holds, "Live's chart holds")
        // A 300 px column shows only under the label cap, 0.4 of the chart
        verify(chart.shownW * 0.4 > 300 + ChartLayout.swatchW + ChartLayout.labelGap,
               "a chart " + chart.shownW + " px wide")
        connectMock(page)
        tryVerify(() => chart.held !== null, 5000, "a paint keeps what it held")
        const plant = () => {
            tryVerify(() => chart.held !== null, 5000, "a paint keeps what it held")
            chart.held = { key: chart.held.key, labelW: 300, dense: chart.held.dense,
                           ranges: chart.held.ranges }
            tryVerify(() => chart.held.labelW === 300 && chart.drawn
                            && chart.drawn.plotW <= chart.shownW - 300, 5000,
                      "a paint keeps a held column")
        }
        const letGo = what => tryVerify(() => chart.held === null || chart.held.labelW < 300,
                                        5000, what + " lets it go")
        plant()
        const kept = page.chartWindowS
        findChild(page, "chartWindow_" + (kept === 60 ? 10 : 60)).clicked()
        letGo("a new window")
        findChild(page, "chartWindow_" + kept).clicked()
        compare(page.chartWindowS, kept)
        plant()
        page.pause()
        page.resume()
        letGo("a resume")
        plant()
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        letGo("the session's end")
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

    // The ticked rows are what Start log records: the line says so where
    // the button is offered, and "Logging" while a log runs
    function test_theStatusLineSaysWhatALogRecords() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const list = connectMock(page)
        const all = list.count
        verify(page.statusText().endsWith(" · Start log records the ticked ones"), page.statusText())
        Bridge.startLog()
        verify(Bridge.logActive, "a log runs")
        compare(page.statusText(), "Logging the " + Bridge.signalModel.tickedCount + " ticked signals")
        Bridge.stopLog()
        verify(!Bridge.logActive)
        verify(page.statusText().endsWith("records the ticked ones"), page.statusText())

        // one plotted signal reads in the singular
        for (let i = list.count - 1; i >= 1; --i) {
            if (list.itemAtIndex(i).subscribed)
                Bridge.signalModel.toggleSubscribed(i)
        }
        tryCompare(Bridge.signalModel, "subscribedCount", 1, 2000, "one signal left plotted")
        Bridge.startLog()
        verify(Bridge.logActive, "a log runs")
        compare(page.statusText(), "Logging the 1 ticked signal")
        Bridge.stopLog()
        verify(!Bridge.logActive)

        // Where Start log isn't offered the note isn't made: a recorded
        // session (Record sessions on) and a replay
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        Bridge.recording = true
        try {
            compare(Bridge.connectSession("mock://demo"), "")
            tryVerify(() => Bridge.recordingActive, 5000, "the session is recorded")
            verify(page.statusText().indexOf("Start log") < 0, page.statusText())
        } finally {
            Bridge.disconnectSession()
            tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
            Bridge.recording = false
        }
        const replay = Bridge.recordingRow(Harness.recordingFixtures + "/findings")
        compare(Bridge.connectSession(replay.uri), "", "a replay connects")
        tryVerify(() => page.statusText().indexOf("Plotting") === 0
                        || page.statusText().indexOf(" of ") > 0, 5000, "its line")
        verify(page.statusText().indexOf("Start log") < 0, page.statusText())
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
            compare(page.statusText(), all + " of " + all + " plotted · Start log records them")
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

    // A row is one accessible node named by its value; its Texts are
    // ignored, so a value changing at the model's rate renames the node and
    // makes, hides or destroys none (355)
    function test_aRowIsOneNamedNode() {
        const page = createTemporaryObject(pageComponent, testCase)
        compare(Bridge.connectSession("mock://demo"), "", "the mock connects")
        const list = findChild(page, "signalList")
        tryVerify(() => list.count > 0 && list.itemAtIndex(0) !== null, 3000, "a row")
        const row = list.itemAtIndex(0)
        tryVerify(() => row.hasValue, 3000, "a value")
        const value = findChild(row, "valueLabel")
        verify(findChild(row, "nameLabel").Accessible.ignored, "the name's Text is ignored")
        verify(value.Accessible.ignored, "the value's Text is ignored")
        compare(row.Accessible.name, row.name + ": " + value.text)
    }

    // A text row (a uds-generic TEXT def, which the mock answers each
    // tick) shows its text; its tick asks it and plots nothing
    function test_textRowShowsTextAndItsTickAsksIt() {
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
        const textBox = findChild(textRow, "plotCheck")
        verify(textBox.enabled, "a text row can be ticked")
        verify(!textRow.subscribed)
        verify(textBox.Accessible.name.startsWith("Ask "), textBox.Accessible.name)
        const plotted = Bridge.signalModel.subscribedCount
        const ticked = Bridge.signalModel.tickedCount
        textBox.toggle()
        textBox.toggled()
        tryVerify(() => textRow.subscribed, 1000, "the tick took")
        compare(Bridge.signalModel.subscribedCount, plotted, "a text row plots nothing")
        compare(Bridge.signalModel.tickedCount, ticked + 1, "a log counts it")
        compare(findChild(textRow, "swatch").color, Qt.rgba(0, 0, 0, 0), "no trace to fill")
        compare(Bridge.signalModel.plottedKeys().length, 1,
                "the vehicle keeps the text row's tick")
        const filter = Bridge.liveSignalModel.filter
        filter.plottedOnly = true
        tryCompare(list, "count", 1, 1000, "Plotted only leaves the text row out")
        filter.clear()
        tryCompare(list, "count", 2)
        tryVerify(() => list.itemAtIndex(1) !== null, 2000, "both rows again")

        // With the text row alone ticked, Start log takes it
        Bridge.signalModel.toggleSubscribed(list.itemAtIndex(1).row)
        tryCompare(Bridge.signalModel, "subscribedCount", 0)
        compare(Bridge.signalModel.tickedCount, 1)
        Bridge.startLog()
        verify(Bridge.logActive, "a log of the text row runs")
        compare(page.statusText(), "Logging the 1 ticked signal")
        Bridge.stopLog()
        verify(!Bridge.logActive)
        Bridge.signalModel.toggleSubscribed(list.itemAtIndex(1).row)
        tryVerify(() => list.itemAtIndex(1) !== null && list.itemAtIndex(1).subscribed, 2000)

        const numberRowNow = list.itemAtIndex(1)
        const numberValue = findChild(numberRowNow, "valueLabel")
        verify(/^\d+/.test(numberValue.text.trim()), "a number shown: " + numberValue.text)
        verify(findChild(numberRowNow, "plotCheck").enabled, "a number row can be plotted")
        verify(findChild(numberRowNow, "plotCheck").Accessible.name.startsWith("Plot "))
    }

    // A row the session's focus leaves unasked says "Not asked", dimmed,
    // where its value would be, and the line under the list counts them;
    // Asked only leaves them out. The mock asks nothing, so the case marks
    // the row through the model as the bridge would.
    function test_anUnaskedRowSaysSoAndIsCounted() {
        // In the window's content item, where the note's visible is its own
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem)
        compare(Bridge.connectSession("mock://demo?profile=uds-generic&did=F190:text,1E1C:u16"),
                "", "uds-generic connects")
        const list = findChild(page, "signalList")
        tryVerify(() => list.itemAtIndex(1) !== null, 2000, "both rows have delegates")
        const numberRow = list.itemAtIndex(1)
        tryVerify(() => numberRow.hasValue, 3000, "the mock answered 1E1C")
        const note = findChild(page, "unaskedNote")
        verify(!note.visible, "nothing unasked yet")

        Harness.markUnasked([numberRow.name])
        const value = findChild(numberRow, "valueLabel")
        tryCompare(value, "text", "Not asked")
        compare(value.color, Theme.textDim)
        tryVerify(() => note.visible, 1000, "the line shows")
        compare(note.text, "1 signal not asked: tick its row to ask it")
        // A log keeps the signals it started with: no tick asks a row then
        Bridge.startLog()
        verify(Bridge.logActive, "a log runs")
        compare(note.text, "1 signal not asked while the log runs")
        Bridge.stopLog()
        verify(!Bridge.logActive)
        compare(note.text, "1 signal not asked: tick its row to ask it")

        const filter = Bridge.liveSignalModel.filter
        filter.askedOnly = true
        tryCompare(list, "count", 1)
        filter.clear()
        tryCompare(list, "count", 2)
        Harness.markUnasked([])
        tryVerify(() => !note.visible, 1000, "an asked row clears the line")
        // The filter made the row's delegate again at a polish: until then
        // itemAtIndex gives the one going
        tryVerify(() => {
            const row = list.itemAtIndex(1)
            // The mock's 1E1C value, a number: neither "Not asked" nor "—"
            return row !== null && /\d/.test(findChild(row, "valueLabel").text)
        }, 2000, "the row asked again shows its value")
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
        const r = Bridge.keepSignalText(JSON.stringify(file), "", "", "")
        verify(r.ok, "the file loads: " + r.words)
        compare(Bridge.connectSession("mock://demo?profile=" + r.profiles[0]), "",
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
        // The walk can answer 0x0A's bitmap range before 0x5C's: the note
        // reads one signal first
        tryVerify(() => note.visible, 5000, "the note shows")
        tryCompare(note, "text", "2 signals hidden: the ECU doesn't send them", 5000)
        tryCompare(list, "count", all - 2, 5000, "Live lists the rest")
        compare(Bridge.signalModel.rowCount(), all, "the model keeps every row")
        for (let r = 0; r < list.count; ++r) {
            list.positionViewAtIndex(r, ListView.Contain)
            const row = list.itemAtIndex(r)
            verify(row, "row " + r + " has a delegate")
            verify(row.name !== "Engine Oil Temp" && row.name !== "Fuel Pressure",
                   "row " + r + " is " + row.name)
        }
    }

    // The note counts what Live leaves out; its Show opens the rows inline,
    // each with the reason, and Hide closes them. The mock under obd2
    // sends the demo car's PIDs and the bitmaps rule out the rest.
    function test_theHiddenNoteOpensTheRowsWithTheirReasons() {
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem)
        const note = findChild(page, "hiddenNote")
        const toggle = findChild(page, "hiddenToggle")
        const list = findChild(page, "hiddenList")
        const part = findChild(page, "hiddenSignals")
        compare(Bridge.connectSession("mock://demo?profile=obd2"), "", "connects")
        tryVerify(() => note.visible && Bridge.signalModel.hiddenCount > 1, 5000, "the note shows")
        verify(!list.visible, "closed to start with")
        compare(toggle.text, "Show")
        mouseClick(toggle)
        tryVerify(() => list.visible, 2000, "the list opens")
        compare(toggle.text, "Hide")
        // One row for each hidden signal, and no other
        tryVerify(() => list.count === Bridge.signalModel.hiddenCount, 2000,
                  "the list holds the hidden rows: " + list.count + " of " + Bridge.signalModel.hiddenCount)
        list.positionViewAtBeginning()
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the first row is drawn: " + list.height)
        const first = list.itemAtIndex(0)
        verify(findChild(first, "hiddenName").text !== "")
        compare(findChild(first, "hiddenWhy").text, "the ECU's supported-PID list leaves it out",
                "the demo car's missing PIDs are the bitmaps' doing")
        // The reason's words follow the model's: only "absent" reads as absent
        compare(part.reasonText("unsupported"), "the ECU's supported-PID list leaves it out")
        compare(part.reasonText("absent"), "its replies come without this field")
        compare(part.reasonText(""), "")
        compare(part.reasonText("other"), "")
        mouseClick(toggle)
        tryVerify(() => !list.visible, 2000, "the list closes")
        compare(toggle.text, "Show")
        Bridge.disconnectSession()
    }

    // A row the recording never holds keeps its dash while the replay plays
    // and reads "Not in this recording" once the last frame has played
    function test_aRowTheRecordingNeverHoldsSaysSoAtTheReplaysEnd() {
        const r = Bridge.keepSignalText(JSON.stringify({
            havne_signals: 1, name: "Never recorded", source: { name: "t" }, extends: "obd2",
            ecus: [{ id: "pcm", rx: "7E8" }],
            signals: [{ key: "never", name: "Never", read: { service: "22", number: "F1FE" },
                        field: { byte: 0, bits: 8 } }]
        }), "", "", "")
        verify(r.ok, r.words)
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem)
        const list = findChild(page, "signalList")
        const valueOf = name => {
            for (let i = 0; i < list.count; ++i) {
                list.positionViewAtIndex(i, ListView.Contain)
                const row = list.itemAtIndex(i)
                if (row && row.name === name)
                    return findChild(row, "valueLabel").text
            }
            return "no row " + name
        }
        compare(Bridge.connectSession("replay://" + Harness.replayFixtures
                                      + "/2026-10-25-engine-start?profile=" + r.profiles[0]
                                      + "&speed=max"), "", "the replay connects")
        tryCompare(Bridge, "replayFinished", true, 10000)
        tryCompare({ get v() { return valueOf("Never") } }, "v", "Not in this recording", 5000)
        Bridge.disconnectSession()
        tryCompare(Bridge, "replayFinished", false, 5000)
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
}
