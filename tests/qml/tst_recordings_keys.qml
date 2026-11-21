// File/layer: tests/qml/tst_recordings_keys.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings_keys (test_qml)
//
// The Recordings page's signals by key (ADR-0065 section 9): a format 4
// recording keeps its panes and names its plotted columns by key; an
// older one keeps its panes by name, in its values and its frames alike.

import QtQuick
import QtTest
import App
import AppTest
TestCase {
    id: testCase
    name: "RecordingsKeys"
    when: windowShown

    Component {
        id: pageComponent
        RecordingsPage { width: 900; height: 900 }
    }

    // A binding error in the app's QML fails the test
    function init() {
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        setStoredViews("{}")
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

    readonly property var earlyRow: ({
        title: "2026-11-03 10:00:00 · Early", uri: "replay://x?profile=obd2",
        folder: Harness.recordingFixtures + "/pull-early", source: "", profile: "obd2" })

    // The Export panel over an opened recording, its sizes in
    function openExport(page) {
        find(page, "exportButton").clicked()
        const panel = find(page, "exportPanel")
        tryVerify(() => panel.visible, 2000, "the panel opens")
        return panel
    }
    function sizesIn(panel) {
        tryVerify(() => panel.sizes.length >= 3, 10000, "the sizes come back")
    }
    function restoreExportChoices(panel) {
        panel.rowChoice = "sweep"
        panel.customText = ""
    }

    // The viewer's kept pane arrangements (Settings recordings/views), set
    // or read between cases
    function setStoredViews(text) {
        const store = Qt.createQmlObject("import QtCore; Settings { category: \"recordings\" }", testCase)
        store.setValue("views", text)
        store.sync()
        store.destroy()
    }
    function storedViews() {
        const store = Qt.createQmlObject("import QtCore; Settings { category: \"recordings\" }", testCase)
        store.sync()
        const text = String(store.value("views", "{}"))
        store.destroy()
        return text
    }

    // A format 4 recording of two obd2 signals, its values kept by key
    function keyedRecording(name) {
        const folder = Harness.writeFramesRecording(name, 1, 10000)
        verify(folder !== "", "the folder is made")
        const header = "ecuio session recording, format 4 (ADR-0049)\n"
                       + "started  2026-11-17 09:00:00.000 (local time)\n"
                       + "profile  obd2\n"
                       + "signal  7E8:410C:2.0:16,RPM,number,rpm,0,1,1\n"
                       + "signal  7E8:410D:2.0:8,Vehicle Speed,number,km/h,0,2,6\n"
                       + "\n00:00:05.000  +5.000  end  test: 0 frames, 100 values, "
                       + "0 records dropped, 0 notes dropped\n"
        const rows = ["host_us,signal,value,unit"]
        for (let t = 1; t <= 50; ++t) {
            rows.push((t * 100000) + ",7E8:410C:2.0:16," + (800 + 10 * t) + ",rpm")
            rows.push((t * 100000) + ",7E8:410D:2.0:8," + t + ",km/h")
        }
        verify(Harness.writeScratchFile(name + "/events.log", header).toString() !== "")
        verify(Harness.writeScratchFile(name + "/signals.csv", rows.join("\n") + "\n").toString() !== "")
        return { title: name, uri: "replay://x?profile=obd2", source: "", folder: folder,
                 profile: "obd2" }
    }

    // A format 2 recording of engine speed: its values by name, and the
    // harness's engine speed frames
    function olderRecording(name) {
        const folder = Harness.writeFramesRecording(name, 50, 100000)
        verify(folder !== "", "the folder is made")
        const header = "ecuio session recording, format 2 (ADR-0049)\n"
                       + "started  2026-11-17 09:00:00.000 (local time)\n"
                       + "profile  obd2\n"
                       + "frame flags  complete\n"
                       + "signal  RPM,number,rpm,0,1,1\n"
                       + "\n00:00:05.000  +5.000  end  test: 50 frames, 50 values, "
                       + "0 records dropped, 0 notes dropped\n"
        const rows = ["host_us,signal,value,unit"]
        for (let t = 1; t <= 50; ++t)
            rows.push((t * 100000) + ",RPM,804,rpm")
        verify(Harness.writeScratchFile(name + "/events.log", header).toString() !== "")
        verify(Harness.writeScratchFile(name + "/signals.csv", rows.join("\n") + "\n").toString() !== "")
        return { title: name, uri: "replay://x?profile=obd2", source: "", folder: folder,
                 profile: "obd2" }
    }

    // A signal's pane is kept by its key; an entry an earlier version kept
    // by name is read while the key has none, and a change writes the key
    // and drops the name
    function test_aPaneIsKeptByKeyAndOneKeptByNameIsRead() {
        setStoredViews(JSON.stringify({ "keyed-panes": { used: 1, assign: {
            "RPM": "own:1", "Vehicle Speed": "own:2", "7E8:410D:2.0:8": "own:3" } } }))
        const page = createPage()
        page.openPart.open(keyedRecording("keyed-panes"))
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        const rpm = page.recording.signalList.find(s => s.name === "RPM")
        const speed = page.recording.signalList.find(s => s.name === "Vehicle Speed")
        compare(rpm.key, "7E8:410C:2.0:16")
        compare(page.panesPart.paneOf(rpm), "own:1", "by its name")
        compare(page.panesPart.paneOf(speed), "own:3", "its key before its name")
        compare(page.panesPart.assignRows()[rpm.index], "own:1")
        compare(page.panesPart.assignRows()[speed.index], "own:3")
        // A new pane from the signal's menu
        page.panesPart.openPaneMenu(rpm.index, find(page, "chart"))
        page.panesPart.choosePane("new")
        const mine = page.viewAssign["7E8:410C:2.0:16"]
        verify(mine !== undefined && mine.indexOf("own:") === 0, "by key: " + mine)
        compare(page.viewAssign.RPM, undefined, "the name's entry is gone")
        compare(page.viewAssign["Vehicle Speed"], "own:2", "another's entry stays")
        tryVerify(() => storedViews().indexOf("\"7E8:410C:2.0:16\":\"own:") >= 0, 3000,
                  "kept by key")
        page.panesPart.setPaneOf(speed, "")
        compare(page.viewAssign["7E8:410D:2.0:8"], undefined)
        compare(page.viewAssign["Vehicle Speed"], undefined, "By unit drops both")
    }

    // Two signals of one name share a view an earlier version kept by name:
    // moving one leaves the other where it was
    function test_movingOneOfTwoSignalsOfAName_keepsTheOthersPane() {
        setStoredViews(JSON.stringify({ "twin-panes": { used: 1, assign: { "RPM": "own:1" } } }))
        const page = createPage()
        page.openPart.open(twoRpmRecording("twin-panes", 800, 900))
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        const e8 = page.recording.signalList.find(s => s.key === "7E8:410C:2.0:16")
        const e9 = page.recording.signalList.find(s => s.key === "7E9:410C:2.0:16")
        compare(page.panesPart.paneOf(e8), "own:1")
        compare(page.panesPart.paneOf(e9), "own:1")
        verify(page.panesPart.setPaneOf(e8, "own:2"))
        compare(page.panesPart.paneOf(e8), "own:2")
        compare(page.panesPart.paneOf(e9), "own:1", "the other keeps its pane")
    }

    // The 65th kept signal is refused, so a view written is a view read back
    function test_aMoveBeyondTheKeptBoundIsRefused() {
        const page = createPage()
        page.openPart.open(keyedRecording("bound-panes"))
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        const rpm = page.recording.signalList.find(s => s.name === "RPM")
        const assign = {}
        for (let i = 0; i < ChartViews.viewsMaxSignals; ++i)
            assign["other:" + i] = "own:1"
        page.panesPart.writeView({ assign: assign })
        compare(Object.keys(page.viewAssign).length, ChartViews.viewsMaxSignals)
        verify(!page.panesPart.setPaneOf(rpm, "own:2"), "the 65th is refused")
        compare(Object.keys(page.viewAssign).length, ChartViews.viewsMaxSignals)
        // The kept ones survive a write and a read
        tryVerify(() => {
            const kept = ChartViews.viewsParse(storedViews())[page.viewName]
            return kept !== undefined && Object.keys(kept.assign).length === ChartViews.viewsMaxSignals
        }, 3000)
        verify(page.panesPart.setPaneOf(page.recording.signalList.find(s => s.name === "Vehicle Speed"), ""),
               "clearing a move needs no room")
    }

    // An older recording keeps a pane by name in both decodes: one chosen
    // in its frames open, whose rows have keys, holds for its values too
    function test_anOlderRecordingsPaneIsKeptByNameInBothDecodes() {
        const page = createPage()
        page.openPart.open(olderRecording("older-panes"))
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        compare(page.recording.signalList.find(s => s.name === "RPM").key, "", "names alone")
        page.decodePart.decodeWith("obd2")
        tryVerify(() => page.recording.state === RecordingView.Open
                        && page.recording.decodedWith === "obd2", 10000, "its frames through obd2")
        const framed = page.recording.signalList.find(s => s.name === "RPM")
        compare(framed.key, "7E8:410C:2.0:16", "a frames open has keys")
        page.panesPart.setPaneOf(framed, "own:4")
        compare(page.viewAssign.RPM, "own:4", "kept by name")
        compare(page.viewAssign["7E8:410C:2.0:16"], undefined, "not by key")
        page.decodePart.decodeWith("")
        tryVerify(() => page.recording.state === RecordingView.Open
                        && page.recording.decodedWith === "", 10000, "values as recorded")
        const recorded = page.recording.signalList.find(s => s.name === "RPM")
        compare(recorded.key, "")
        compare(page.panesPart.paneOf(recorded), "own:4", "the pane is kept")
        compare(page.panesPart.assignRows()[recorded.index], "own:4")
    }

    // The plotted export names its columns by key in a format 4 recording,
    // which core takes; by name in an older one
    function test_thePlottedColumnsAreKeysInAFormat4Recording() {
        const page = createPage()
        page.openPart.open(keyedRecording("keyed-export"))
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        tryVerify(() => page.plottedNames.length === 2, 5000, "both plotted")
        compare(page.plottedNames.slice().sort(), ["7E8:410C:2.0:16", "7E8:410D:2.0:8"])
        const panel = openExport(page)
        restoreExportChoices(panel)
        panel.columnChoice = "plotted"
        verify(panel.usePlotted)
        compare(panel.columnNames().slice().sort(), ["7E8:410C:2.0:16", "7E8:410D:2.0:8"])
        sizesIn(panel)
        restoreExportChoices(panel)

        page.openPart.backToList()
        page.openPart.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "pull-early opens")
        verify(page.plottedNames.length > 0)
        const names = page.recording.signalList.map(s => s.name)
        for (const n of page.plottedNames)
            verify(names.indexOf(n) >= 0, n + " is a name")
    }

    // A format 4 recording of two modules' RPM, each rising 10 rpm every
    // tenth of a second from its own start
    function twoRpmRecording(name, from7E8, from7E9) {
        const folder = Harness.writeFramesRecording(name, 50, 100000)
        verify(folder !== "", "the folder is made")
        const header = "ecuio session recording, format 4 (ADR-0049)\n"
                       + "started  2026-11-18 09:00:00.000 (local time)\n"
                       + "profile  obd2\n"
                       + "frame flags  complete\n"
                       + "signal  7E8:410C:2.0:16,RPM,number,rpm,0,1,1\n"
                       + "signal  7E9:410C:2.0:16,RPM,number,rpm,0,2,1\n"
                       + "\n00:00:05.000  +5.000  end  test: 0 frames, 100 values, "
                       + "0 records dropped, 0 notes dropped\n"
        const rows = ["host_us,signal,value,unit"]
        for (let t = 1; t <= 50; ++t) {
            rows.push((t * 100000) + ",7E8:410C:2.0:16," + (from7E8 + 10 * t) + ",rpm")
            rows.push((t * 100000) + ",7E9:410C:2.0:16," + (from7E9 + 10 * t) + ",rpm")
        }
        verify(Harness.writeScratchFile(name + "/events.log", header).toString() !== "")
        verify(Harness.writeScratchFile(name + "/signals.csv", rows.join("\n") + "\n").toString() !== "")
        return { title: name, uri: "replay://x?profile=obd2", source: "", folder: folder,
                 profile: "obd2" }
    }

    // Compare pairs a signal by key, else by a name each recording has once
    // (RecordingView.pairsWith, havne-cli view's rule): two modules' RPM
    // each with its own, for the dashed values, the difference and a rise's
    // alignment, and none with an older recording's one RPM
    function test_compareGoesByKeyElseByANameEachHasOnce() {
        const page = createPage()
        page.openPart.open(twoRpmRecording("pairs-here", 800, 900))
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        page.comparePart.compareWith(twoRpmRecording("pairs-there", 780, 700))
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        const sigs = page.recording.signalList
        const theirs = find(page, "compareView").signalList
        const e8 = sigs.find(s => s.key === "7E8:410C:2.0:16")
        const e9 = sigs.find(s => s.key === "7E9:410C:2.0:16")
        compare(theirs[ComparePairs.pairOf(page.comparePairs, e8.index)].key, e8.key)
        compare(theirs[ComparePairs.pairOf(page.comparePairs, e9.index)].key, e9.key)

        const got = page.comparePart.compareValuesAt(1000)
        compare(got[e8.index].text, "880", "7E8's own there")
        compare(got[e9.index].text, "800", "7E9's own there")
        const picks = {}
        picks[e8.index] = true
        picks[e9.index] = true
        page.plotted = picks
        const pairs = page.seriesPart.differencePairs()
        compare(pairs.length, 2)
        for (const p of pairs)
            compare(theirs[p.theirs].key, sigs[p.mine].key)

        // Rising to 850: here at 0.5 s; there 7E8 at 0.7 s, 7E9 at 1.5 s
        page.comparePart.setAlignOn("rise")
        compare(page.alignSignal, e8.key, "the first number signal, by its key")
        page.alignLevel = 850
        page.comparePart.align()
        tryVerify(() => page.compareAligned, 5000, "aligned on the rise")
        fuzzyCompare(page.compareOffsetMs, -200, 1, "on 7E8's pair, not 7E9's")
        const box = find(page, "alignSignal")
        compare(box.count, 2, "both RPMs listed")
        box.activated(1)
        compare(page.alignSignal, e9.key, "the second, by its key")
        page.comparePart.setAlignOn("start")
        page.comparePart.align()

        page.comparePart.compareWith(olderRecording("pairs-older"))
        tryVerify(() => page.compareAligned && find(page, "compareView").signalList.length === 1,
                  10000, "the older recording opens")
        compare(ComparePairs.pairOf(page.comparePairs, e8.index), undefined, "a name this recording has twice pairs nothing")
        compare(ComparePairs.pairOf(page.comparePairs, e9.index), undefined)
        compare(page.seriesPart.differencePairs().length, 0)

        // The first recording opened again through its frames while
        // comparing: its rows are made again, and their pairs with them
        page.decodePart.decodeWith("obd2")
        tryVerify(() => page.recording.state === RecordingView.Open
                        && page.recording.decodedWith === "obd2", 10000, "its frames through obd2")
        const paired = page.recording.signalList.filter(s => ComparePairs.pairOf(page.comparePairs, s.index) !== undefined)
        compare(paired.map(s => s.name), ["RPM"], "obd2's one RPM, by its name there")
        compare(find(page, "compareView").signalList[ComparePairs.pairOf(page.comparePairs, paired[0].index)].name, "RPM")
    }

    // A rise's signal kept across another decode: an older recording's
    // RPM by name in its values, by obd2's key through its frames, and by
    // name again
    function test_theAlignmentsSignalOutlivesADecodeSwitch() {
        const page = createPage()
        page.openPart.open(olderRecording("align-older"))
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        page.comparePart.compareWith(keyedRecording("align-keyed"))
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        page.comparePart.setAlignOn("rise")
        compare(page.alignSignal, "RPM", "by its name: the values have no key")

        page.decodePart.decodeWith("obd2")
        tryVerify(() => page.recording.state === RecordingView.Open
                        && page.recording.decodedWith === "obd2", 10000, "its frames through obd2")
        const rpm = page.recording.signalList.filter(s => s.name === "RPM")
        compare(rpm.length, 1)
        verify(rpm[0].key !== "", "obd2 gives RPM a key")
        compare(page.alignSignal, rpm[0].key, "the one row of its name, by its key")

        page.decodePart.decodeWith("")
        tryVerify(() => page.recording.state === RecordingView.Open
                        && page.recording.decodedWith === "", 10000, "values as recorded")
        compare(page.alignSignal, "RPM", "by its name again")
        page.comparePart.setAlignOn("start")
    }
}
