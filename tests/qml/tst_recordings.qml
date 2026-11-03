// File/layer: tests/qml/tst_recordings.qml | Qt Quick Test, QML tier
// Testing: RecordingsPage.qml (ADR-0049 step 5) against the real bridge
//          and RecordingView: the list from Bridge.recentRecordings and a
//          recording that ends joining it, Replay handing its URI on, Open
//          reaching the viewer on tests/fixtures/recording/findings (its
//          signal, the default plotted, the chart's series over the view,
//          the frames check's findings), the cursor's readout, the view
//          kept inside the recording through setView, zoom, a drag and the
//          wheel, the chart without antialiasing while the view moves and
//          with it at rest, a second recording overlaid (dashed beside the
//          first's rows, sharing their scales) and aligned on the starts, a
//          rise, a marker and the UTC starts as havne-cli view aligns them
//          (tests/fixtures/recording/pull-early and pull-late), a finding's
//          row centering the view, a refused folder's error, and back to
//          the list. test_qml's recordings go to a scratch folder.

import QtQuick
import QtTest
import App
import AppTest
TestCase {
    id: testCase
    name: "Recordings"
    when: windowShown

    Component {
        id: pageComponent
        RecordingsPage { width: 900; height: 900 }
    }

    SignalSpy {
        id: replaySpy
        signalName: "replayRequested"
    }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
    }

    function cleanup() {
        Bridge.recording = false
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
    }

    function createPage() {
        // Under the window's content item: under the TestCase, visible reads false
        const page = createTemporaryObject(pageComponent, testCase.parent)
        verify(page)
        return page
    }

    function find(item, name) {
        const found = findChild(item, name)
        verify(found, "no " + name)
        return found
    }

    readonly property var findingsRow: ({
        title: "2026-11-03 10:00:00 · Findings", uri: "replay://x?profile=obd2",
        folder: Harness.recordingFixtures + "/findings", source: "socketcan://vcan0?profile=obd2",
        profile: "obd2" })

    readonly property var earlyRow: ({
        title: "2026-11-03 10:00:00 · Early", uri: "replay://x?profile=obd2",
        folder: Harness.recordingFixtures + "/pull-early", source: "", profile: "obd2" })
    readonly property var lateRow: ({
        title: "2026-11-03 10:01:30 · Late", uri: "replay://y?profile=obd2",
        folder: Harness.recordingFixtures + "/pull-late", source: "", profile: "obd2" })

    // Opens pull-early with pull-late listed beside it
    function openEarly() {
        const page = createPage()
        page.recordings = [lateRow, earlyRow]
        page.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
        return page
    }

    // Opens the findings fixture from its row and waits for the viewer
    function openFindings() {
        const page = createPage()
        page.recordings = [findingsRow]
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        mouseClick(find(list.itemAtIndex(0), "recordingRowArea"))
        verify(page.viewing, "the viewer replaces the list")
        verify(!list.visible)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
        return page
    }

    // A recording that ends joins the list, newest first, saying what it
    // recorded and how it replays
    function test_aRecordingThatEndsJoinsTheList() {
        const page = createPage()
        const list = find(page, "recordingList")
        const before = list.count
        Bridge.recording = true
        Bridge.connectSessionAsync("mock://recordings-page")
        tryVerify(() => Bridge.connected, 10000, "the mock connects")
        tryVerify(() => Bridge.frameCount > 0, 10000, "a frame is recorded")
        Bridge.disconnectSession()
        tryVerify(() => list.count === Math.min(before + 1, page.recordingsListed), 10000,
                  "the recording that ended is listed")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        compare(find(list.itemAtIndex(0), "recordingDetail").text,
                "mock://recordings-page · replays as mock")
        verify(page.recordings[0].folder !== "", "a row knows its folder")
    }

    // Replay hands the row's replay:// URI on, for Main to put in
    // Settings' field
    function test_replayHandsItsUriOn() {
        const page = createPage()
        page.recordings = [findingsRow]
        replaySpy.target = page
        replaySpy.clear()
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        mouseClick(find(list.itemAtIndex(0), "replayButton"))
        compare(replaySpy.count, 1)
        compare(replaySpy.signalArguments[0][0], findingsRow.uri)
        verify(!page.viewing, "Replay doesn't open the viewer")
    }

    // Open shows the recording: its signal plotted by default, the chart's
    // series over the whole recording, the check's findings listed
    function test_openShowsTheViewer() {
        const page = openFindings()
        compare(find(page, "openedTitle").text, findingsRow.title)
        compare(find(page, "signalRows").count, 1)
        verify(page.plotted[0], "RPM, ranked, is plotted")
        compare(page.viewT0, 0)
        compare(page.viewT1, 20000)
        const series = page.series(400)
        compare(series.length, 1)
        compare(series[0].row, 0)
        verify(series[0].columns.lows.length > 0)
        compare(find(page, "checkSummary").text, "Frames check: 8 findings")
        compare(find(page, "findingList").count, 8)
        verify(find(page, "recordingInfo").text.indexOf("20 s long") === 0,
               find(page, "recordingInfo").text)

        // Unticked, nothing is plotted
        page.togglePlotted(0)
        compare(page.series(400).length, 0)
    }

    // The ticks start at the recording's default: its ranked numbers, never
    // a text signal (pull-early: RPM, Throttle and Coolant, then the VIN)
    function test_theTicksStartAtTheDefault() {
        const page = createPage()
        page.open({ title: "Pull", folder: Harness.recordingFixtures + "/pull-early" })
        const rows = find(page, "signalRows")
        tryVerify(() => rows.visible && rows.count === 4, 10000, "the recording opens")
        const ticked = []
        for (let i = 0; i < 4; ++i) {
            tryVerify(() => rows.itemAtIndex(i) !== null, 2000, "row " + i + " has a delegate")
            const check = find(rows.itemAtIndex(i), "plotCheck")
            ticked.push(check.checked)
            compare(page.plotted[i] === true, check.checked, "row " + i)
        }
        compare(JSON.stringify(ticked), "[true,true,true,false]")
        verify(!find(rows.itemAtIndex(3), "plotCheck").enabled, "a text signal has nothing to plot")
        compare(page.series(400).length, 3)
    }

    // The cursor reads each signal's value in force at its time
    function test_theCursorReadsTheValuesAtItsTime() {
        const page = openFindings()
        compare(find(page, "cursorTime").text, "Tap the chart to read the values at a time")
        const row = find(page, "signalRows").itemAtIndex(0)
        compare(find(row, "cursorValue").text, "—")
        page.setCursor(5000)
        compare(find(page, "cursorTime").text, "At 5.0 s")
        const at = page.readout[0]
        verify(at.atMs >= 0 && at.atMs <= 5000, "the sample in force: " + at.atMs)
        compare(find(row, "cursorValue").text, page.valueText(Number(at.text), 0, "rpm"))
        compare(page.series(400)[0].labelValue, Number(at.text), "the label shows it too")
    }

    // The view stays inside the recording, no narrower than minSpanMs,
    // however it's moved
    function test_theViewStaysInsideTheRecording() {
        const page = openFindings()
        page.setView(-5000, 1000)
        compare(page.viewT0, 0)
        compare(page.viewT1, 6000)
        page.zoomAt(3000, 0.5)
        compare(page.viewT1 - page.viewT0, 3000)
        page.setView(19900, 19950)
        compare(page.viewT1 - page.viewT0, page.minSpanMs)
        compare(page.viewT1, 20000)
        page.zoomAt(10000, 1000)
        compare(page.viewT0, 0)
        compare(page.viewT1, 20000)

        // A drag left moves the view later, a tap places the cursor, the
        // wheel zooms in
        const frame = find(page, "chartFrame")
        page.setView(0, 4000)
        // A drag in steps, as a pointer moves (mouseDrag's two moves don't
        // carry a DragHandler past its threshold)
        // The test's window is 200 px: grown to the page, so the pointer
        // lands on the chart
        const win = frame.Window.window
        const size = [win.width, win.height]
        win.width = page.width
        win.height = page.height
        const x = frame.width / 2, y = frame.height / 2
        mousePress(frame, x, y)
        for (let dx = 10; dx <= 200; dx += 10)
            mouseMove(frame, x - dx, y)
        mouseRelease(frame, x - 200, y)
        verify(page.viewT0 > 0, "dragged later: " + page.viewT0)
        verify(!page.hasCursor, "a drag places no cursor")
        mouseClick(frame, x, y)
        verify(page.cursorMs > page.viewT0 && page.cursorMs < page.viewT1,
               "a tap places the cursor in view: " + page.cursorMs)
        compare(page.viewT1 - page.viewT0, 4000)
        mouseWheel(frame, x, y, 0, 120)
        verify(page.viewT1 - page.viewT0 < 4000, "zoomed in: " + (page.viewT1 - page.viewT0))
        mouseClick(find(page, "zoomAll"))
        compare(page.viewT1 - page.viewT0, 20000)
        win.width = size[0]
        win.height = size[1]
    }

    // While the view moves the chart draws without antialiasing, and once
    // it rests, draws again with it; a single change keeps it
    function test_theChartDropsAntialiasingWhileTheViewMoves() {
        const page = openFindings()
        const chart = find(page, "chart")
        page.setView(0, 4000)
        tryVerify(() => !chart.moving, 2000, "rested after opening")
        page.setView(1000, 5000)
        verify(!chart.moving, "one change is not a move")
        verify(chart.antialiasing)
        page.setView(1100, 5100)
        verify(chart.moving, "a second change soon after is")
        verify(!chart.antialiasing)
        tryVerify(() => !chart.moving, 2000, "the view rests")
        verify(chart.antialiasing)
        // The rest's own paint runs in the next frame's polish, which a
        // slow runner delivers after the rest is seen: count from after it.
        // None comes without it
        verify(waitForPolish(chart), "the rest drawn")
        const paints = chart.paints
        wait(page.settleMs * 2)
        compare(chart.paints, paints, "no paint asked for while resting")
        chart.moving = true
        chart.moving = false
        tryVerify(() => chart.paints > paints, 2000, "drawn again with antialiasing")
    }

    // Another recording overlays the first: its signals of the same names
    // dashed beside their rows' own, sharing their scales, aligned on the
    // starts until told otherwise; Stop takes it away
    function test_aComparedRecordingIsDrawnDashed() {
        const page = openEarly()
        const choice = find(page, "compareChoice")
        compare(choice.count, 2, "the placeholder and the other recording")
        verify(!find(page, "stopCompare").visible)
        choice.activated(1)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        compare(choice.currentText, lateRow.title, "the choice names it")
        verify(find(page, "stopCompare").visible)
        compare(page.compareOffsetMs, 0)
        compare(page.viewT0, 0)
        compare(page.viewT1, 40000, "to the longer one's end")
        const all = page.series(400)
        compare(all.length, 6, "three plotted, each twice")
        const dashed = all.filter(s => s.dash !== undefined)
        compare(dashed.length, 3)
        for (const s of dashed) {
            compare(JSON.stringify(s.dash), JSON.stringify(Theme.seriesCompareDash(s.row)))
            compare(s.scaleKey, s.row)
            verify(all.some(m => m.dash === undefined && m.row === s.row && m.scaleKey === s.row),
                   "beside its row's own, on one scale")
        }
        compare(find(page, "compareStatus").text,
                "Dashed: " + lateRow.title + ", aligned on their starts")
        find(page, "stopCompare").clicked()
        verify(!page.comparing)
        compare(page.series(400).length, 3)
        compare(page.viewT1, 30000)
    }

    // Aligned on a rise, a marker or the UTC starts, where havne-cli view
    // lines the two pulls up (tests/golden/cli/view-rise.jsonl: 14.6 s and
    // 24.6 s; view-marker.jsonl; view-utc.jsonl: 90 s apart)
    function test_theAlignmentsAreHavneClisOwn() {
        const page = openEarly()
        page.compareWith(lateRow)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        const status = find(page, "compareStatus")

        page.setAlignOn("rise")
        compare(page.alignSignal, "RPM", "the first number signal")
        verify(!page.compareAligned, "no level yet")
        verify(status.text.indexOf("type a level") >= 0, status.text)
        page.alignHysteresis = 500
        page.alignLevel = 4000
        verify(page.compareAligned)
        compare(page.compareOffsetMs, -10000)
        compare(page.viewT0, -10000, "from the compared one's start")
        compare(page.viewT1, 30000)
        verify(status.text.indexOf("at 14.6 s here and 24.6 s there") > 0, status.text)

        // One pull, ten seconds apart: at the cursor the two agree, and
        // before the first's start only the compared one has values
        const rpm = find(page, "signalRows").model.find(s => s.name === "RPM").index
        page.setCursor(14600)
        compare(page.compareReadout[rpm].text, page.readout[rpm].text)
        const row = find(page, "signalRows").itemAtIndex(rpm)
        compare(find(row, "compareValue").text, find(row, "cursorValue").text)
        page.setCursor(-5000)
        compare(page.cursorMs, -5000)
        compare(find(page, "cursorTime").text, "At -5.0 s")
        verify(page.readout[rpm] === undefined, "nothing of the first's yet")
        verify(page.compareReadout[rpm].atMs >= 0)

        // Its hysteresis keeps 2010 from being found (view-unaligned.jsonl)
        page.alignHysteresis = 30
        page.alignLevel = 2010
        verify(!page.compareAligned)
        verify(status.text.indexOf("isn't drawn") > 0, status.text)
        compare(page.viewT0, 0, "back to the first alone")
        compare(page.viewT1, 30000)
        compare(page.series(400).length, 3)

        page.setAlignOn("marker")
        compare(page.alignLabel, "pull")
        verify(page.compareAligned)
        compare(page.compareOffsetMs, -10000, "11.5 s here, 21.5 s there")
        page.setAlignOn("utc")
        compare(page.compareOffsetMs, 90000)
        compare(page.viewT1, 130000)
        verify(status.text.indexOf("1:30.0 apart") > 0, status.text)
        page.setAlignOn("start")
        compare(page.compareOffsetMs, 0)

        // Back to the list ends the comparison and its alignment
        find(page, "backToList").clicked()
        verify(!page.comparing)
        compare(page.alignOn, "start")
        compare(page.alignLabel, "")
    }

    // A finding's row centers the view on it and puts the cursor at its
    // start
    function test_aFindingsRowCentersTheView() {
        const page = openFindings()
        page.setView(0, 2000)
        const list = find(page, "findingList")
        tryVerify(() => list.itemAtIndex(1) !== null, 2000, "the row has a delegate")
        // The rate drop: 7.92 s for 1.4 s
        compare(find(list.itemAtIndex(1), "findingText").text, "0x3E9 slowed for 1.4 s")
        mouseClick(find(list.itemAtIndex(1), "findingRowArea"))
        compare(page.viewT0, 7620)
        compare(page.viewT1, 9620)
        compare(page.cursorMs, 7920)
    }

    // The frame test (developer mode only): it moves the view for its
    // seconds, then says what it measured, in the log too, and puts the
    // ticks and the view back
    function test_theFrameTestMeasuresAndPutsTheViewBack() {
        const page = openFindings()
        verify(!find(page, "frameTest").visible, "hidden outside developer mode")
        Theme.developerMode = true
        verify(find(page, "frameTest").visible)
        page.frameTestSeconds = 0.6
        page.setView(5000, 6000)
        let logged = ""
        const log = line => { logged = line }
        Bridge.logMessage.connect(log)
        mouseClick(find(page, "frameTest"))
        verify(page.frameTesting)
        tryVerify(() => !page.frameTesting, 5000, "the test ends")
        Bridge.logMessage.disconnect(log)
        Theme.developerMode = false
        const result = find(page, "frameTestResult").text
        verify(/^Frame test, 1 plotted at \d+ px: [\d.]+ frames\/s shown panning, [\d.]+ zooming; [\d.]+ charts drawn\/s/.test(result),
               result)
        compare(logged, result, "the activity log has it")
        verify(page.plotted[0], "the ticks are back")
        compare(page.viewT0, 0)
        compare(page.viewT1, 20000)
    }

    // A folder the reader refuses says why; All recordings goes back
    // Finding a signal among many (ADR-0050): a box for each group the
    // recording holds, words and a group narrowing its rows, Plotted
    // only; back to the list clears the filter
    function test_theFilterNarrowsTheSignalRows() {
        const page = openEarly()
        const rows = find(page, "signalRows")
        const all = rows.count
        const bar = find(page, "recordingFilter")
        // Edited as typing edits it, keeping the field's binding
        const field = find(bar, "filterText")
        field.insert(0, "THR")
        field.textEdited()
        tryCompare(rows, "count", 1)
        compare(rows.model[0].name, "Throttle")
        field.remove(0, field.length)
        field.textEdited()
        tryCompare(rows, "count", all)

        mouseClick(find(bar, "filterButton"))
        tryVerify(() => bar.options.opened, 2000, "the filter's options open")
        const boxes = bar.options.contentItem
        for (const code of [0, 1, 2])
            verify(findChild(boxes, "group_" + code), "a box for group " + code)
        verify(!findChild(boxes, "group_3"))
        // A box's own handler, as a tap runs it
        findChild(boxes, "group_1").toggled()
        tryCompare(rows, "count", 2)
        compare(rows.model.map(s => s.name).join(","), "RPM,Coolant")

        const plottedOnly = findChild(boxes, "plottedOnly")
        plottedOnly.toggle()
        plottedOnly.toggled()
        const plotted = rows.model.filter(s => page.plotted[s.index] === true).length
        tryCompare(rows, "count", plotted)
        bar.options.close()

        mouseClick(find(page, "backToList"))
        page.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens again")
        compare(find(page, "signalRows").count, all, "back to the list cleared the filter")
    }

    function test_aRefusedFolderSaysWhyAndBackGoesToTheList() {
        const page = createPage()
        page.open({ title: "Gone", folder: Harness.recordingFixtures + "/no-such-recording" })
        const error = find(page, "openError")
        tryVerify(() => error.visible, 10000, "refused")
        verify(error.text.indexOf("signals.csv") >= 0, error.text)
        mouseClick(find(page, "backToList"))
        verify(!page.viewing)
        verify(find(page, "recordingList").visible)
    }
}
