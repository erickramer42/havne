// File/layer: tests/qml/tst_liveview.qml | Qt Quick Test, QML tier
// Testing: LiveView's trace styles keyed by model row (color from
//          Theme.seriesColors, dashes past the palette), each row's
//          swatch under mock://demo, filled while plotted and hollow
//          while not, a text row under uds-generic (the mock answering
//          its DIDs), an enum row's label (a signal file's), the list
//          giving up rows so the chart stays whole
//          on a short page, the rows the fake ECU withholds from its
//          bitmaps left out with a line saying how many (Windows: the
//          fake J2534 DLL), the link banner's message for every state,
//          F toggling the chart but typed into the filter as a letter,
//          and every item inside a phone's 320 px.
//          The banner on screen in each state is tst_linkhealth.qml's.

import QtQuick
import QtTest
import App
import AppTest
TestCase {
    id: testCase
    name: "LiveView"
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

    function cleanup() {
        Bridge.liveSignalModel.filter.clear()
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        Harness.clearFakeKnobs()
        Bridge.deleteUnusedSignalFiles([])
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
        verify(findChild(bar, "filterButton").highlighted, "a group chosen lights the button")

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

    // On a short page (a small window, a large zoom) the list gives up
    // rows, down to one, so the chart stays whole on the page at its
    // minimum height or more; a tall page shows every row
    function test_chartStaysOnAShortPage_data() {
        return [{ tag: "330", h: 330 }, { tag: "450", h: 450 },
                { tag: "600", h: 600 }, { tag: "1000", h: 1000 }]
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
        if (data.h === 1000)
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
    // highlighted and kept by the page
    function test_theWindowIsPickedAboveTheChart() {
        const page = createTemporaryObject(pageComponent, testCase)
        verify(page)
        const before = page.chartWindowS
        for (const s of page.chartWindows)
            verify(findChild(page, "chartWindow_" + s), "a button for " + s + " s")
        findChild(page, "chartWindow_60").clicked()
        compare(page.chartWindowS, 60)
        verify(findChild(page, "chartWindow_60").highlighted)
        verify(!findChild(page, "chartWindow_10").highlighted)
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
