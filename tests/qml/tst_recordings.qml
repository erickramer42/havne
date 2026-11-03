// File/layer: tests/qml/tst_recordings.qml | Qt Quick Test, QML tier
// Testing: RecordingsPage.qml (ADR-0049 step 5) against the real bridge
//          and RecordingView: the list from Bridge.recentRecordings and a
//          recording that ends joining it, Replay handing its URI on, Open
//          reaching the viewer on tests/fixtures/recording/findings (its
//          signal, the default plotted, the chart's series over the view,
//          the frames check's findings), the cursor's readout, the view
//          kept inside the recording through setView, zoom, a drag and the
//          wheel, a finding's row centering the view, a refused folder's
//          error, and back to the list. test_qml's recordings go to a
//          scratch folder.

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
        verify(page.cursorMs < 0, "a drag places no cursor")
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

    // A folder the reader refuses says why; All recordings goes back
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
