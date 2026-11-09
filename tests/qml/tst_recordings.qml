// File/layer: tests/qml/tst_recordings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings (test_qml)

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

    // A recording that opens moves the page's items at the layout's next
    // polish, a frame later; a case whose checks all pass at once would
    // otherwise press where an item was before it opened
    // Opens pull-early with pull-late listed beside it
    function openEarly() {
        const page = createPage()
        page.recordings = [lateRow, earlyRow]
        page.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens")
        verify(waitForPolish(page.Window.window), "the viewer laid out")
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
        verify(waitForPolish(page.Window.window), "the viewer laid out")
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

    // A CSV import (ADR-0054's appendix "the CSV import as built") is
    // listed and opens from its row: values alone, so no Replay, no frames
    // check, no decode through a profile, and its numbers plotted
    function test_aCsvImportIsListedAndOpensWithoutFrames() {
        const folder = Harness.importCsvFixture("logger-a", "20261105-220322 Logger A")
        verify(folder !== "", "the fixture imports")
        const page = createPage()
        page.refresh()
        const at = page.recordings.findIndex(r => r.folder.endsWith("/20261105-220322 Logger A"))
        verify(at >= 0, "the import is listed")
        compare(page.recordings[at].title, "2026-11-05 22:03:22 · Logger A")
        compare(page.recordings[at].frames, false)
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(at) !== null, 2000, "the row has a delegate")
        const row = list.itemAtIndex(at)
        compare(find(row, "recordingDetail").text,
                "values alone: the recording has no frames to replay")
        verify(!find(row, "replayButton").visible, "nothing to replay")
        mouseClick(find(row, "recordingRowArea"))
        verify(page.viewing, "the viewer replaces the list")
        tryVerify(() => find(page, "signalRows").visible, 10000, "the import opens")
        verify(waitForPolish(page.Window.window), "the viewer laid out")
        compare(find(page, "signalRows").count, 3)
        compare(find(page, "checkSummary").text, "Frames check: the recording has no frames")
        verify(!find(page, "replayView").visible, "no Replay in Settings")
        verify(!find(page, "decodeChoice").visible, "no frames to decode")
        compare(page.series(400).length, 3, "its numbers are plotted")
    }

    // Import: a picked file's kind guessed from its first line; a CSV
    // given a column map file imports through it and joins the list (the
    // guess is skipped); a candump
    // log of two buses is refused in core's words, the interface field
    // shows, and the bus named imports; the kind switches; Cancel closes
    function test_importMakesARecordingOfALoggersFile() {
        const page = createPage()
        verify(find(page, "importButton").visible)
        const panel = find(page, "importPanel")
        const note = find(page, "importNote")
        verify(!panel.visible, "closed until a file is picked")

        const csv = Harness.writeScratchFile("import-log.csv",
                                             "time,rpm,note\n0,800,a\n0.5,900,b\n")
        const map = Harness.writeScratchFile("import-map.json",
            '{"havne_column_map": 1, "time": {"column": "time", "unit": "s"}, '
            + '"signals": [{"column": "rpm", "name": "RPM", "unit": "rpm"}]}')
        page.chooseImportFile(csv)
        verify(panel.visible)
        compare(find(page, "importFileName").text, "Import import-log.csv")
        verify(find(page, "importKind_csv").selected, "guessed from its first line")
        compare(find(page, "importMapName").text, "Guessed from the header")
        verify(!find(page, "importInterface").visible)
        tryVerify(() => !Bridge.recordingImport.busy, 10000, "the guess ends")
        verify(!find(page, "importStart").enabled, "its time has no unit yet")
        page.chooseImportMap(map)
        verify(!find(page, "guessPreview").visible, "a map file skips the guess")
        compare(find(page, "importMapName").text, "import-map.json")
        verify(find(page, "importStart").enabled)
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(note.text, "Imported 2 values; columns ignored: 1")
        verify(note.visible)
        verify(Qt.colorEqual(note.color, Theme.success))
        verify(!panel.visible, "done with the file")
        verify(page.recordings.some(r => r.title.endsWith(" · CSV import")),
               "it joins the list")

        const log = Harness.writeScratchFile("import-two.log",
            "(1.000000) can0 7E8#01\n(1.100000) can1 123#02\n(1.600000) can1 123#03\n")
        page.chooseImportFile(log)
        verify(!note.visible, "a new file, no old words")
        verify(find(page, "importKind_candump").selected, "guessed from its first line")
        verify(!find(page, "importMapName").visible, "a candump log has no column map")
        verify(!find(page, "importInterface").visible, "until core asks for it")
        verify(find(page, "importStart").enabled)
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(note.text, "the log has several interfaces: name one of can0 and can1")
        verify(Qt.colorEqual(note.color, Theme.danger))
        verify(panel.visible, "the file kept")
        verify(find(page, "importInterface").visible)
        find(page, "importInterface").text = "can1"
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(note.text, "Imported 2 frames over 0.5 s")
        verify(!panel.visible)
        verify(page.recordings.some(r => r.title.endsWith(" · candump import")))

        // The next log needs no interface: the one typed for the last is gone
        const one = Harness.writeScratchFile("import-one.log", "(1.000000) can0 7E8#01\n")
        page.chooseImportFile(one)
        compare(find(page, "importInterface").text, "")
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(note.text, "Imported 1 frames over 0.0 s")

        page.chooseImportFile(log)
        find(page, "importKind_csv").clicked()
        compare(page.importKind, RecordingImport.Csv)
        tryVerify(() => !Bridge.recordingImport.busy, 10000, "the guess ends")
        verify(!find(page, "importStart").enabled, "a candump log guesses no map")
        find(page, "importCancel").clicked()
        verify(!panel.visible)
        verify(!note.visible)
    }

    // Waits for a picked CSV's guess to show
    function guessed(page) {
        tryVerify(() => !page.importGuessing && !Bridge.recordingImport.busy, 10000,
                  "the guess ends")
        return find(page, "guessPreview")
    }
    function guessRow(preview, i) {
        return find(preview, "guessRow_" + i)
    }

    // A CSV with no column map: its guess is shown (the time column and
    // its unit from the header, each other column with its name, unit and
    // kind and the first rows of its cells); an edit changes the map the
    // import gets, and the recording holds what the edits left
    function test_aCsvIsImportedThroughItsGuessedMapWithEdits() {
        const page = createPage()
        const csv = Harness.writeScratchFile("guess-log.csv",
            "Time (ms),RPM [rpm],Note,Load [%]\n0,800,a,12\n500,900,b,30\n1000,950,c,55\n")
        page.chooseImportFile(csv)
        const preview = guessed(page)
        verify(preview.visible)
        compare(find(page, "importMapName").text, "Guessed from the header")
        compare(find(preview, "guessTime").text, "Time: Time (ms)")
        compare(find(preview, "guessTimeFixed").text, "Its unit: ms")
        compare(find(preview, "guessTimeSamples").text, "0  500  1000")
        verify(!find(preview, "guessWaiting").visible)
        compare(preview.columns.length, 3, "the header columns but the time's")
        const rpm = guessRow(preview, 0)
        compare(find(rpm, "name").text, "RPM")
        compare(find(rpm, "unit").text, "rpm")
        compare(find(rpm, "kind").text, "number")
        compare(find(rpm, "samples").text, "800  900  950")
        compare(find(guessRow(preview, 1), "kind").text, "text")
        verify(find(page, "importStart").enabled, "its unit is known")

        // Edits: a name, Note out, Load's kind text
        find(rpm, "name").text = "Engine speed"
        const note = find(guessRow(preview, 1), "include")
        note.toggle()
        note.toggled()
        compare(preview.included, 2)
        find(guessRow(preview, 2), "kind").clicked()
        compare(find(guessRow(preview, 2), "kind").text, "text")
        const map = JSON.parse(preview.mapText())
        compare(map.signals.length, 2)
        compare(map.signals[0].name, "Engine speed")
        compare(map.signals[1].column, "Load [%]")
        compare(map.signals[1].kind, "text")

        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(find(page, "importNote").text, "Imported 6 values; columns ignored: 1")
        verify(!find(page, "importPanel").visible, "done with the file")
    }

    // A time of plain numbers has no unit to guess: Import waits for the
    // choice and says so
    function test_aTimeWithoutAUnitWaitsForOne() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-plain.csv",
            "time,rpm\n0,800\n1,900\n2,950\n"))
        const preview = guessed(page)
        verify(find(preview, "guessWaiting").visible)
        compare(find(preview, "guessWaiting").text, "Choose the time column's unit to import")
        verify(!find(page, "importStart").enabled)
        find(preview, "guessTimeUnit_ms").clicked()
        verify(!find(preview, "guessWaiting").visible)
        verify(find(page, "importStart").enabled)
        compare(JSON.parse(preview.mapText()).time.unit, "ms")
        find(preview, "guessTimeUnit_us").clicked()
        compare(JSON.parse(preview.mapText()).time.unit, "us")
    }

    // Save as map is enabled exactly when Import would be: a map with no
    // time unit is one every later import refuses
    function test_saveWaitsForATimeUnitToo() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-save-unit.csv",
            "time,rpm\n0,800\n1,900\n2,950\n"))
        const preview = guessed(page)
        verify(!preview.ready)
        verify(preview.included > 0)
        verify(!find(preview, "saveMapButton").enabled, "no unit chosen")
        find(preview, "guessTimeUnit_ms").clicked()
        verify(find(preview, "saveMapButton").enabled, "unit chosen")
    }

    // A column whose header the file has twice is left out for good
    function test_aDuplicatedHeaderColumnCantBePutBack() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-dup.csv",
            "time (s),rpm,dup,dup\n0,800,1,2\n1,900,3,4\n2,950,5,6\n"))
        const preview = guessed(page)
        compare(preview.columns.length, 3)
        verify(!find(guessRow(preview, 1), "include").enabled, "the header names it twice")
        verify(!find(guessRow(preview, 2), "include").enabled)
        verify(find(guessRow(preview, 1), "why").text.indexOf("put it back") < 0)
    }

    // A time with a zone to give, not a unit, can't be fixed here
    function test_aTimeThatNeedsAZoneAsksForAMap() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-zone.csv",
            "time,rpm\n2026-11-05T22:03:22,800\n2026-11-05T22:03:23,900\n2026-11-05T22:03:24,950\n"))
        const preview = guessed(page)
        verify(find(preview, "guessWaiting").visible)
        verify(find(preview, "guessWaiting").text.indexOf("column map that says its zone") >= 0)
        verify(!find(preview, "guessTimeUnit_ms").visible, "no unit to choose")
        verify(!find(page, "importStart").enabled)
    }

    // A file core can't guess a map for is refused in core's words, with
    // the way out
    function test_aFileWithNoHeaderIsRefusedInCoresWords() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-none.csv",
                                                       "just,words\nmore,words\n"))
        guessed(page)
        const note = find(page, "importNote")
        verify(note.visible)
        verify(Qt.colorEqual(note.color, Theme.danger))
        verify(note.text.startsWith("no row within the file's first 1000"), note.text)
        verify(note.text.endsWith("Column map... picks a map instead."), note.text)
        verify(!find(page, "importStart").enabled)
        verify(find(page, "importMapButton").enabled)
    }

    // A column the guess left out can be put back, as text named by its
    // header; one with no header can't
    function test_aLeftOutColumnCanBePutBack() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-back.csv",
            "time (s),rpm,empty,\n0,800,,\n1,900,,\n2,950,,\n"))
        const preview = guessed(page)
        compare(preview.columns.length, 3)
        compare(preview.included, 1)
        const empty = guessRow(preview, 1)
        verify(find(empty, "why").visible)
        verify(find(empty, "why").text.indexOf("Check it to put it back") >= 0, find(empty, "why").text)
        verify(find(empty, "include").enabled)
        const unnamed = guessRow(preview, 2)
        verify(!find(unnamed, "include").enabled, "no header to name it by")
        verify(find(unnamed, "why").text.indexOf("put it back") < 0)
        const box = find(empty, "include")
        box.toggle()
        box.toggled()
        compare(preview.included, 2)
        compare(find(empty, "name").text, "empty")
        compare(find(empty, "kind").text, "text")
        const map = JSON.parse(preview.mapText())
        compare(map.signals.map(s => s.column).join(), "rpm,empty")
    }

    // A refused import keeps the panel and the edits; core's words say why
    function test_aRefusedGuessImportKeepsItsEdits() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-refused.csv",
            "time (s),rpm\n0,800\n1,900\n2,950\n"))
        const preview = guessed(page)
        const longName = "n".repeat(40)
        find(guessRow(preview, 0), "name").text = longName
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        verify(page.importFailed)
        verify(Qt.colorEqual(find(page, "importNote").color, Theme.danger))
        verify(find(page, "importPanel").visible, "the panel stays")
        compare(find(guessRow(preview, 0), "name").text, longName, "the edit stays")
        find(guessRow(preview, 0), "name").text = "RPM"
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && !page.importFailed, 10000,
                  "the corrected import ends")
        verify(!find(page, "importPanel").visible)
    }

    // Save as map keeps the edited map under a safe name, never over
    // another; it is offered next time, and picking it skips the guess
    function test_aGuessedMapIsSavedAndOfferedNextTime() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-save.csv",
            "time (s),rpm\n0,800\n1,900\n2,950\n"))
        const preview = guessed(page)
        const stamp = String(Date.now())
        const safe = "my-map-" + stamp
        find(preview, "saveMapName").text = "my map " + stamp
        find(preview, "saveMapButton").clicked()
        compare(find(preview, "saveMapNote").text, "Saved as " + safe)
        verify(!preview.saveFailed)
        verify(page.savedMaps.some(m => m.name === safe), "listed")
        find(preview, "saveMapButton").clicked()
        verify(preview.saveFailed, "never over another")
        compare(find(preview, "saveMapNote").text, "a map named " + safe + " is saved already")
        find(preview, "saveMapName").text = ""
        find(preview, "saveMapButton").clicked()
        compare(find(preview, "saveMapNote").text, "give the map a name")
        find(preview, "saveMapName").text = "left over"

        // Next file: the saved map is offered; picking it skips the guess
        page.chooseImportFile(Harness.writeScratchFile("guess-next.csv",
            "time (s),rpm\n0,800\n1,900\n"))
        verify(page.importGuessing, "reading the second file")
        compare(find(preview, "saveMapNote").text, "", "the last file's note is gone while reading")
        verify(!find(preview, "saveMapNote").visible)
        const second = guessed(page)
        compare(second, preview, "one preview serves each file")
        compare(find(second, "saveMapNote").text, "", "the last file's note is gone")
        verify(!find(second, "saveMapNote").visible)
        compare(find(second, "saveMapName").text, "")
        compare(second.columns.length, 1, "the second file's columns")
        verify(find(page, "savedMaps").visible)
        const at = page.savedMaps.findIndex(m => m.name === safe)
        page.chooseImportMap(page.savedMaps[at].url)
        compare(find(page, "importMapName").text, safe + ".json")
        verify(!preview.visible)
        verify(find(page, "importStart").enabled)
        find(page, "useTheGuess").clicked()
        guessed(page)
        verify(preview.visible)
        compare(find(page, "importMapName").text, "Guessed from the header")
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

    // The viewer's Replay view hands on the part in view as a stretch of
    // the row's URI, in the recording's seconds; the whole recording in
    // view, the row's URI alone
    function test_replayViewHandsOnTheStretchInView() {
        const page = openFindings()
        replaySpy.target = page
        replaySpy.clear()
        const button = find(page, "replayView")
        verify(button.visible)
        button.clicked()
        compare(replaySpy.signalArguments[0][0], findingsRow.uri, "all in view")
        page.setView(2500, 7250)
        button.clicked()
        compare(replaySpy.signalArguments[1][0], findingsRow.uri + "&from=2.5&to=7.25")
        page.setView(0, 4000)
        button.clicked()
        compare(replaySpy.signalArguments[2][0], findingsRow.uri + "&from=0&to=4")
        compare(replaySpy.count, 3)
        compare(button.text, "Replay in Settings", "the name says where it goes")

        // Off while a session runs, as the list's Replay is
        Bridge.connectSessionAsync("mock://replay-view")
        tryVerify(() => Bridge.connected, 10000, "the mock connects")
        verify(!button.enabled, "off while connected")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        verify(button.enabled, "on again")
    }

    // Open shows the recording: its signal plotted by default, the chart's
    // series over the whole recording, the check's findings listed
    // Save report: the whole recording's (the zoom never narrows it), on
    // the view's thread, the picked vehicle's baseline named, the note and
    // one log line when it ends
    function test_saveReportWritesTheWholeRecording() {
        const page = openFindings()
        const button = find(page, "saveReport")
        verify(button.visible)
        verify(button.enabled)
        page.zoomAt(5000, 0.25)
        verify(page.viewT1 - page.viewT0 < 20000, "zoomed in")
        const url = Harness.writeScratchFile("findings.json", "")
        verify(url.toString() !== "")
        page.vehicles = vehicleBook(startEarlyRow.folder)
        verify(page.saveReportTo(url, 1))
        const note = find(page, "reportNote")
        tryVerify(() => note.text.indexOf("Report saved: ") === 0, 10000, note.text)
        verify(/^Report saved: \d+ ECUs?, \d+ trouble codes?, \d+ key values?$/.test(note.text),
               note.text)
        verify(button.enabled, "free again once it ends")
        const report = JSON.parse(Harness.readScratchFile(url))
        compare(report.source.kind, "recording")
        compare(report.source.from_s, 0)
        verify(report.baseline.read, "the baseline's start-early recording")
        compare(report.baseline.started, "2026-11-06T15:00:00Z")
        verify(!report.baseline.is_source, "another recording")
        verify(report.key_values.length > 0, "RPM's value at the end")

        // Another recording opened: the note is the old one's no more
        page.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the next opens")
        compare(note.text, "")
    }

    // The viewer's Frames view (ADR-0049's appendix "frames read back"):
    // the switch is there for a recording with frames, the table holds the
    // rows of the part in view, times on the viewer's axis, and a tap puts
    // the cursor at the row's time
    function test_theFramesViewShowsThePartInView() {
        const page = openFindings()
        const values = find(page, "detailView_values")
        const frames = find(page, "detailView_frames")
        verify(values.visible && frames.visible, "a recording with frames has the switch")
        compare(page.detailView, "values", "the values each time it opens")
        verify(find(page, "signalRows").visible)
        verify(!find(page, "viewerFrames").visible)

        mouseClick(frames)
        verify(page.framesShown)
        verify(find(page, "viewerFrames").visible)
        verify(!find(page, "signalRows").visible, "the values give way")
        verify(!find(page, "recordingFilter").visible)
        const words = find(page, "framesWords")
        tryVerify(() => /^\d+ frames? in view$/.test(words.text), 10000,
                  "the part in view is read: " + words.text)
        const total = parseInt(words.text)
        verify(total > 0, words.text)
        const list = find(find(page, "viewerFrameTable"), "frameList")
        compare(list.count, total)

        // The part in view alone: zoomed to the first second there are
        // fewer rows, once the view has rested
        page.setView(0, 1000)
        tryVerify(() => parseInt(words.text) < total, 10000,
                  "the rows follow the view: " + words.text)

        // The rows run oldest first in the viewer, each at its host_us:
        // the first at 0.100 s, the frame's own 0.101 s not shown
        tryVerify(() => list.itemAtIndex(0) !== null)
        const first = list.itemAtIndex(0)
        compare(first.model.timestampUs, 100000)

        // A tap puts the cursor at the row's time
        verify(!page.hasCursor)
        mouseClick(find(first, "frameRowArea"))
        verify(page.hasCursor)
        compare(page.cursorMs, 100)

        // A filter on the frames carries to nothing else: the next
        // recording opened has none
        mouseClick(find(page, "framesFilterToggle"))
        const ids = find(page, "filterIds")
        ids.text = "7E8"
        find(page, "filterPanel").apply()
        compare(find(page, "framesFilterToggle").text, "Filter (1)")

        // Back to the values; the frames are let go
        mouseClick(values)
        verify(!page.framesShown)
        verify(find(page, "signalRows").visible)
        compare(page.detailView, "values")

        page.backToList()
        page.open(findingsRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens again")
        compare(find(page, "filterIds").text, "")
        compare(find(page, "framesFilterToggle").text, "Filter")
    }

    // The viewer's By ID list: one row per ID in view, and a tap on one
    // puts its ID in the filter and returns to the table
    function test_theViewersByIdListTakesATapAsAFilter() {
        const page = openFindings()
        mouseClick(find(page, "detailView_frames"))
        const words = find(page, "framesWords")
        tryVerify(() => /^\d+ frames? in view$/.test(words.text), 10000, words.text)
        verify(!find(page, "viewerFrameIds").visible, "the table first")

        mouseClick(find(page, "framesTable_byId"))
        verify(page.framesById)
        const idsView = find(page, "viewerFrameIds")
        verify(idsView.visible)
        verify(!find(find(page, "viewerFrameTable"), "frameList").visible)
        const ids = find(idsView, "idList")
        tryVerify(() => ids.count >= 1 && ids.itemAtIndex(0) !== null && ids.height > 0
                  && ids.itemAtIndex(0).width > 0, 10000, "the IDs show, laid out")
        verify(idsView.refreshing, "read again while it shows")

        const row = ids.itemAtIndex(0)
        const id = row.model.arbId
        mouseClick(find(row, "idRowArea"))
        verify(!page.framesById, "back to the table")
        verify(page.framesFilterOpen, "the filter opens")
        compare(find(page, "filterIds").text, id.toString(16).toUpperCase())
        compare(find(page, "framesFilterToggle").text, "Filter (2)", "the ID and its width")
        verify(!idsView.refreshing, "not read while hidden")
    }

    function test_aRecordingWithNoFramesHasNoFramesSwitch() {
        const page = createPage()
        const folder = Harness.importCsvFixture("logger-a", "viewer-frames-import")
        verify(folder !== "", "the import is made")
        page.open({ title: "Imported", folder: folder })
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        verify(!find(page, "detailView_frames").visible)
        verify(!find(page, "detailView_values").visible)
        compare(find(page, "viewerFrames").visible, false)
    }

    // A part with more rows than the table holds says where the table
    // begins; a zoom into fewer drops the note
    function test_aLongPartSaysWhereTheTableBegins() {
        const folder = Harness.writeFramesRecording("viewer-frames-long", 10200, 1000)
        verify(folder !== "")
        const page = createPage()
        page.open({ title: "Long", folder: folder })
        // Each wait covers a read of 10,200 rows and a 10,000-row table
        // on whatever else the machine is doing (a build beside the
        // suite took the open past 10 s); the case measures the cap and
        // the words, nothing about time, so the waits are long
        const longWait = 60000
        tryVerify(() => page.viewing && find(page, "detailView_frames").visible, longWait)
        // The switch reaches its place at the layout's next polish: a press
        // before that lands where it isn't
        verify(waitForPolish(page.Window.window), "the viewer laid out")
        mouseClick(find(page, "detailView_frames"))
        verify(page.framesShown, "the press reached the switch")
        const words = find(page, "framesWords")
        tryVerify(() => words.text === "the first 10,000 frames from 0.0 s: zoom in for the rest",
                  longWait, "the table's cap is said")
        compare(find(find(page, "viewerFrameTable"), "frameList").count, 10000)
        page.setView(0, 2000)
        tryVerify(() => words.text === "2000 frames in view", longWait, "the rows of 2 s")

        // A view that does not start at the recording's start: the rows
        // are those from its left edge
        page.setView(5000, 6000)
        tryVerify(() => words.text === "1001 frames in view", longWait, "the rows from 5 s")
        const list = find(find(page, "viewerFrameTable"), "frameList")
        tryVerify(() => list.itemAtIndex(0) !== null, longWait)
        compare(list.itemAtIndex(0).model.timestampUs, 5000000)
    }

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

    // The values from the frames (ADR-0049's appendix of 2026-11-06): the
    // choice offers the app's profiles, the open goes through the one
    // picked, the info says so, and "as recorded" goes back to signals.csv
    function test_theValuesCanComeFromTheFramesThroughAProfile() {
        const page = openFindings()
        const choice = find(page, "decodeChoice")
        verify(choice.visible)
        compare(choice.currentIndex, 0, "as recorded")
        const obd2 = choice.indexOfValue("obd2")
        verify(obd2 > 0, "obd2 is offered")
        verify(choice.indexOfValue("mock") > 0, "so is the mock")
        compare(choice.indexOfValue("signals.json"), -1, "it has no file of its own")
        choice.activated(obd2)
        const info = find(page, "recordingInfo")
        tryVerify(() => info.visible
                  && info.text.indexOf("values decoded from its frames through obd2") > 0,
                  10000, "the frames open")
        compare(choice.currentIndex, obd2)
        compare(find(page, "signalRows").count, 1)
        verify(page.series(400).length === 1, "RPM is plotted")
        choice.activated(0)
        tryVerify(() => info.visible && info.text.indexOf("values decoded") < 0,
                  10000, "signals.csv again")
        compare(choice.currentIndex, 0)
    }

    // The ticks start at the recording's default: its ranked numbers, never
    // a text signal (pull-early: RPM, Throttle and Coolant, then the VIN)
    // A pan keeps each column's samples together: the columns lie on a
    // grid from time 0, so after a pan by part of a column the columns
    // both views share hold the same lowest and highest
    function test_aPanKeepsTheColumnsOnTheirGrid() {
        const page = openFindings()
        page.viewT0 = 2000
        page.viewT1 = 12000
        const before = page.series(400)[0].columns
        const colMs = (before.t1 - before.t0) / before.lows.length
        verify(colMs > 0 && Math.abs(before.t0 / colMs - Math.round(before.t0 / colMs)) < 1e-6,
               "the grid starts on a whole column: " + before.t0)
        page.viewT0 += colMs * 0.4
        page.viewT1 += colMs * 0.4
        const after = page.series(400)[0].columns
        const shift = Math.round((after.t0 - before.t0) / colMs)
        let shared = 0, differ = 0
        for (let c = 0; c + shift < before.lows.length && c < after.lows.length; ++c) {
            if (!isFinite(after.lows[c]) && !isFinite(before.lows[c + shift]))
                continue
            ++shared
            if (after.lows[c] !== before.lows[c + shift] || after.highs[c] !== before.highs[c + shift])
                ++differ
        }
        verify(shared > 10, "columns to compare: " + shared)
        compare(differ, 0, "columns regrouped by the pan")
    }

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

    // A tap lands on the first plotted signal's sample nearest it, so its
    // value is the one the line has there; each trace's label carries its
    // value's sample time, where the chart puts a dot. With nothing
    // plotted a tap stays where it fell
    // An enum signal's cursor value is its label, not its raw number
    function test_theCursorReadsAnEnumsLabel() {
        const page = createPage()
        page.open({ title: "Fuel", folder: Harness.recordingFixtures + "/fuel-status" })
        const rows = find(page, "signalRows")
        tryVerify(() => rows.visible && rows.count === 2, 10000, "the recording opens")
        let row = null
        for (let i = 0; i < 2 && row === null; ++i) {
            tryVerify(() => rows.itemAtIndex(i) !== null, 2000, "row " + i + " has a delegate")
            if (rows.itemAtIndex(i).modelData.name === "Fuel System 1")
                row = rows.itemAtIndex(i)
        }
        verify(row !== null, "the enum's row")
        page.setCursor(1000)
        compare(find(row, "cursorValue").text, "Open loop, cold")
        page.setCursor(4200)
        compare(find(row, "cursorValue").text, "Closed loop")
    }

    function test_aTapSnapsToTheNearestSample() {
        const page = openEarly()
        const sigs = find(page, "signalRows").model
        const rpm = sigs.find(s => s.name === "RPM").index
        const throttle = sigs.find(s => s.name === "Throttle").index
        page.plotted = ({ [rpm]: true, [throttle]: true })
        // RPM every 100 ms from 100 ms, Throttle 1 ms after each
        compare(page.snapped(1140), 1100)
        compare(page.snapped(1160), 1200)
        page.setView(0, 4000)
        page.setCursor(page.snapped(1160))
        compare(page.readout[rpm].atMs, 1200)
        const labelMs = row => page.series(400).find(s => s.row === row).labelMs
        compare(labelMs(rpm), 1200, "RPM's dot on its sample, at the cursor")
        compare(labelMs(throttle), 1101, "Throttle's on its own, 99 ms behind")
        page.setView(2000, 4000)
        verify(isNaN(labelMs(rpm)), "no dot for a cursor out of view")
        page.plotted = ({})
        compare(page.snapped(1160), 1160)
    }

    // Maximized on a wide page, the cursor's values sit in a strip beside
    // the chart; its button folds it to a tab, giving the plot the width,
    // and opens it again, and the choice is kept
    function test_theCursorsStripFoldsAwayAndBack() {
        const page = openFindings()
        page.height = 600
        page.stripFolded = false
        page.toggleMaximized()
        verify(page.chartMaximized, "maximized")
        page.setCursor(5000)
        const strip = find(page, "cursorStrip")
        const fold = find(page, "stripFold")
        const chart = find(page, "chart")
        tryVerify(() => strip.visible && fold.visible, 2000, "a cursor: its values beside the chart")
        verify(waitForPolish(page.Window.window), "laid out")
        const open = chart.shownW
        compare(fold.Accessible.name, "Hide the values at the cursor")

        mouseClick(fold)
        verify(page.stripFolded, "folded, and kept")
        verify(!strip.visible, "the strip gives way")
        verify(fold.visible, "its tab stays to open it again")
        tryVerify(() => chart.shownW > open + 60, 2000,
                  "the plot has the width: " + chart.shownW + " against " + open)
        compare(fold.Accessible.name, "Show the values at the cursor")

        mouseClick(fold)
        verify(!page.stripFolded)
        tryVerify(() => strip.visible && chart.shownW === open, 2000, "the strip is back")
        page.toggleMaximized()
        page.height = 900
    }

    // On a tall page (a phone upright) the values go below the chart in a
    // band, so the plot keeps the width; the fold gives the height back
    function test_theCursorsStripGoesBelowTheChartOnATallPage() {
        const page = openFindings()
        page.width = 400
        page.stripFolded = false
        page.toggleMaximized()
        page.setCursor(5000)
        const frame = find(page, "chartFrame")
        const strip = find(page, "cursorStrip")
        const fold = find(page, "stripFold")
        const chart = find(page, "chart")
        tryVerify(() => strip.visible && frame.height > frame.width, 2000, "a tall frame, its values shown")
        verify(waitForPolish(page.Window.window), "laid out")
        compare(chart.shownW, frame.width - 8, "the plot keeps the width")
        verify(strip.y >= chart.y + chart.shownH, "the values below the chart: strip " + strip.y + " h " + strip.height + ", chart " + chart.y + " shownH " + chart.shownH + " h " + chart.height + ", frame " + frame.width + "x" + frame.height + ", grid " + frame.stripHeight)
        verify(strip.width > frame.width / 2, "a band across the frame: " + strip.width)
        const open = chart.shownH

        mouseClick(fold)
        verify(!strip.visible, "folded")
        // One plotted signal here: a band of one row, folded to the button's
        tryVerify(() => frame.stripHeight === fold.height + 8, 2000, "folded to the button")
        compare(chart.shownH, frame.height - 8 - frame.stripHeight, "the plot has the rest")
        verify(chart.shownH > open, "the plot has the height: " + chart.shownH + " against " + open)
        compare(chart.shownW, frame.width - 8)
        mouseClick(fold)
        tryVerify(() => strip.visible && chart.shownH === open, 2000, "the band is back")
        page.toggleMaximized()
        page.width = 900
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
        // lands on the chart. The press waits for that size and the layout
        // after it, and a miss says where it landed
        const win = frame.Window.window
        const size = [win.width, win.height]
        win.width = page.width
        win.height = page.height
        tryVerify(() => win.width >= page.width && win.height >= page.height, 5000,
                  "the window grown: " + win.width + "x" + win.height)
        verify(waitForPolish(win), "the grown window laid out")
        const x = frame.width / 2, y = frame.height / 2
        const at = frame.mapToItem(null, x, y)
        verify(x > 200 && at.x < win.width && at.y < win.height,
               "the press lands on the chart: " + frame.width + "x" + frame.height
               + " at " + at.x + "," + at.y + " in " + win.width + "x" + win.height)
        // Whether the drag went active tells a press that missed from a
        // drag that moved nothing
        const pan = find(frame, "chartPan")
        let panned = false
        const seen = () => { if (pan.active) panned = true }
        pan.activeChanged.connect(seen)
        mousePress(frame, x, y)
        for (let dx = 10; dx <= 200; dx += 10)
            mouseMove(frame, x - dx, y, 10)
        mouseRelease(frame, x - 200, y)
        pan.activeChanged.disconnect(seen)
        verify(page.viewT0 > 0, "dragged later: " + page.viewT0
               + (panned ? ", the drag went active" : ", the drag never went active"))
        verify(!page.hasCursor, "a drag places no cursor")
        mouseClick(frame, x, y)
        verify(page.cursorMs > page.viewT0 && page.cursorMs < page.viewT1,
               "a tap places the cursor in view: " + page.cursorMs)
        const first = find(page, "signalRows").model.find(s => !s.isText && page.plotted[s.index] === true)
        compare(page.readout[first.index].atMs, page.cursorMs,
                "the tap lands on a sample of the first signal plotted")
        compare(page.viewT1 - page.viewT0, 4000)
        mouseWheel(frame, x, y, 0, 120)
        verify(page.viewT1 - page.viewT0 < 4000, "zoomed in: " + (page.viewT1 - page.viewT0))
        mouseClick(find(page, "zoomAll"))
        compare(page.viewT1 - page.viewT0, 20000)
        // A sideways scroll doesn't zoom, and a notch in four parts, as a
        // high-resolution wheel or a touchpad sends it, zooms as one does
        page.setView(4000, 8000)
        mouseWheel(frame, x, y, 120, 0)
        compare(page.viewT0, 4000, "a sideways scroll leaves the view")
        compare(page.viewT1, 8000)
        for (let i = 0; i < 4; ++i)
            mouseWheel(frame, x, y, 0, 30)
        fuzzyCompare(page.viewT1 - page.viewT0, 4000 * 0.8, 1, "four quarters of a notch: one notch's zoom")
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

    // The traces are the chart's Shape: each trace's lines, and its bands'
    // fill and edges, at the layout's points in its row's color; the
    // page's marks are a Canvas over them
    function test_theTracesAreTheShapesUnderThePagesMarks() {
        const page = openFindings()
        const chart = find(page, "chart")
        // The samples come after the open: wait for a chart that has them
        tryVerify(() => chart.drawn && chart.drawn.traces.some(t => t.pieces.length > 0),
                  5000, "drawn with the samples")
        const l = chart.drawn
        const shape = find(chart, "chartTraces")
        const marks = find(chart, "chartMarks")
        verify(marks.visible, "the page has marks")
        const kids = chart.children
        verify(kids.indexOf(marks) > kids.indexOf(shape), "the marks over the traces")
        let points = 0
        for (let i = 0; i < l.traces.length; ++i) {
            const tr = l.traces[i]
            const set = chart.tracePaths[i]
            const lines = tr.pieces.filter(p => !p.band)
            const bands = tr.pieces.length - lines.length
            compare(set.line.pathElements[0].paths.length, lines.length, "trace " + i + "'s lines")
            compare(set.edge.pathElements[0].paths.length, 2 * bands, "trace " + i + "'s band edges")
            compare(set.fill.pathElements[0].paths.length, bands, "trace " + i + "'s band fills")
            if (lines.length > 0) {
                verify(Qt.colorEqual(set.line.strokeColor, Theme.seriesColor(tr.row)),
                       "trace " + i + " in its row's color")
                const drawn = set.line.pathElements[0].paths[0]
                compare(drawn.length, lines[0].points.length)
                compare(drawn[0].x, lines[0].points[0].x)
                compare(drawn[0].y, lines[0].points[0].y)
                points += drawn.length
            }
            // A trace bands or not by the chart's width when it draws
            if (bands > 0) {
                const band = tr.pieces.filter(p => p.band)[0]
                verify(Qt.colorEqual(set.edge.strokeColor, Theme.seriesColor(tr.row)),
                       "trace " + i + "'s band edges in its row's color")
                const edge = set.edge.pathElements[0].paths[0]
                compare(edge.length, band.top.length)
                compare(edge[0].x, band.top[0].x)
                compare(edge[0].y, band.top[0].y)
                points += edge.length
            }
        }
        verify(points > 0, "some trace drawn")
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
            signals: [], own: "", plugin: "", baseline: folder })
        return { salt: "", picked: "v1",
                 list: [entry("v1", "Daily driver", baseline), entry("v2", "Weekend car", "")] }
    }

    SignalSpy {
        id: vehiclesSpy
        signalName: "vehiclesEdited"
    }

    // A page that stores its vehicle edits as Settings would
    function createBookPage(baseline) {
        const page = createPage()
        page.vehiclesEdited.connect(doc => { page.vehicles = doc })
        vehiclesSpy.target = page
        vehiclesSpy.clear()
        page.vehicles = vehicleBook(baseline)
        return page
    }

    function openRow(page, row) {
        page.open(row)
        tryVerify(() => find(page, "recordingInfo").visible, 10000, "the recording opens")
        verify(waitForPolish(page.Window.window), "the viewer laid out")
    }

    // Aligned on the engine starts where havne-cli view lines them up, and
    // a clear word when either has none: pull-early's engine runs from its
    // first sample
    function test_theEngineStartsAlignAsHavneClisDo() {
        const page = createPage()
        page.recordings = [startLateRow, startEarlyRow, earlyRow, lateRow]
        openRow(page, startEarlyRow)
        const choice = find(page, "alignChoice")
        const status = find(page, "compareStatus")
        page.compareWith(startLateRow)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        choice.currentIndex = choice.indexOfValue("engine")
        verify(choice.currentIndex > 0, "the choice is offered")
        compare(choice.currentText, "Align engine starts")
        choice.activated(choice.currentIndex)
        compare(page.alignOn, "engine")
        verify(page.compareAligned)
        compare(page.compareOffsetMs, -7000)
        compare(page.viewT0, -7000, "from the compared one's start")
        compare(status.text, "Dashed: " + startLateRow.title
                + ", aligned on their engine starts, at 5.6 s here and 12.6 s there")

        page.compareWith(earlyRow)
        tryVerify(() => status.text.indexOf("isn't drawn") > 0, 10000, status.text)
        compare(status.text, earlyRow.title + " isn't drawn: it has no engine start"
                + " (RPM reaching 500 after being below 100)")
        verify(!page.compareAligned)
        compare(page.viewT0, 0, "the first alone")
        compare(page.series(400).length, 2, "its own traces alone")

        openRow(page, earlyRow)
        page.setAlignOn("engine")
        page.compareWith(startEarlyRow)
        tryVerify(() => status.text.indexOf("isn't drawn") > 0, 10000, status.text)
        verify(status.text.indexOf("this recording has no engine start") > 0, status.text)
        page.compareWith(lateRow)
        tryVerify(() => status.text.indexOf("neither") > 0, 10000, status.text)
        verify(!page.compareAligned)
    }

    // The picked vehicle's baseline: an open recording becomes it, its row
    // carries the badge, and another recording overlays it aligned on the
    // engine starts. Another vehicle picked has its own; none picked, none
    function test_aRecordingBecomesTheVehiclesBaseline() {
        const page = createBookPage("")
        page.recordings = [startLateRow, startEarlyRow]
        openRow(page, startEarlyRow)
        const use = find(page, "useAsBaseline")
        const versus = find(page, "compareBaseline")
        const mine = find(page, "isBaseline")
        verify(use.visible)
        compare(use.text, "Use as Daily driver's baseline")
        verify(!versus.visible, "no baseline yet")
        verify(!mine.visible)
        mouseClick(use)
        compare(vehiclesSpy.count, 1)
        compare(VehicleBook.picked(page.vehicles).baseline, startEarlyRow.folder)
        compare(VehicleBook.find(page.vehicles, "v2").baseline, "", "only the picked vehicle's")
        verify(!use.visible)
        verify(mine.visible)
        compare(mine.text, "Daily driver's baseline")
        verify(!versus.visible, "never compared with itself")

        // The list marks its row
        find(page, "backToList").clicked()
        page.recordings = [startLateRow, startEarlyRow]
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(1) !== null, 2000, "the rows have delegates")
        verify(find(list.itemAtIndex(1), "baselineBadge").visible)
        compare(find(list.itemAtIndex(1), "baselineBadge").text, "Baseline")
        verify(!find(list.itemAtIndex(0), "baselineBadge").visible)

        // Another recording overlays it, aligned on the engine starts
        openRow(page, startLateRow)
        verify(versus.visible)
        verify(use.visible, "this one can take its place")
        mouseClick(versus)
        tryVerify(() => page.compareAligned, 10000, "the baseline opens")
        compare(page.alignOn, "engine")
        compare(find(page, "alignChoice").currentValue, "engine")
        compare(page.compareTitle, "start-early", "its folder's name")
        compare(page.compareOffsetMs, 7000, "12.6 s here, 5.6 s there")

        page.vehicles = VehicleBook.pick(page.vehicles, "v2")
        verify(!versus.visible, "the other vehicle has none")
        compare(use.text, "Use as Weekend car's baseline")
        page.vehicles = VehicleBook.pick(page.vehicles, "")
        verify(!find(page, "baselineRow").visible, "no vehicle picked")
        compare(vehiclesSpy.count, 1)
    }

    // A baseline whose folder is gone says so on the list and in the
    // viewer, offers no overlay, and Clear it clears it
    function test_aBaselineWhoseFolderIsGoneSaysSo() {
        const page = createBookPage(Harness.recordingFixtures + "/no-such-recording")
        page.recordings = [startEarlyRow]
        const gone = find(page, "baselineGone")
        verify(gone.visible)
        compare(gone.text, "Daily driver's baseline recording is gone: its folder was moved or deleted.")
        openRow(page, startEarlyRow)
        verify(gone.visible, "in the viewer too")
        verify(!find(page, "compareBaseline").visible)
        verify(find(page, "useAsBaseline").visible)
        find(page, "clearBaseline").clicked()
        compare(vehiclesSpy.count, 1)
        compare(VehicleBook.picked(page.vehicles).baseline, "")
        verify(!gone.visible)
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

        // What reached the button tells a press that went elsewhere from
        // one whose grab was taken
        const back = find(page, "backToList")
        const seen = []
        // (pressed is also a property, so its signal is heard as a change)
        const heard = ["pressedChanged", "released", "clicked", "canceled"].map(
                    s => [s, () => seen.push(s === "pressedChanged" ? "pressed " + back.pressed : s)])
        for (const [s, h] of heard)
            back[s].connect(h)
        const at = back.mapToItem(null, back.width / 2, back.height / 2)
        mouseClick(back)
        for (const [s, h] of heard)
            back[s].disconnect(h)
        verify(!page.viewing, "the click on All recordings went back to the list: "
               + (seen.length ? seen.join(", ") : "nothing from the button")
               + ", at " + at.x + "," + at.y + ", the popup "
               + (bar.options.visible ? "visible" : "hidden"))
        page.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens again")
        compare(find(page, "signalRows").count, all, "back to the list cleared the filter: text '"
                + bar.filter.text + "', plotted only " + bar.filter.plottedOnly
                + ", groups [" + bar.filter.groups + "]")
    }

    function test_aRefusedFolderSaysWhyAndBackGoesToTheList() {
        const page = createPage()
        page.open({ title: "Gone", folder: Harness.recordingFixtures + "/no-such-recording" })
        const error = find(page, "openError")
        tryVerify(() => error.visible, 10000, "refused")
        verify(error.text.indexOf("signals.csv") >= 0, error.text)
        verify(waitForPolish(page.Window.window), "the refusal laid out")
        mouseClick(find(page, "backToList"))
        verify(!page.viewing)
        verify(find(page, "recordingList").visible)
    }
}
