// File/layer: tests/qml/RecordingsKit.qml | Qt Quick Test, QML tier
// Testing: QML: the base of the tst_recordings* files (test_qml); not a test itself

import QtQuick
import QtTest
import App
import AppTest

// The Recordings page tests: the page, its stores put back, finding and tapping.
// A file derives from it and sets its `name`; `init` and `cleanup` are the kit.
TestCase {
    id: testCase
    when: windowShown

    Component {
        id: pageComponent
        RecordingsPage { width: 900; height: 900 }
    }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        setStoredRules("{}")
        setStoredViews("{}")
        setHintSeen(false)
    }

    // The second cursor's hint flag (its Settings), so which case measures
    // the hint line never depends on the order the cases ran in
    function setHintSeen(seen) {
        const store = Qt.createQmlObject("import QtCore; Settings { category: \"recordings\" }", testCase)
        store.setValue("spanHintSeen", seen)
        store.sync()
        store.destroy()
    }

    // The viewer's kept stretch levels (its Settings, in the test's scratch
    // store): a case that applies levels or plants some leaves none behind
    function setStoredRules(text) {
        const store = Qt.createQmlObject("import QtCore; Settings { category: \"recordings\" }", testCase)
        store.setValue("stretchRules", text)
        store.sync()
        store.destroy()
    }

    function cleanup() {
        DocumentPicker.setPickerForTesting(false)
        Relay.stop()   // a failed case must not leave Replay off for the next
        Bridge.unitSystem = 0
        Theme.developerMode = false
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

    // The readouts group digits with the system locale, as Live and the gauges do.
    function grouped(n) { return Number(n).toLocaleString(Qt.locale(), 'f', 0) }

    // A change or rate with decimals, in the system locale's form ("-9,5" under a comma locale)
    function decimal(n, places) { return Number(n).toLocaleString(Qt.locale(), 'f', places) }

    function find(item, name) {
        const found = findChild(item, name)
        verify(found, "no " + name)
        return found
    }

    // A row of the recordings list to press, once the page is laid out: where the empty
    // list's hint wraps, a new page owes its column a pass and the list has no height yet
    function listRow(page, index) {
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(index) !== null, 2000, "the row has a delegate")
        verify(waitForPolish(page.Window.window), "the list laid out")
        const row = list.itemAtIndex(index)
        const middle = row.mapToItem(list, row.width / 2, row.height / 2).y
        verify(middle >= 0 && middle < list.height,
               "the row's middle is inside the list: " + middle + " of " + list.height)
        return row
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

    // Opens pull-early with pull-late listed beside it. A recording that
    // opens moves the page's items at the layout's next polish, a frame
    // later; a case whose checks all pass at once would otherwise press
    // where an item was before it opened
    function openEarly() {
        const page = createPage()
        page.recordings = [lateRow, earlyRow]
        page.openPart.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
        verify(waitForPolish(page.Window.window), "the viewer laid out")
        return page
    }

    // Opens the findings fixture from its row and waits for the viewer
    function openFindings() {
        const page = createPage()
        page.recordings = [findingsRow]
        const list = find(page, "recordingList")
        mouseClick(find(listRow(page, 0), "recordingRowArea"))
        verify(page.viewing, "the viewer replaces the list")
        verify(!list.visible)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
        verify(waitForPolish(page.Window.window), "the viewer laid out")
        return page
    }

    // Waits for a picked CSV's guess to show
    function guessed(page) {
        tryVerify(() => !page.importGuessing && !Bridge.recordingImport.busy, 10000,
                  "the guess ends")
        return find(page, "guessPreview")
    }

    // The Export panel over an opened recording, its sizes in
    function openExport(page) {
        find(page, "exportButton").clicked()
        const panel = find(page, "exportPanel")
        tryVerify(() => panel.visible, 2000, "the panel opens")
        return panel
    }

    // The test's window is 200 px: grown to the page so a pointer lands on
    // the chart; the returned function puts it back
    function growWindow(page) {
        const win = page.Window.window
        const size = [win.width, win.height]
        win.width = page.width
        win.height = page.height
        verify(waitForPolish(win), "the grown window laid out")
        return () => { win.width = size[0]; win.height = size[1] }
    }

    // pull-early's chart with its panes drawn, and a point of pane i's plot
    // and of its axis in the chart frame's coordinates
    function panedChart(page) {
        const chart = find(page, "chart")
        tryVerify(() => chart.drawn && chart.drawn.panes.length === 3
                        && page.paneBoxes.length === 3, 5000, "three panes drawn")
        return chart
    }

    // (whole pixels, where a pointer lands)
    function inPlot(chart, i, share) {
        const p = chart.drawn.panes[i]
        return { x: Math.round(chart.x + chart.drawn.left + chart.drawn.plotW / 2),
                 y: Math.round(chart.y + p.top + p.h * (share === undefined ? 0.5 : share)) }
    }

    function rowOf(page, name) {
        const rows = find(page, "signalRows")
        const at = rows.model.findIndex(s => s.name === name)
        verify(at >= 0, "no row " + name)
        tryVerify(() => rows.itemAtIndex(at) !== null, 2000, "the row has a delegate")
        return { item: rows.itemAtIndex(at), index: rows.model[at].index }
    }

    // Both cursors placed, their statistics arrived
    function spanned(page, a, b) {
        page.cursorsPart.setCursor(a)
        page.cursorsPart.setCursorB(b)
        tryVerify(() => Object.keys(page.spanStats).length > 0, 5000, "the statistics arrive")
    }

    // The viewer's kept pane arrangements (Settings recordings/views), set
    // or read between cases
    function setStoredViews(text) {
        const store = Qt.createQmlObject("import QtCore; Settings { category: \"recordings\" }", testCase)
        store.setValue("views", text)
        store.sync()
        store.destroy()
    }

    // A values recording of one RPM signal sampled three to four times a
    // second for ten minutes, swinging through a few thousand rpm: every
    // sample shiftMs later and plus higher, in the unit given; and its row
    function swingRecording(name, shiftMs, plus, unit) {
        const folder = Harness.writeFramesRecording(name, 1, 10000)
        verify(folder !== "", "the folder is made")
        const header = "ecuio session recording, format 2 (ADR-0049)\n"
                       + "started  2026-11-03 09:00:00.000 (local time)\n"
                       + "profile  test\n"
                       + "signal  RPM,number," + unit + ",0,1\n"
                       + "\n00:10:01.000  +601.000  end  test: 0 frames, 0 values, 0 records "
                       + "dropped, 0 notes dropped\n"
        const rows = ["host_us,signal,value,unit"]
        let t = 1000
        for (let k = 0; t < 600000; ++k) {
            const v = Math.round(2500 + 1500 * Math.sin(t / 7000) + 300 * Math.sin(t / 900)) + plus
            rows.push(((t + shiftMs) * 1000) + ",RPM," + v + "," + unit)
            t += k % 3 === 0 ? 333 : 250
        }
        verify(Harness.writeScratchFile(name + "/events.log", header).toString() !== "")
        verify(Harness.writeScratchFile(name + "/signals.csv", rows.join("\n") + "\n").toString() !== "")
        return { title: name, uri: "replay://x?profile=test", source: "", folder: folder,
                 profile: "test" }
    }

    // A page comparing a swing recording with a copy 100 rpm higher, lined
    // up on their starts, RPM plotted, Difference still off
    function swingCompared(name) {
        const here = swingRecording(name, 0, 0, "rpm")
        const higher = swingRecording(name + "-higher", 0, 100, "rpm")
        const page = createPage()
        page.recordings = [higher, here]
        openRow(page, here)
        const picks = {}
        picks[page.shownSignals.find(s => s.name === "RPM").index] = true
        page.plotted = picks
        page.comparePart.compareWith(higher)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        compare(page.compareOffsetMs, 0)
        page.cursorsPart.showAll()
        return page
    }

    // Draws until the drawn grid's difference has its answer
    function differenceRead(page) {
        tryVerify(() => {
            page.seriesPart.series(400)
            return page.difference !== null && page.difference.key === page.differenceWanted.key
        }, 10000, "the view's difference is read")
    }

    // A road log with two pulls (a stab at the throttle and a part-throttle
    // rise that are none), each chosen in turn against pull-early's one:
    // where havne-cli view --align-on pull:2,1 lines them up
    // (tests/golden/cli/view-align-pull.jsonl)
    readonly property var twiceRow: ({
        title: "2026-11-11 09:00:00 · Twice", uri: "", folder: Harness.recordingFixtures + "/pull-twice",
        source: "", profile: "test" })

    // Two key ons and their engine starts, 5.6 s and 12.6 s in
    // (tests/golden/cli/view-engine-start.jsonl)
    readonly property var startEarlyRow: ({
        title: "2026-11-06 09:00:00 · Start early", uri: "replay://s?profile=test",
        folder: Harness.recordingFixtures + "/start-early", source: "", profile: "test" })

    readonly property var startLateRow: ({
        title: "2026-11-06 10:00:00 · Start late", uri: "replay://t?profile=test",
        folder: Harness.recordingFixtures + "/start-late", source: "", profile: "test" })

    // Settings' vehicles: two, the first picked, its baseline the folder
    // given
    function vehicleBook(baseline) {
        const entry = (id, label, folder) => ({
            id: id, label: label, vinKey: "", uri: "", plotted: [], unplotted: [],
            picks: [], own: "", baseline: folder })
        return { salt: "", picked: "v1",
                 list: [entry("v1", "Daily driver", baseline), entry("v2", "Weekend car", "")] }
    }

    function openRow(page, row) {
        page.openPart.open(row)
        tryVerify(() => find(page, "recordingInfo").visible, 10000, "the recording opens")
        verify(waitForPolish(page.Window.window), "the viewer laid out")
    }
}
