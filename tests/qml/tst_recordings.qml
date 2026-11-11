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

    // Opens pull-early and waits for its viewer
    function openEarlyViewer() {
        const page = createPage()
        page.open(earlyRow)
        tryVerify(() => find(page, "exportButton").visible, 10000, "the viewer opens")
        return page
    }

    // Opens pull-early with pull-late listed beside it. A recording that
    // opens moves the page's items at the layout's next polish, a frame
    // later; a case whose checks all pass at once would otherwise press
    // where an item was before it opened
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
        compare(find(list.itemAtIndex(0), "recordingDetail").text, "mock://recordings-page",
                "a mock address replays as mock: the address alone")
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
        verify(!find(page, "replayView").visible, "no Replay of the view")
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
        verify(!find(page, "importStart").enabled, "a time with fractions has no unit yet")
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

    // A time named so, of whole numbers with no unit, reads as ms: Import is ready,
    // the unit shows chosen and changes with a tap
    function test_aTimeWithoutAUnitReadsAsMilliseconds() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-plain.csv",
            "time,rpm\n0,800\n1,900\n2,950\n"))
        const preview = guessed(page)
        verify(!find(preview, "guessWaiting").visible)
        verify(find(preview, "guessTimeUnit_ms").visible)
        verify(find(preview, "guessTimeUnit_ms").selected, "ms, as the guess read it")
        verify(find(page, "importStart").enabled)
        compare(JSON.parse(preview.mapText()).time.unit, "ms")
        find(preview, "guessTimeUnit_us").clicked()
        compare(JSON.parse(preview.mapText()).time.unit, "us")
        verify(find(page, "importStart").enabled)
    }

    // A time with fractions may be seconds: Import waits for the choice
    // and says so
    function test_aTimeWithFractionsWaitsForAUnit() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-fraction.csv",
            "time,rpm
0.0,800
0.5,900
1.0,950
"))
        const preview = guessed(page)
        verify(find(preview, "guessWaiting").visible)
        compare(find(preview, "guessWaiting").text, "Choose the time column's unit to import")
        verify(!find(page, "importStart").enabled)
        find(preview, "guessTimeUnit_s").clicked()
        verify(!find(preview, "guessWaiting").visible)
        verify(find(page, "importStart").enabled)
        compare(JSON.parse(preview.mapText()).time.unit, "s")
    }

    // Save as map is enabled exactly when Import would be: a time no unit
    // can fix (dates with no zone) leaves both waiting
    function test_saveWaitsForATimeImportCanRead() {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("guess-save-unit.csv",
            "time,rpm\n0,800\n1,900\n2,950\n"))
        let preview = guessed(page)
        verify(preview.ready)
        verify(find(preview, "saveMapButton").enabled, "ms, as the guess read it")
        page.chooseImportFile(Harness.writeScratchFile("guess-save-zone.csv",
            "time,rpm\n2026-11-05T22:03:22,800\n2026-11-05T22:03:23,900\n"))
        preview = guessed(page)
        verify(!preview.ready)
        verify(preview.included > 0)
        verify(!find(preview, "saveMapButton").enabled, "no zone to read it by")
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
    // A worker thread that can't start leaves the panel open and the note
    // in the danger color, for the guess and for the import alike
    function test_anImportThatCouldntStartSaysSo() {
        const page = createPage()
        const note = find(page, "importNote")
        Harness.failImportThreads(Bridge.recordingImport, true)
        try {
            page.chooseImportFile(Harness.writeScratchFile("nostart.csv",
                "time (s),rpm\n0,800\n1,900\n"))
            verify(page.importFailed)
            verify(!Bridge.recordingImport.busy)
            verify(note.visible)
            verify(note.text.indexOf("The file couldn't be read") === 0, note.text)
            verify(Qt.colorEqual(note.color, Theme.danger))

            page.chooseImportFile(Harness.writeScratchFile("nostart.log",
                "(1.000000) can0 7E8#01\n"))
            verify(!note.visible, "a new file, no old words")
            verify(find(page, "importStart").enabled)
            find(page, "importStart").clicked()
            compare(note.text, "Not imported: the import couldn't start")
            verify(Qt.colorEqual(note.color, Theme.danger))
            verify(find(page, "importPanel").visible, "the panel stays")
        } finally {
            Harness.failImportThreads(Bridge.recordingImport, false)
        }
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the importer is still usable")
        verify(!page.importFailed)
    }

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

    // Replay off says what to do first: stop the relay, or disconnect;
    // while a session starts or ends it's plain Replay, off
    function test_replaySaysWhatStopsIt() {
        const page = createPage()
        page.recordings = [findingsRow]
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        const button = find(list.itemAtIndex(0), "replayButton")
        verify(button.enabled)
        compare(button.text, "Replay")
        const name = button.Accessible.name
        verify(name.indexOf("Replay ") === 0 && name.indexOf("to replay") < 0, "its own name: " + name)

        compare(Relay.start("elm327://bt:00:11:22:33:44:55", 0), "", "the relay starts")
        tryVerify(() => !button.enabled, 2000, "off while relaying")
        compare(button.text, "Stop the relay to replay")
        compare(button.Accessible.name, name + ": Stop the relay to replay", "spoken with its reason")
        Relay.stop()
        tryVerify(() => button.enabled, 2000, "on again")

        Bridge.connectSessionAsync("mock://replay-words")
        verify(Bridge.connecting, "the connect is under way")
        verify(!button.enabled)
        compare(button.text, "Replay", "nothing to disconnect yet")
        tryVerify(() => Bridge.connected, 10000, "the mock connects")
        compare(button.text, "Disconnect to replay")
        compare(button.Accessible.name, name + ": Disconnect to replay", "spoken with its reason")
        Bridge.disconnectSession()
        verify(Bridge.disconnecting, "the teardown is under way")
        verify(!button.enabled)
        compare(button.text, "Replay", "already disconnecting")
        compare(button.Accessible.name, name, "and nothing to add")
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        verify(button.enabled, "on again")
        compare(button.text, "Replay")
        compare(button.Accessible.name, name)
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
        compare(button.text, "Replay", "one tap replays the part in view")
        compare(button.Accessible.name, "Replay the part in view as a session")

        // Off while a session runs, as the list's Replay is
        Bridge.connectSessionAsync("mock://replay-view")
        tryVerify(() => Bridge.connected, 10000, "the mock connects")
        verify(!button.enabled, "off while connected")
        compare(button.text, "Disconnect to replay", "and it says why")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        verify(button.enabled, "on again")
    }

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
    // The directory of a scratch file, to pick as a folder
    function scratchFolder(name) {
        const url = Harness.writeScratchFile(name + "-marker", "").toString()
        return url.substring(0, url.lastIndexOf("/"))
    }

    function test_theExportPanelTakesTheChartsPlaceAndClosesAgain() {
        const page = openEarlyViewer()
        const chart = find(page, "chartFrame")
        verify(chart.visible, "the chart first")
        const panel = openExport(page)
        verify(!chart.visible, "the panel takes its place")
        verify(!find(page, "chartSplitter").visible)
        find(panel, "exportClose").clicked()
        tryVerify(() => chart.visible, 2000, "the chart comes back")
        verify(!panel.visible)
    }

    function test_theExportSizesEachIntervalAndTheCustomOne() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        restoreExportChoices(panel)
        sizesIn(panel)
        const sweep = findChild(find(panel, "exportRows_sweep"), "intervalSize")
        verify(/^\d[\d,.'\u2019\s]* rows, [\d.,]+ (B|KB|MB)$/.test(sweep.text), sweep.text) // any locale's
        const grid = findChild(find(panel, "exportRows_250"), "intervalSize")
        verify(sweep.text !== grid.text, sweep.text + " and " + grid.text)

        // A custom interval is asked for once typed, and a bad one is said
        find(panel, "exportRows_custom").clicked()
        const field = find(panel, "exportCustomMs")
        verify(field.visible)
        panel.customText = "40"
        tryVerify(() => panel.sizes.length === 4, 10000, "the custom interval sized")
        compare(panel.chosenMs, 40)
        verify(find(panel, "exportGo").enabled)
        panel.customText = "12x"
        compare(panel.customMs, -1)
        verify(!find(panel, "exportGo").enabled, "not with a bad interval")
        compare(find(panel, "exportCustomProblem").text, "ms, 1 to 86400000")
        restoreExportChoices(panel)
    }

    // A recording sampled every 20 ms: each interval thins it, so each line
    // is a size and none carries the note
    function test_anIntervalThatThinsTheRecordingIsSizedNotNoted() {
        const name = "export-fast"
        const folder = Harness.writeFramesRecording(name, 1, 100000)
        verify(folder !== "", "the folder is made")
        let header = "ecuio session recording, format 2 (ADR-0049)
"
                     + "started  2026-11-03 10:00:00.000 (local time)
"
                     + "utc  2026-11-03T16:00:00.000Z
core  0.3.6
"
                     + "uri  socketcan://vcan0?profile=raw
profile  raw
"
                     + "signal  Signal 1,number,V,0,0
"
        let rows = "host_us,signal,value,unit
"
        for (let t = 1; t <= 100; ++t)
            rows += (t * 20000) + ",Signal 1," + t + ",V
"
        header += "
10:00:02.000  +2.000  end  havne-cli: seconds: 2 frames, 100 values, "
                  + "0 records dropped, 0 notes dropped
"
        Harness.writeScratchFile(name + "/events.log", header)
        Harness.writeScratchFile(name + "/signals.csv", rows)
        const page = createPage()
        page.open({ title: "Fast", folder: folder })
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        const panel = openExport(page)
        restoreExportChoices(panel)
        sizesIn(panel)
        for (const name of ["exportRows_sweep", "exportRows_100", "exportRows_250"]) {
            const text = findChild(find(panel, name), "intervalSize").text
            verify(/ rows?, /.test(text), name + ": " + text)
        }
        restoreExportChoices(panel)
    }

    // An interval the recording is too slow for gives no fewer rows than every sweep, and
    // its line says so in place of a size that looks like a saving
    function test_anIntervalSlowerThanTheRecordingSaysItGivesNoFewerRows() {
        const folder = Harness.importCsvFixture("logger-a", "export-slow")
        verify(folder !== "", "the fixture imports")
        const page = createPage()
        page.open({ title: "Slow", folder: folder })
        tryVerify(() => find(page, "signalRows").visible, 10000, "it opens")
        const panel = openExport(page)
        restoreExportChoices(panel)
        sizesIn(panel)
        const textOf = name => findChild(find(panel, name), "intervalSize").text
        verify(/ rows?, /.test(textOf("exportRows_sweep")), textOf("exportRows_sweep"))
        compare(textOf("exportRows_100"), "no fewer rows than every sweep", "100 ms is faster than 4 Hz")
        compare(textOf("exportRows_250"), "no fewer rows than every sweep", "250 ms is the recording's own pace")
        verify(find(panel, "exportRows_100").enabled && find(panel, "exportRows_250").enabled,
               "still choosable")
        find(panel, "exportRows_custom").clicked()
        panel.customText = "1000"
        tryVerify(() => panel.sizes.length === 4, 10000, "the custom interval sized")
        verify(/ rows?, /.test(textOf("exportRows_custom")), "a slower interval still saves: "
               + textOf("exportRows_custom"))
        restoreExportChoices(panel)
    }

    // A row reads in one locale: a comma-decimal one groups the count
    // with a point and writes the size with a comma
    function test_theExportSizeIsInTheRowsLocale() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        restoreExportChoices(panel)
        panel.numberLocale = Qt.locale("de_DE")
        compare(panel.bytesText(999), "999 B")
        compare(panel.bytesText(12345), "12,3 KB")
        compare(panel.bytesText(1234567), "1,2 MB")
        compare(panel.bytesText(2500000000), "2,5 GB")
        sizesIn(panel)
        const sweep = findChild(find(panel, "exportRows_sweep"), "intervalSize")
        verify(/^\d[\d.]* rows?, (\d+ B|\d+,\d (KB|MB))$/.test(sweep.text), sweep.text)
        panel.sizes = [{ rowMs: 0, rows: 18402, bytes: 1234567 }]
        compare(panel.sizeOf(0), "18.402 rows, 1,2 MB", "the count grouped as the size is written")
        panel.numberLocale = Qt.locale()
    }

    function test_theRowIntervalIsRememberedAndTheRestStartsOver() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        restoreExportChoices(panel)
        find(panel, "exportRows_100").clicked()
        find(panel, "exportLayout_long").clicked()
        find(panel, "exportColumns_plotted")
        panel.columnChoice = "plotted"
        panel.raw = true
        find(panel, "exportClose").clicked()
        const again = openExport(page)
        compare(again.format, "csv")
        verify(!again.longLayout, "wide each time")
        compare(again.columnChoice, "own")
        verify(!again.raw, "raw is off each time")
        // Kept between runs: a page of its own reads it back from the store
        page.destroy()
        wait(0)
        const next = openEarlyViewer()
        const nextPanel = openExport(next)
        compare(nextPanel.rowChoice, "100", "the interval is kept")
        restoreExportChoices(nextPanel)
    }

    function test_theExportWritesTheChosenWideCsvAndSaysSo() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        restoreExportChoices(panel)
        sizesIn(panel)
        find(panel, "exportRows_250").clicked()
        const url = "file:///" + scratchFolder("wide").replace(/^file:\/+/, "") + "/early-wide.csv"
        verify(panel.exportTo(url))
        const note = find(panel, "exportNote")
        tryVerify(() => note.text.indexOf("Saved: ") === 0, 10000, "the export ends saved")
        verify(/ rows of \d+ signals?, left out: 1 text value$/.test(note.text), note.text)
        const text = Harness.readScratchFile(url)
        verify(text.indexOf("Time (ms),") === 0, text.substring(0, 40))
        const seconds = text.split("\r\n")[2].split(",")[0]
        compare(Number(seconds) % 250, 0, "on the grid: " + seconds)
        restoreExportChoices(panel)
    }

    function test_thePlottedColumnsAreOffered() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        restoreExportChoices(panel)
        verify(page.plottedNames.length > 0, "some plotted")
        const plotted = find(panel, "exportColumns_plotted")
        compare(plotted.text, "The " + page.plottedNames.length + " plotted")
        // One plotted, so its CSV differs from the log's own
        const plottedAtFirst = page.plottedNames.slice()
        verify(plottedAtFirst.length >= 2, "two or more plotted to start")
        const first = plottedAtFirst[0]
        const one = {}
        for (const s of panel.view.signalList)
            one[s.index] = s.name === first
        page.plotted = one
        tryVerify(() => page.plottedNames.length === 1, 2000, "one plotted")
        sizesIn(panel)
        const own = panel.sizes.find(s => s.rowMs === 0).bytes
        panel.columnChoice = "plotted"
        verify(panel.usePlotted)
        compare(panel.columnNames(), [first])
        // The sizes are asked again for the columns, the old ones gone at once
        compare(panel.sizes.length, 0, "no size for the other columns")
        sizesIn(panel)
        const oneBytes = panel.sizes.find(s => s.rowMs === 0).bytes
        verify(oneBytes !== own, "the plotted column's own size: " + own + " then " + oneBytes)
        // Another plotted while they're chosen: asked again
        const two = {}
        for (const s of panel.view.signalList)
            two[s.index] = s.name === first || s.name === plottedAtFirst[1]
        page.plotted = two
        tryVerify(() => page.plottedNames.length === 2, 2000, "two plotted")
        compare(panel.sizes.length, 0, "no size for the old plotted set")
        sizesIn(panel)
        verify(panel.sizes.find(s => s.rowMs === 0).bytes !== oneBytes, "two columns' size")
        // Nothing plotted, nothing to offer: the log's own again
        const none = {}
        for (const s of panel.view.signalList)
            none[s.index] = false
        page.plotted = none
        tryVerify(() => !panel.plottedOffered, 2000)
        verify(!panel.usePlotted, "back to the log's own")
        compare(panel.columnNames(), [])
        // Other units, other sizes asked for, with the log's own columns
        sizesIn(panel)
        Bridge.unitSystem = 1 - Bridge.unitSystem
        compare(panel.sizes.length, 0, "no size in the other units")
        sizesIn(panel)
        restoreExportChoices(panel)
    }

    // A sizing for the old columns that ends before the 300 ms timer asks
    // again leaves no sizes beside the new choice (the first sizing is held
    // 120 ms at core's first ask; the switch comes while it is held)
    function test_aSizingForTheOldColumnsLeavesNoSizes() {
        const page = openEarlyViewer()
        verify(page.plottedNames.length > 0, "some plotted")
        const exporter = find(page, "exporter")
        Harness.holdExports(exporter, 120)
        try {
            const panel = openExport(page)
            restoreExportChoices(panel)
            tryVerify(() => Harness.heldExports(exporter) >= 1, 10000, "the sizing is held")
            panel.columnChoice = "plotted"
            verify(panel.usePlotted)
            wait(200)   // past the held sizing's end, short of the timer's 300 ms
            compare(panel.sizes.length, 0, "the old columns' sizes didn't arrive")
            Harness.holdExports(exporter, 0)
            sizesIn(panel)
            restoreExportChoices(panel)
        } finally {
            Harness.holdExports(exporter, 0)
        }
    }

    function test_aRecordingOfFramesAloneExportsNoCsv() {
        const page = createPage()
        const folder = Harness.writeFramesRecording("export-frames-only", 50, 10000)
        verify(folder !== "")
        page.open({ title: "Frames", uri: "replay://x?profile=raw", source: "", folder: folder, profile: "raw" })
        tryVerify(() => find(page, "exportButton").visible, 10000, "the viewer opens")
        const panel = openExport(page)
        compare(panel.format, "pcapng", "CSV isn't offered")
        verify(!find(panel, "exportFormat_csv").enabled)
        verify(find(panel, "exportNoValues").visible)
        verify(find(panel, "exportGo").enabled)
    }

    function test_aRecordingOfValuesAloneHasNoPcapng() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        verify(!find(panel, "exportFormat_pcapng").visible, "pull-early has no frames.csv")
        verify(find(panel, "exportFormat_folder").visible)
    }

    function test_aDecodedViewSaysTheCsvIsAsRecorded() {
        const page = openFindings()
        page.decodeWith("obd2")
        tryVerify(() => page.viewing && find(page, "recordingInfo").visible
                        && find(page, "recordingInfo").text.indexOf("decoded") >= 0, 10000, "decoded")
        const panel = openExport(page)
        verify(find(panel, "exportDecodedNote").visible)
        verify(!panel.plottedOffered, "a decode's names aren't the recording's columns")
        panel.format = "pcapng"
        verify(!find(panel, "exportDecodedNote").visible, "the CSV's note alone")
    }

    function test_rawIsDevelopersAndWarnsAboutTheVin() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        const box = find(panel, "exportRaw")
        const was = Theme.developerMode
        Theme.developerMode = false
        verify(!box.visible, "hidden outside Developer mode")
        panel.raw = true
        verify(!panel.rawOn, "and no effect")
        Theme.developerMode = true
        verify(box.visible)
        verify(panel.rawOn)
        const warning = find(panel, "exportRawWarning")
        verify(warning.visible)
        verify(warning.text.indexOf("VIN") >= 0, warning.text)
        panel.raw = false
        verify(!warning.visible)
        Theme.developerMode = was
    }

    function test_aFolderCopyAndAFailedExportSaySo() {
        const page = openFindings()
        const panel = openExport(page)
        panel.format = "folder"
        compare(find(panel, "exportGo").text, "Choose a folder...")
        verify(panel.exportTo(scratchFolder("copy")))
        const note = find(panel, "exportNote")
        tryVerify(() => note.text.indexOf("Saved as ") === 0, 10000, "the copy ends saved")
        verify(/^Saved as recording(-\d+)?: \d+ files?, /.test(note.text), note.text)
        panel.format = "pcapng"
        verify(panel.exportTo("file:///" + scratchFolder("bad").replace(/^file:\/+/, "") + "/no-such/out.pcapng"))
        tryVerify(() => note.text.indexOf("Not exported: ") === 0, 10000, "the export ends refused")
        compare(note.color, Theme.danger)
        // A start the exporter refuses says so too
        verify(!panel.exportTo(""))
        compare(note.text, "Not exported: the export couldn't start")
        compare(note.color, Theme.danger)
    }

    // Cancel takes Save as...'s place while an export runs (held at core's
    // first ask for a minute): it ends well inside that, says so in the
    // note's own color, writes nothing, and the next export runs whole
    function test_aCanceledExportWritesNothingAndSaysSo() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        restoreExportChoices(panel)
        sizesIn(panel)
        const exporter = find(panel, "exporter")
        const go = find(panel, "exportGo")
        const cancel = find(panel, "exportCancel")
        const note = find(panel, "exportNote")
        verify(go.visible && !cancel.visible, "Save as... while none runs")
        const url = "file:///" + scratchFolder("canceled").replace(/^file:\/+/, "") + "/canceled.csv"
        Harness.holdExports(exporter, 60000)
        try {
            verify(panel.exportTo(url))
            tryVerify(() => Harness.heldExports(exporter) >= 1, 10000, "held at core's first ask")
            verify(cancel.visible && !go.visible, "Cancel in its place")
            cancel.clicked()
            tryVerify(() => note.text.indexOf("Export canceled") === 0, 20000, note.text)
        } finally {
            Harness.holdExports(exporter, 0)
        }
        compare(note.color, Theme.textDim, "not a failure")
        verify(go.visible && !cancel.visible)
        compare(Harness.readScratchFile(url), "", "nothing written")
        verify(panel.exportTo(url))
        tryVerify(() => note.text.indexOf("Saved: ") === 0, 10000, note.text)
        restoreExportChoices(panel)
    }

    // A finished export for a recording the viewer has left says nothing
    // here: Back and another recording opened before it ended
    function test_aLateExportForAnotherRecordingLeavesTheNote() {
        const page = openEarlyViewer()
        const panel = openExport(page)
        const note = find(panel, "exportNote")
        const exporter = find(panel, "exporter")
        const result = folder => ({ ok: true, folder: folder, name: "", summary: "1 row",
                                    line: "Exported: a test's line", words: "" })
        const before = note.text
        exporter.finished(result(panel.view.folder + "-another"))
        compare(note.text, before, "another recording's export")
        exporter.finished(result(panel.view.folder))
        compare(note.text, "Saved: 1 row", "this one's")
    }

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
        tryVerify(() => /^\d+ frames? in view$/.test(words.text), 10000, "the frames in view are counted")
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
        tryVerify(() => words.text === "The first 10,000 frames from 0.0 s: zoom in for the rest",
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

    // On a phone the strings that were cut at 384 dp are short forms, the
    // way back is a quiet arrow, and the name is the viewer's title
    function test_aPhoneGetsTheShortStrings() {
        const page = openFindings()
        const title = find(page, "openedTitle")
        const back = find(page, "backToList")
        const header = find(page, "recordingsHeader")
        compare(title.text, findingsRow.title)
        compare(title.elide, Text.ElideMiddle, "the folder's date at the end stays")
        compare(back.text, "All recordings")
        verify(header.visible, "the desktop keeps the page's heading")
        Theme.compact = true
        try {
            compare(find(page, "cursorTime").text, "Tap for values")
            verify(title.visible, "the name in the title row")
            compare(back.text, "←")
            verify(back.flat, "quiet")
            compare(back.Accessible.name, "All recordings")
            verify(!header.visible, "the tab names the page")
            page.setCursor(5000)
            compare(find(page, "cursorTime").text, "At 5.0 s", "a cursor reads the same")
        } finally {
            Theme.compact = false
        }
        verify(header.visible && back.text === "All recordings", "and back")
    }

    // The desktop's one order: the chart, its zoom row, the switch and the
    // filter, the rows, then About this recording, Replay and Save report in
    // the title row. A phone upright pins the chart and gives About a view
    // of its own: the zoom row, the switch and the filter head the rows,
    // and About's view holds the recording's lines with Replay and Save
    // report among them
    function test_theViewersOrderIsChartRowsThenAbout_data() {
        return [{ tag: "desktop", w: 1280, h: 720, phone: false },
                { tag: "phone", w: 384, h: 832, phone: true }]
    }
    function test_theViewersOrderIsChartRowsThenAbout(data) {
        const page = openFindings()
        Theme.compact = data.phone
        try {
            page.width = data.w
            page.height = data.h
            verify(waitForPolish(page.Window.window), "laid out")
            const yOf = name => {
                const item = find(page, name)
                verify(item.visible, name + " shows")
                return item.mapToItem(page, 0, 0).y
            }
            const middleOf = name => {
                const item = find(page, name)
                return item.mapToItem(page, 0, item.height / 2).y
            }
            const inOrder = order => {
                const ys = order.map(yOf)
                for (let i = 1; i < order.length; ++i)
                    verify(ys[i] > ys[i - 1],
                           order[i] + " (" + ys[i] + ") under " + order[i - 1] + " (" + ys[i - 1] + ")")
            }
            if (data.phone) {
                verify(page.pinnedViewer, "the phone's viewer is pinned")
                inOrder(["openedTitle", "chartFrame", "chartSplitter", "zoomAll", "detailView_values",
                         "recordingFilter"])
                const rows = find(page, "signalRows")
                verify(rows.itemAtIndex(0).mapToItem(page, 0, 0).y > yOf("recordingFilter"),
                       "the first row under the filter")
                verify(!find(page, "aboutHeading").visible, "the switch names About, no heading does")
                verify(!find(page, "recordingInfo").visible, "About waits for its view")
                find(page, "detailView_about").clicked()
                verify(waitForPolish(page.Window.window), "About laid out")
                verify(!rows.visible, "the rows give way to About")
                const findings = find(page, "findingList")
                tryVerify(() => findings.contentY <= findings.originY + 0.5, 2000, "About's list at its start")
                inOrder(["chartSplitter", "zoomAll", "detailView_about", "recordingInfo", "decodeChoice",
                         "compareChoice", "checkSummary"])
                const save = yOf("saveReport")
                verify(save > yOf("recordingInfo") && save < yOf("decodeChoice"), "Save report in About")
                compare(yOf("replayView"), save, "Replay beside it")
            } else {
                inOrder(["openedTitle", "chartFrame", "chartSplitter", "zoomAll", "recordingFilter",
                         "signalRows", "aboutHeading", "recordingInfo", "decodeChoice", "compareChoice",
                         "checkSummary"])
                const rows = find(page, "signalRows")
                compare(rows.itemAtIndex(0).mapToItem(rows, 0, 0).y, 0, "no head over the desktop's rows")
                verify(rows.contentHeight <= rows.height + 0.5, "the rows hold their list: "
                       + rows.contentHeight + " in " + rows.height)
                const save = yOf("saveReport")
                const replay = yOf("replayView")
                verify(save < yOf("chartFrame") && replay < yOf("chartFrame"), "both in the title row")
                const sw = find(page, "detailView_values")
                verify(sw.visible && Math.abs(middleOf("detailView_values") - middleOf("recordingFilter")) < 3
                       && sw.mapToItem(page, 0, 0).x < find(page, "recordingFilter").mapToItem(page, 0, 0).x,
                       "the switch at the filter's left")
            }
        } finally {
            Theme.compact = false
            page.width = 900
            page.height = 900
        }
    }

    // On a page taller than the viewer the room left over goes under it:
    // the zoom row stays right under the splitter, maximized too
    function test_theZoomRowStaysUnderTheChart() {
        const page = openFindings()
        try {
            page.height = 2000
            verify(waitForPolish(page.Window.window), "laid out")
            const splitter = find(page, "chartSplitter")
            const zoom = find(page, "zoomAll")
            const gap = zoom.mapToItem(page, 0, 0).y - splitter.mapToItem(page, 0, splitter.height).y
            verify(gap >= 0 && gap <= 16, "the zoom row " + gap + " px under the splitter")
            page.toggleMaximized()
            verify(waitForPolish(page.Window.window), "maximized")
            const chart = find(page, "chartFrame")
            const under = zoom.mapToItem(page, 0, 0).y - chart.mapToItem(page, 0, chart.height).y
            verify(under >= 0 && under <= 16, "maximized, the zoom row " + under + " px under the chart")
            page.toggleMaximized()
        } finally {
            page.height = 900
        }
    }

    // The chart's height until the splitter sets one: 45 % of the page on
    // the desktop, 40 % on a phone, never under what three panes, their
    // seams and the time axis need there
    function test_theChartTakesAShareOfThePage_data() {
        return [{ tag: "desktop", w: 1280, h: 720, phone: false, want: 0.45 * 720 },
                { tag: "phone", w: 384, h: 832, phone: true, want: 0.4 * 832 },
                { tag: "short phone", w: 384, h: 500, phone: true, want: -1 }]
    }
    function test_theChartTakesAShareOfThePage(data) {
        const page = openFindings()
        const kept = page.chartHeight
        page.chartHeight = -1
        Theme.compact = data.phone
        try {
            page.width = data.w
            page.height = data.h
            const chart = find(page, "chartFrame")
            const want = data.want > 0 ? data.want : page.phoneChartFloor
            if (data.want < 0) {
                const room = want - 8 - ChartLayout.top - ChartLayout.axisHCursor - ChartLayout.bottomPad
                compare(ChartLayout.panesThatFit(room, ChartLayout.paneMinH), 3, "three panes fit the floor")
                compare(ChartLayout.panesThatFit(room - 1, ChartLayout.paneMinH), 2, "and not a pixel less")
            }
            tryVerify(() => Math.abs(chart.height - want) < 1, 2000, "the chart is " + want)
            verify(Math.abs(chart.height - want) < 1, "the chart is " + chart.height)
        } finally {
            Theme.compact = false
            page.chartHeight = kept
            page.width = 900
            page.height = 900
        }
    }

    // The desktop's F with Frames chosen gives the table the page, the
    // switch leading the zoom row; with Values, the chart
    function test_maximizedWithFramesTheTableHasThePage() {
        const page = openFindings()
        const chart = find(page, "chartFrame")
        const table = find(page, "viewerFrameTable")
        const frames = find(page, "detailView_frames")
        mouseClick(frames)
        verify(page.framesShown, "the Frames view")
        tryVerify(() => table.visible, 2000, "the table")
        page.toggleMaximized()
        try {
            verify(waitForPolish(page.Window.window), "laid out")
            verify(!chart.visible, "no chart")
            verify(table.visible && table.height > page.height * 0.6,
                   "the table has the page: " + table.height + " of " + page.height)
            verify(frames.visible && frames.mapToItem(page, 0, 0).y < table.mapToItem(page, 0, 0).y,
                   "the switch above it")
            verify(!find(page, "signalRows").visible && !find(page, "aboutHeading").visible,
                   "the rest gives way")
            mouseClick(find(page, "detailView_values"))
            tryVerify(() => chart.visible && !table.visible, 2000, "Values: the chart")
            verify(waitForPolish(page.Window.window), "laid out")
            verify(chart.height > page.height * 0.6, "the chart has the page: " + chart.height)
        } finally {
            if (page.chartMaximized)
                page.toggleMaximized()
            page.detailView = "values"
        }
    }

    // The Export panel opens under the title row, so the page goes to its
    // top, wherever it was scrolled; on a phone upright the pinned viewer
    // gives way to the panel and comes back
    function test_theExportPanelOpensAtThePagesTop() {
        const page = openFindings()
        try {
            page.width = 384
            page.height = 600
            verify(waitForPolish(page.Window.window), "laid out")
            const scroll = find(page, "recordingsScroll")
            verify(!page.pinnedViewer, "a desktop's page scrolls as a whole")
            verify(scroll.contentHeight > scroll.height, "the page scrolls")
            scroll.contentY = scroll.contentHeight - scroll.height
            page.openExport()
            compare(scroll.contentY, 0, "at the top")
            const panel = find(page, "exportPanel")
            verify(panel.visible, "the panel")
            verify(panel.mapToItem(scroll, 0, 0).y < scroll.height / 2, "on the first screen")
            page.closeExport()

            Theme.compact = true
            verify(waitForPolish(page.Window.window), "a phone upright")
            verify(page.pinnedViewer, "upright the viewer is pinned")
            verify(!scroll.interactive, "the page itself stays put")
            page.openExport()
            verify(!page.pinnedViewer, "the panel takes the page")
            compare(scroll.contentY, 0, "at the top")
            verify(panel.visible && panel.mapToItem(scroll, 0, 0).y < scroll.height / 2,
                   "the panel on the first screen")
            verify(waitForPolish(page.Window.window), "the panel laid out")
            verify(scroll.contentHeight > scroll.height, "the panel scrolls the page")
            scroll.contentY = scroll.contentHeight - scroll.height
            page.closeExport()
            verify(page.pinnedViewer, "pinned again")
            compare(scroll.contentY, 0, "the pinned page from its top")
        } finally {
            Theme.compact = false
            page.width = 900
            page.height = 900
        }
    }

    // On a phone every button, box, choice and field the page shows, its
    // list rows and the viewer's signal rows are a touch's size (48), and
    // nothing runs past the page's right edge; the desktop keeps its own sizes
    // The frame table's rows and the By ID list's are data rows, as dense
    // as Live's signal rows; the UI review judges them with those
    readonly property var dataRows: ["frameRowArea", "idRowArea"]
    function walkTargets(page) {
        const seen = []
        const small = []
        const wide = []
        const walk = item => {
            for (const child of item.children) {
                if (child.visible && child.width > 0
                        && (child.pressed !== undefined || child.placeholderText !== undefined)) {
                    const name = child.objectName || String(child)
                    const h = child.mapToItem(page, 0, child.height).y
                              - child.mapToItem(page, 0, 0).y
                    seen.push(name)
                    if (h < Theme.touchTarget - 0.5 && dataRows.indexOf(name) < 0)
                        small.push(name + " " + Math.round(h))
                    if (child.mapToItem(page, child.width, 0).x > page.width + 0.5)
                        wide.push(name + "@" + Math.round(child.mapToItem(page, child.width, 0).x) + " w" + Math.round(child.width))
                }
                // A label too: its box ends inside the page
                if (child.visible && child.width > 0 && child.contentWidth !== undefined
                        && child.pressed === undefined && child.placeholderText === undefined
                        && child.mapToItem(page, child.width, 0).x > page.width + 0.5)
                    wide.push((child.objectName || String(child)) + "@"
                              + Math.round(child.mapToItem(page, child.width, 0).x))
                walk(child)
            }
        }
        walk(page)
        return { seen: seen, small: small, wide: wide }
    }

    function test_aPhonesRecordingListRowsAreATouchsSize() {
        const page = createPage()
        page.recordings = [findingsRow]
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        const desk = list.itemAtIndex(0).height
        Theme.compact = true
        try {
            page.width = 384
            verify(waitForPolish(page.Window.window), "laid out")
            verify(list.itemAtIndex(0).height >= Theme.touchTarget - 0.5,
                   "a list row is " + list.itemAtIndex(0).height)
            const b = walkTargets(page)
            verify(b.seen.length >= 1, "buttons found")
            compare(b.small.join(", "), "", "under " + Theme.touchTarget)
        } finally {
            Theme.compact = false
        }
        tryCompare(list.itemAtIndex(0), "height", desk, 2000, "the desktop's row is as it was")
        verify(desk >= 56)
    }

    // Each panel the page opens: the import's guessed map, the Export panel,
    // the Frames table with its filter and By ID list, the compare and align
    // rows, at the narrowest phone widths
    function test_aPhonesImportPanelIsATouchsSize_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }
    function test_aPhonesImportPanelIsATouchsSize(data) {
        const page = createPage()
        page.chooseImportFile(Harness.writeScratchFile("phone-guess-plain.csv",
            "time,rpm\n0,800\n1,900\n2,950\n"))
        const preview = guessed(page)
        Theme.compact = true
        try {
            page.width = data.w
            verify(waitForPolish(page.Window.window), "laid out")
            verify(find(preview, "guessTimeUnit_ms").selected, "the unit shows, changeable")
            const b = walkTargets(page)
            for (const name of ["importKind_csv", "guessTimeUnit_ms"])
                verify(b.seen.indexOf(name) >= 0, name + " is seen: " + b.seen.join(", "))
            compare(b.small.join(", "), "", "under " + Theme.touchTarget)
            compare(b.wide.join(", "), "", "past the right edge at " + data.w)
        } finally {
            Theme.compact = false
        }
    }

    // The walk of an open viewer: every control a touch's size, every label
    // and control inside the page, and the names it must have reached
    function walkPanel(page, data, seen, what) {
        verify(waitForPolish(page.Window.window), "laid out")
        const b = walkTargets(page)
        for (const n of b.seen)
            seen.push(n)
        const out = []
        if (b.small.length > 0)
            out.push(what + ": under " + Theme.touchTarget + ": " + b.small.join(", "))
        if (b.wide.length > 0)
            out.push(what + ": past the right edge at " + data.w + ": " + b.wide.join(", "))
        return out
    }
    function wasWalked(seen, names) {
        for (const n of names)
            verify(seen.indexOf(n) >= 0, n + " was walked: " + seen.join(", "))
    }

    function test_aPhonesViewerPanelsAreATouchsSize_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }
    function test_aPhonesViewerPanelsAreATouchsSize(data) {
        const page = openFindings()
        Theme.compact = true
        try {
            page.width = data.w
            verify(!find(page, "reportNote").visible, "no note, no row for it")
            page.reportNote = "Saved to a-recording-with-a-rather-long-file-name-for-a-phone.zip"
            verify(waitForPolish(page.Window.window), "laid out")
            const note = find(page, "reportNote")
            tryVerify(() => note.width > 0.6 * data.w, 2000,
                      "the note has the row: " + note.width + " of " + note.parent.width)
            const seen = []
            const problems = []
            problems.push(...walkPanel(page, data, seen, "the viewer"))
            openExport(page)
            problems.push(...walkPanel(page, data, seen, "the Export panel"))
            find(page, "exportButton").clicked()

            mouseClick(find(page, "detailView_frames"))
            tryVerify(() => /^\d+ frames? in view$/.test(find(page, "framesWords").text), 10000)
            find(page, "framesFilterToggle").clicked()
            problems.push(...walkPanel(page, data, seen, "the Frames table and its filter"))
            mouseClick(find(page, "framesTable_byId"))
            problems.push(...walkPanel(page, data, seen, "the By ID list"))
            mouseClick(find(page, "detailView_values"))
            problems.push(...walkPanel(page, data, seen, "the values again"))
            compare(problems.join("; "), "")
            wasWalked(seen, ["exportFormat_csv", "exportRows_sweep", "direction_0",
                             "framesTable_byId", "detailView_values", "decodeChoice"])
        } finally {
            Theme.compact = false
        }
    }

    function test_aPhonesCompareAndAlignRowsAreATouchsSize_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }
    function test_aPhonesCompareAndAlignRowsAreATouchsSize(data) {
        const page = openEarly()
        Theme.compact = true
        try {
            page.width = data.w
            verify(waitForPolish(page.Window.window), "laid out")
            find(page, "detailView_about").clicked()   // the pinned form's About holds the rows
            find(page, "compareChoice").activated(1)
            tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
            const seen = []
            const problems = []
            problems.push(...walkPanel(page, data, seen, "the compare and align rows"))
            compare(problems.join("; "), "")
            wasWalked(seen, ["compareChoice", "alignChoice"])
        } finally {
            Theme.compact = false
        }
    }

    function test_aPhonesViewerTargetsAreATouchsSize_data() {
        return [{ tag: "320", w: 320 }, { tag: "384", w: 384 }]
    }
    function test_aPhonesViewerTargetsAreATouchsSize(data) {
        const page = openFindings()
        const rows = find(page, "signalRows")
        compare(rows.itemAtIndex(0).height, page.rowHeight)
        const desk = rows.itemAtIndex(0).height
        verify(desk < 48, "a desktop's signal row is " + desk)
        Theme.compact = true
        try {
            page.width = data.w
            verify(waitForPolish(page.Window.window), "laid out")
            verify(rows.itemAtIndex(0).height >= Theme.touchTarget - 0.5,
                   "a signal row is " + rows.itemAtIndex(0).height)
            const b = walkTargets(page)
            compare(b.wide.join(", "), "", "past the right edge at " + data.w)
            verify(b.seen.indexOf("detailView_values") >= 0, "the view switch is seen: " + b.seen.join(", "))
            compare(b.small.join(", "), "", "under " + Theme.touchTarget)
        } finally {
            Theme.compact = false
        }
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
        compare(find(page, "cursorTime").text, "Click the chart for values")
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
    function onAxis(chart, i) {
        const p = chart.drawn.panes[i]
        return { x: Math.round(chart.x + chart.drawn.left / 2), y: Math.round(chart.y + p.top + p.h / 2) }
    }
    // Two clicks stamped 100 ms apart: Qt Quick Test's own add the
    // double-click interval to their stamps after each release. Each pair
    // later than the last, so it never counts as the second of another
    property int clickStamp: 100000000
    function doubleClick(item, x, y) {
        clickStamp += 10000
        Harness.clickStamped(item, x, y, clickStamp)
        Harness.clickStamped(item, x, y, clickStamp + 100)
    }

    // The viewer's chart is in panes by unit, in the list's order, each
    // with its axis; a compared recording's traces go to their namesakes'
    function test_theViewersChartIsInPanesByUnit() {
        const page = openEarly()
        const chart = panedChart(page)
        const l = chart.drawn
        compare(l.panes.map(p => p.label).join("|"), "rpm|%|°C")
        verify(l.panes.every(p => p.axis && p.ticks.length >= 2), "an axis each")
        verify(l.left > 0, "the axes' column")
        compare(l.seams.length, 2)
        verify(find(page, "paneSeam_1").visible)
        verify(find(page, "paneAxis_2").visible)
        const rows = {}
        for (const s of find(page, "signalRows").model)
            rows[s.index] = s
        for (const tr of l.traces)
            compare(l.panes[tr.pane].label, rows[tr.row].unit, rows[tr.row].name + " in its unit's pane")
        page.compareWith(lateRow)
        tryVerify(() => page.compareAligned && chart.drawn
                        && chart.drawn.traces.some(t => t.dash !== undefined), 10000, "the compare drawn")
        for (const tr of chart.drawn.traces.filter(t => t.dash !== undefined))
            compare(chart.drawn.panes[tr.pane].label, rows[tr.row].unit, "a compared trace in its namesake's pane")
    }

    // ---- Two cursors and the span's statistics

    function rowOf(page, name) {
        const rows = find(page, "signalRows")
        const at = rows.model.findIndex(s => s.name === name)
        verify(at >= 0, "no row " + name)
        tryVerify(() => rows.itemAtIndex(at) !== null, 2000, "the row has a delegate")
        return { item: rows.itemAtIndex(at), index: rows.model[at].index }
    }
    // Both cursors placed, their statistics arrived
    function spanned(page, a, b) {
        page.setCursor(a)
        page.setCursorB(b)
        tryVerify(() => Object.keys(page.spanStats).length > 0, 5000, "the statistics arrive")
    }
    function atTime(page, chart, t) {
        return Math.round(chart.x + chart.drawn.left
                          + (t - page.viewT0) / (page.viewT1 - page.viewT0) * (chart.drawn.plotW - 1))
    }

    // Shift+click places the second cursor (the first, while there is
    // none), each on a sample; a plain click then moves only the first
    function test_aShiftClickPlacesTheSecondCursorAndAClickKeepsIt() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const frame = find(page, "chartFrame")
            const chart = panedChart(page)
            const y = inPlot(chart, 0).y
            mouseClick(frame, atTime(page, chart, 5000), y, Qt.LeftButton, Qt.ShiftModifier)
            verify(page.hasCursor, "with no first cursor it places that")
            verify(!page.hasCursorB)
            compare(page.cursorMs % 100, 0, "on a sample")

            mouseClick(frame, atTime(page, chart, 20000), y, Qt.LeftButton, Qt.ShiftModifier)
            verify(page.hasCursorB, "the second")
            const b = page.cursorBMs
            compare(b % 100, 0, "on a sample")
            verify(Math.abs(b - 20000) < 300, "where it was clicked: " + b)

            wait(Application.styleHints.mouseDoubleClickInterval + 50)
            mouseClick(frame, atTime(page, chart, 10000), y)
            verify(Math.abs(page.cursorMs - 10000) < 300, "the first moved: " + page.cursorMs)
            compare(page.cursorBMs, b, "the second stays")
        } finally {
            restore()
        }
    }

    // The value labels at the plot's right end follow the readout, by time:
    // A walked past B, they show what the rows' A column does
    function test_theLabelsFollowTheReadoutAfterACrossing() {
        const page = openEarly()
        const rpm = rowOf(page, "RPM")
        const labelOf = () => page.series(400).filter(t => t.row === rpm.index)[0]
        spanned(page, 1000, 3000)
        compare(labelOf().labelValue, Number(page.readout[rpm.index].text), "A is the earlier")
        page.setCursor(5000)
        verify(page.cursorMs > page.cursorBMs, "A is past B")
        const earlier = Number(page.readoutB[rpm.index].text)
        verify(earlier !== Number(page.readout[rpm.index].text), "the two cursors read differently")
        compare(find(rpm.item, "cursorValue").text, page.valueText(earlier, 0, "rpm"),
                "the rows' A is the earlier one")
        compare(labelOf().labelValue, earlier, "and so is the label")
        compare(labelOf().labelMs, page.readoutB[rpm.index].atMs, "its dot too")
    }

    // The span line, the rows' B and change, and the table: RPM over 1 s
    // to 3 s of pull-early, by hand from signals.csv
    function test_theSpanReadsItsStatistics() {
        const page = openEarly()
        spanned(page, 1000, 3000)
        compare(find(page, "spanText").text, "A 1.0 s to B 3.0 s · 2.0 s")
        verify(find(page, "spanLine").visible)
        const rpm = rowOf(page, "RPM")
        compare(find(rpm.item, "cursorValue").text, "2,001 rpm")
        tryCompare(find(rpm.item, "cursorBValue"), "text", "1,982 rpm")
        tryCompare(find(rpm.item, "cursorDelta"), "text", "-19 rpm")

        const table = find(page, "spanStats")
        tryVerify(() => table.visible, 2000, "the table shows with a span")
        const row = find(table, "statRow_" + rpm.index)
        compare(find(row, "statChange").text, "-19 rpm")
        compare(find(row, "statLow").text, "1,982 rpm")
        compare(find(row, "statHigh").text, "2,012 rpm")
        compare(find(row, "statMean").text, "2,003 rpm")
        compare(find(row, "statRate").text, "-9.5 rpm/s")
        // A rising signal's change and rate carry their sign
        const up = page.changeText(12.5, 1, "kPa")
        compare(up, "+12.5 kPa")

        // Text has B and no change; a row not plotted has both
        const vin = rowOf(page, "VIN")
        compare(find(vin.item, "cursorDelta").text, "")
        compare(findChild(table, "statRow_" + vin.index), null, "text isn't in the table")
        page.togglePlotted(rpm.index)
        tryVerify(() => findChild(table, "statRow_" + rpm.index) === null, 2000,
                  "the table is the plotted signals")
        compare(find(rpm.item, "cursorDelta").text, "-19 rpm", "the row keeps its change")
    }

    // The readout goes by time, whichever cursor is which
    function test_theEarlierCursorReadsAsA() {
        const page = openEarly()
        spanned(page, 3000, 1000)
        compare(find(page, "spanText").text, "A 1.0 s to B 3.0 s · 2.0 s")
        const rpm = rowOf(page, "RPM")
        compare(find(rpm.item, "cursorValue").text, "2,001 rpm")
        compare(find(rpm.item, "cursorBValue").text, "1,982 rpm")
        tryCompare(find(rpm.item, "cursorDelta"), "text", "-19 rpm")
    }

    // A second cursor on the first is no span: no line, no statistics
    function test_twoCursorsTogetherAreNoSpan() {
        const page = openEarly()
        spanned(page, 2000, 3000)
        page.setCursor(3000)
        verify(page.hasCursorB, "the second stays")
        verify(!page.hasSpan)
        compare(Object.keys(page.spanStats).length, 0)
        verify(!find(page, "spanStats").visible)
    }

    function test_clearRemovesTheSecondCursorAndZoomGoesToTheSpan() {
        const page = openEarly()
        spanned(page, 10000, 20000)
        const zoom = find(page, "zoomToSpan")
        tryVerify(() => zoom.visible, 2000, "the buttons show")
        mouseClick(zoom)
        fuzzyCompare(page.viewT0, 9500, 0.5, "the span with a margin")
        fuzzyCompare(page.viewT1, 20500, 0.5)
        page.askSpanStats()
        mouseClick(find(page, "clearSpan"))
        verify(!page.hasCursorB, "the second has gone")
        compare(page.cursorMs, 10000, "the first stays")
        verify(!find(page, "spanLine").visible)
        verify(!find(page, "spanStats").visible)
        wait(200)
        compare(Object.keys(page.spanStats).length, 0, "an answer on its way is dropped")
    }

    // A pointer's drag in steps, under Shift when asked
    function dragAcross(frame, x, y, dx, modifiers) {
        mousePress(frame, x, y, Qt.LeftButton, modifiers)
        const parts = 8
        for (let k = 1; k <= parts; ++k)
            mouseMove(frame, x + dx * k / parts, y, 10, Qt.LeftButton, modifiers)
        mouseRelease(frame, x + dx, y, Qt.LeftButton, modifiers)
    }

    // Shift+drag draws the span's box and leaves a cursor at each end; a
    // plain drag still pans
    function test_aShiftDragBoxesTheSpan() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const frame = find(page, "chartFrame")
            const chart = panedChart(page)
            const y = inPlot(chart, 0).y
            const x = atTime(page, chart, 10000)
            const dx = atTime(page, chart, 20000) - x
            const view = [page.viewT0, page.viewT1]
            dragAcross(frame, x, y, dx, Qt.ShiftModifier)
            compare([page.viewT0, page.viewT1], view, "the view doesn't pan")
            verify(page.hasSpan, "both cursors")
            verify(Math.abs(page.spanFromMs - 10000) < 300, "from " + page.spanFromMs)
            verify(Math.abs(page.spanToMs - 20000) < 300, "to " + page.spanToMs)
            compare(page.cursorMs % 100, 0, "the ends on samples")
            compare(page.cursorBMs % 100, 0)
            verify(isNaN(page.boxFromMs), "the box has gone")

            page.setView(5000, 25000)
            const before = page.viewT0
            dragAcross(frame, x, y, 80, Qt.NoModifier)
            verify(page.viewT0 < before, "a plain drag pans")
        } finally {
            restore()
        }
    }

    // On a phone a long press places the second cursor and a drag after it
    // boxes the span; on the desktop a long press places nothing
    function test_aLongPressPlacesTheSecondCursorOnAPhone() {
        const page = openEarly()
        const restore = growWindow(page)
        const tap = find(page, "chartTap")
        tap.longPressThreshold = 0.05
        try {
            const frame = find(page, "chartFrame")
            const chart = panedChart(page)
            const y = inPlot(chart, 0).y
            const x = atTime(page, chart, 15000)
            page.setCursor(5000)
            mousePress(frame, x, y)
            wait(200)
            mouseRelease(frame, x, y)
            verify(!page.hasCursorB, "the desktop's long press is nothing")

            Theme.compact = true
            verify(waitForPolish(page.Window.window), "laid out")
            const px = atTime(page, chart, 15000)
            const py = inPlot(chart, 0).y
            mousePress(frame, px, py)
            wait(200)
            verify(page.hasCursorB, "the hold places it")
            verify(Math.abs(page.cursorBMs - 15000) < 300, "where it was held: " + page.cursorBMs)
            compare(page.cursorMs, 5000, "the first stays")
            mouseRelease(frame, px, py)

            page.clearCursorB()
            const view = [page.viewT0, page.viewT1]
            const to = atTime(page, chart, 25000)
            mousePress(frame, px, py)
            wait(200)
            for (let k = 1; k <= 8; ++k)
                mouseMove(frame, px + (to - px) * k / 8, py, 10)
            verify(!isNaN(page.boxFromMs), "the drag draws the box")
            mouseRelease(frame, to, py)
            compare([page.viewT0, page.viewT1], view, "the pan is locked out")
            verify(Math.abs(page.spanFromMs - 15000) < 300, "from " + page.spanFromMs)
            verify(Math.abs(page.spanToMs - 25000) < 300, "to " + page.spanToMs)
        } finally {
            Theme.compact = false
            tap.longPressThreshold = -1
            restore()
        }
    }

    // A handle shows its cursor's time and moves it, snapped at the drop;
    // one dropped on the other removes the second; a press on one places
    // nothing
    function test_aHandleMovesItsCursor() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const frame = find(page, "chartFrame")
            const chart = panedChart(page)
            const a = find(page, "cursorHandleA")
            const b = find(page, "cursorHandleB")
            verify(!a.visible && !b.visible, "none before a cursor")
            spanned(page, 10000, 20000)
            tryVerify(() => a.visible && b.visible, 2000, "both handles")
            compare(find(a, "handleTime").text, "10.0 s")
            compare(find(b, "handleTime").text, "20.0 s")
            verify(b.width >= 24 && b.height >= 24, "a desktop handle: " + b.width + " x " + b.height)
            fuzzyCompare(b.x + b.width / 2, atTime(page, chart, 20000), 1.5, "under its cursor")
            fuzzyCompare(b.y + b.height, chart.y + chart.drawn.top + chart.drawn.plotH + chart.drawn.axisH,
                         0.5, "at the axis's foot")

            const y = b.y + b.height - 6
            mouseClick(frame, b.x + b.width / 2, y)
            compare(page.cursorMs, 10000, "a press on a handle places no cursor")

            const samplesBefore = page.spanStats[0].samples
            const x = b.x + b.width / 2
            dragAcross(frame, x, y, atTime(page, chart, 25000) - x, Qt.NoModifier)
            verify(Math.abs(page.cursorBMs - 25000) < 300, "moved: " + page.cursorBMs)
            compare(page.cursorBMs % 100, 0, "snapped at the drop")
            compare(page.cursorMs, 10000, "the other stays")
            tryVerify(() => page.spanStats[0] !== undefined && page.spanStats[0].samples > samplesBefore,
                      5000, "the statistics follow")

            const from = b.x + b.width / 2
            dragAcross(frame, from, y, atTime(page, chart, 10000) - from, Qt.NoModifier)
            verify(!page.hasCursorB, "dropped on the first, the second goes")
            verify(!b.visible)
        } finally {
            restore()
        }
    }

    // The arrows step a sample of the first plotted signal, B says which
    // cursor, Home and End go to the ends, and a step off the view brings
    // it back
    function test_theKeysStepTheCursors() {
        const page = openEarly()
        page.forceActiveFocus()
        page.setCursor(1000)
        keyClick(Qt.Key_Right)
        tryCompare(page, "cursorMs", 1100, 2000, "a sample on")
        keyClick(Qt.Key_Left)
        tryCompare(page, "cursorMs", 1000, 2000, "a sample back")

        keyClick("b")
        verify(!page.arrowsMoveB, "with no second cursor B does nothing")
        page.setCursorB(3000)
        keyClick("b")
        verify(page.arrowsMoveB)
        keyClick(Qt.Key_Right)
        tryCompare(page, "cursorBMs", 3100, 2000, "B moves")
        compare(page.cursorMs, 1000, "A stays")
        keyClick("b")
        verify(!page.arrowsMoveB, "back to A")

        keyClick(Qt.Key_End)
        tryCompare(page, "cursorMs", 29900, 2000, "the last sample")
        keyClick(Qt.Key_Right)
        wait(100)
        compare(page.cursorMs, 29900, "nothing past the last")
        keyClick(Qt.Key_Home)
        tryCompare(page, "cursorMs", 100, 2000, "the first sample")

        page.setView(10000, 12000)
        page.setCursor(11900)
        keyClick(Qt.Key_Right)
        keyClick(Qt.Key_Right)
        tryVerify(() => page.cursorMs >= 12000 && page.cursorMs <= page.viewT1 && page.viewT0 > 10000,
                  2000, "a step off the view brings it back")
        page.clearCursorB()
    }

    // The first cursor's hint comes once, and the second cursor ends it
    function test_theSecondCursorsHintComesOnce() {
        const page = openEarly()
        page.spanHintSeen = false
        page.setCursor(1000)
        verify(page.spanHintUp)
        verify(find(page, "spanLine").visible)
        compare(find(page, "spanText").text, "At 1.0 s. Shift+click to place the second cursor")
        verify(!find(page, "zoomToSpan").visible, "nothing to zoom to yet")
        page.setCursorB(3000)
        verify(!page.spanHintUp, "the second cursor ends it")
        page.clearCursorB()
        page.cursorMs = NaN
        page.setCursor(2000)
        verify(!page.spanHintUp, "not a second time")
        verify(page.spanHintSeen, "and that's kept")
        verify(!find(page, "spanLine").visible)
    }

    // Maximized, each strip's cell gives A, B and the change
    function test_theStripShowsTheSpanMaximized() {
        const page = openEarly()
        spanned(page, 1000, 3000)
        page.toggleMaximized()
        try {
            const strip = find(page, "cursorStrip")
            tryVerify(() => strip.visible && strip.count > 0, 5000, "the strip shows")
            compare(strip.cellHeight, 56)
            tryVerify(() => strip.itemAtIndex(0) !== null, 2000, "a cell")
            const cell = strip.itemAtIndex(0)
            compare(find(cell, "stripValue").text, "2,001 rpm")
            tryCompare(find(cell, "stripSpan"), "text", "B 1,982 rpm · Δ -19 rpm")
            page.clearCursorB()
            compare(strip.cellHeight, 40)
        } finally {
            page.toggleMaximized()
        }
    }

    // A new open has no span, and a change of units asks again
    function test_theSpanGoesAtAnOpenAndFollowsTheUnits() {
        const page = openEarly()
        spanned(page, 1500, 3000)
        const coolant = rowOf(page, "Coolant")
        tryCompare(find(coolant.item, "cursorDelta"), "text", "0 °C")
        const before = page.statsAsk
        Bridge.unitSystem = 1
        tryVerify(() => page.statsAsk !== before && page.spanStats[coolant.index] !== undefined
                        && page.spanStats[coolant.index].unit === "°F", 5000, "asked again in °F")
        tryCompare(find(rowOf(page, "Coolant").item, "cursorBValue"), "text", "190 °F", 2000,
                   "the second cursor's too")
        page.open(lateRow)
        tryVerify(() => find(page, "signalRows").visible && !page.hasCursor, 10000, "the new open")
        verify(!page.hasCursorB)
        compare(Object.keys(page.spanStats).length, 0)
    }

    // The span's extremes are dots on the trace at their times: filled
    // for the highest, hollow for the lowest
    function test_theExtremesAreDotsOnTheTrace() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const chart = panedChart(page)
            page.setView(0, 5000)
            spanned(page, 1000, 3000)
            const rpm = rowOf(page, "RPM")
            const stat = page.spanStats[rpm.index]
            tryVerify(() => page.spanDots.some(d => d.row === rpm.index), 3000, "the dots are drawn")
            const mine = page.spanDots.filter(d => d.row === rpm.index)
            compare(mine.length, 2)
            const xAt = t => chart.drawn.left + (t - page.viewT0) / (page.viewT1 - page.viewT0)
                             * (chart.drawn.plotW - 1)
            const high = mine.find(d => d.filled), low = mine.find(d => !d.filled)
            fuzzyCompare(high.x, xAt(stat.highestMs), 0.01)
            fuzzyCompare(low.x, xAt(stat.lowestMs), 0.01)
            verify(high.y < low.y, "the highest above the lowest")
            page.clearCursorB()
            tryVerify(() => page.spanDots.length === 0, 3000, "gone with the span")
        } finally {
            restore()
        }
    }

    // A phone's axis strip grows while a cursor exists, and its chart's
    // floor already holds it; the desktop's stays
    function test_aPhonesAxisStripGrowsWithACursor() {
        const page = openEarly()
        const chart = panedChart(page)
        compare(chart.drawn.axisH, ChartLayout.axisH)
        const paints = chart.paints
        page.setCursor(1000)
        tryVerify(() => chart.paints > paints, 2000, "drawn with the cursor")
        compare(chart.drawn.axisH, ChartLayout.axisH, "the desktop's stays")
        page.cursorMs = NaN
        Theme.compact = true
        try {
            page.setCursor(NaN)
            page.cursorMs = NaN
            chart.requestPaint()
            tryVerify(() => chart.drawn && chart.drawn.axisH === ChartLayout.axisH, 2000, "none yet")
            page.setCursor(1000)
            tryVerify(() => chart.drawn && chart.drawn.axisH === ChartLayout.axisHCursor, 2000, "grown")
        } finally {
            Theme.compact = false
        }
    }

    // The frame test plots a signal of each unit first, so it draws the
    // most panes the recording gives
    function test_theFrameTestPlotsOneSignalOfEachUnit() {
        const page = openEarly()
        compare(page.frameTestSignals(find(page, "signalRows").model).map(s => s.name).join(","),
                "RPM,Throttle,Coolant")
        const s = (name, unit) => ({ name: name, unit: unit, isText: false, samples: 1 })
        const list = [s("A", "rpm"), s("B", "rpm"), s("C", "%"), s("D", "°C"), s("E", "°F"),
                      { name: "T", unit: "", isText: true, samples: 1 }, s("F", "V"), s("G", "V")]
        compare(page.frameTestSignals(list).map(s => s.name).join(","), "A,C,D,F",
                "a signal of each pane first")
        compare(page.frameTestSignals(list.slice(0, 3)).map(s => s.name).join(","), "A,C,B",
                "then the others")
    }

    // On a phone the axes' strip is a touch wide, past the captions: a
    // double tap there makes the range automatic again
    function test_aPhonesAxisStripIsATouchWide() {
        const page = openEarly()
        const chart = panedChart(page)
        page.setPaneRange("unit %", { vMin: 0, vMax: 10 })
        const p = chart.drawn.panes[1]
        const x = (chart.drawn.left + Theme.touchTarget) / 2
        verify(x > chart.drawn.left, "past the captions")
        Theme.compact = true
        try {
            compare(page.doubleTapAt(x, p.top + p.h / 2), "axis")
            compare(page.paneRanges["unit %"], undefined)
        } finally {
            Theme.compact = false
        }
        page.setPaneRange("unit %", { vMin: 0, vMax: 10 })
        compare(page.doubleTapAt(x, p.top + p.h / 2), "maximize", "on the desktop that's the plot")
        page.toggleMaximized()
    }

    // A pane's range: Ctrl+wheel zooms it about the pointer, a drag on its
    // axis pans it, a double click on the axis makes it automatic again; a
    // plain wheel still zooms the time, and the range goes at a new open
    function test_aPanesRangeZoomsPansAndComesBack() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const frame = find(page, "chartFrame")
            const chart = panedChart(page)
            const auto = chart.drawn.panes[1]
            const at = inPlot(chart, 1, 0.25)
            const span = page.viewT1 - page.viewT0
            // The plot's edges are the view's: the axes' column is left of it
            fuzzyCompare(page.timeAt(chart.x + chart.drawn.left), page.viewT0, 1e-6, "the plot's left edge")
            fuzzyCompare(page.timeAt(chart.x + chart.drawn.left + chart.drawn.plotW - 1), page.viewT1, 1e-6,
                         "its right edge")
            mouseWheel(frame, at.x, at.y, 0, 120, Qt.NoButton, Qt.ControlModifier)
            const zoomed = page.paneRanges["unit %"]
            verify(zoomed, "the % pane's range is the user's")
            fuzzyCompare(zoomed.vMax - zoomed.vMin, 0.8 * (auto.vMax - auto.vMin), 1e-6, "one notch in")
            const v = ChartLayout.valueAt(auto, at.y - chart.y)
            fuzzyCompare(ChartLayout.valueAt({ top: auto.top, h: auto.h, vMin: zoomed.vMin, vMax: zoomed.vMax },
                                             at.y - chart.y), v, 1e-6, "about the pointer")
            compare(page.viewT1 - page.viewT0, span, "the time stays")
            compare(page.paneRanges["unit rpm"], undefined, "the other panes stay automatic")
            tryVerify(() => chart.drawn.panes[1].manual, 2000, "drawn with it")

            // A drag down the axis moves the range up by its share
            const axis = onAxis(chart, 1)
            mousePress(frame, axis.x, axis.y)
            for (let dy = 5; dy <= 40; dy += 5)
                mouseMove(frame, axis.x, axis.y + dy, 10)
            mouseRelease(frame, axis.x, axis.y + 40)
            const panned = page.paneRanges["unit %"]
            fuzzyCompare(panned.vMin - zoomed.vMin, 40 / auto.h * (zoomed.vMax - zoomed.vMin), 1e-6,
                         "panned by the drag's share of the pane")
            compare(page.viewT1 - page.viewT0, span, "an axis drag moves no time")

            // A double click on the axis: automatic again, nothing maximized
            doubleClick(frame, axis.x, axis.y)
            compare(page.paneRanges["unit %"], undefined, "automatic again")
            verify(!page.chartMaximized, "an axis's double click maximizes nothing")
            verify(!page.hasCursor, "a tap on the axes places no cursor")

            mouseWheel(frame, at.x, at.y, 0, 120)
            verify(page.viewT1 - page.viewT0 < span, "a plain wheel zooms the time")
            compare(Object.keys(page.paneRanges).length, 0, "and no range")

            mouseWheel(frame, at.x, at.y, 0, 120, Qt.NoButton, Qt.ControlModifier)
            verify(page.paneRanges["unit %"], "set again")
            Bridge.unitSystem = 1
            compare(Object.keys(page.paneRanges).length, 0, "new units drop it")
            Bridge.unitSystem = 0
            mouseWheel(frame, at.x, at.y, 0, 120, Qt.NoButton, Qt.ControlModifier)
            verify(page.paneRanges["unit %"], "and again")
            page.backToList()
            page.open(earlyRow)
            tryVerify(() => find(page, "signalRows").visible, 10000, "open again")
            compare(Object.keys(page.paneRanges).length, 0, "a range lasts while the recording is open")
        } finally {
            restore()
        }
    }

    // A seam's drag trades its two panes' heights, kept by the count of
    // panes between runs; a double click on it evens them
    function test_aSeamsDragIsKeptAndADoubleClickEvensIt() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const frame = find(page, "chartFrame")
            const chart = panedChart(page)
            const before = chart.drawn.panes.map(p => p.h)
            const seam = find(page, "paneSeam_0")
            const at = seam.mapToItem(frame, seam.width / 2, seam.height / 2)
            mousePress(frame, at.x, at.y)
            for (let dy = 5; dy <= 30; dy += 5)
                mouseMove(frame, at.x, at.y + dy, 10)
            mouseRelease(frame, at.x, at.y + 30)
            verify(page.paneShares["3"], "the shares kept for three panes")
            tryVerify(() => Math.abs(chart.drawn.panes[0].h - before[0] - 30) < 0.5, 2000,
                      "the upper pane 30 px taller: " + JSON.stringify(chart.drawn.panes.map(p => p.h)))
            fuzzyCompare(chart.drawn.panes[1].h, before[1] - 30, 0.5, "the lower one shorter")
            fuzzyCompare(chart.drawn.panes[2].h, before[2], 0.5, "the third untouched")
            const store = Qt.createQmlObject("import QtCore; Settings { category: \"recordings\" }", testCase)
            tryVerify(() => { store.sync(); return String(store.value("paneShares", "")).indexOf("\"3\"") >= 0 },
                      3000, "written to the store")
            store.destroy()

            const evenAt = seam.mapToItem(frame, seam.width / 2, seam.height / 2)
            doubleClick(frame, evenAt.x, evenAt.y)
            compare(page.paneShares["3"], undefined, "evened")
            verify(!page.chartMaximized, "a seam's double click maximizes nothing")
            tryVerify(() => Math.abs(chart.drawn.panes[0].h - chart.drawn.panes[1].h) < 0.5, 2000, "even")
        } finally {
            page.setPaneShares(3, null)
            restore()
        }
    }

    // On a phone a vertical pinch over a pane zooms its range; a sideways
    // one zooms the time
    function test_aVerticalPinchZoomsAPanesRange() {
        const page = openEarly()
        const restore = growWindow(page)
        try {
            const frame = find(page, "chartFrame")
            const chart = panedChart(page)
            const auto = chart.drawn.panes[0]
            const mid = inPlot(chart, 0)
            const span = page.viewT1 - page.viewT0
            const touch = touchEvent(frame)
            // Fingers moving apart up and down: the pane's range narrows
            // about the pinch's middle (the handler starts past its drag
            // threshold, so the factor is the scale from there)
            touch.press(0, frame, mid.x, mid.y - 4).press(1, frame, mid.x + 4, mid.y + 4).commit()
            const steps = [8, 12, 16, 20, 24, 28, 32, 36, 40]
            for (const d of steps)
                touch.move(0, frame, mid.x, mid.y - d).move(1, frame, mid.x + 4, mid.y + d).commit()
            touch.release(0, frame, mid.x, mid.y - 40).release(1, frame, mid.x + 4, mid.y + 40).commit()
            const range = page.paneRanges["unit rpm"]
            verify(range, "the rpm pane's range is the user's")
            verify(range.vMax - range.vMin < (auto.vMax - auto.vMin) * 0.95,
                   "the fingers apart: a narrower range, " + (range.vMax - range.vMin)
                   + " of " + (auto.vMax - auto.vMin))
            const y = mid.y - chart.y
            fuzzyCompare(ChartLayout.valueAt({ top: auto.top, h: auto.h, vMin: range.vMin, vMax: range.vMax }, y),
                         ChartLayout.valueAt(auto, y), 0.03 * (auto.vMax - auto.vMin), "about the middle")
            compare(page.viewT1 - page.viewT0, span, "the time stays")

            const t = touchEvent(frame)
            t.press(0, frame, mid.x - 4, mid.y).press(1, frame, mid.x + 4, mid.y + 4).commit()
            for (const d of steps)
                t.move(0, frame, mid.x - d, mid.y).move(1, frame, mid.x + d, mid.y + 4).commit()
            t.release(0, frame, mid.x - 40, mid.y).release(1, frame, mid.x + 40, mid.y + 4).commit()
            verify(page.viewT1 - page.viewT0 < span, "a sideways pinch zooms the time")
        } finally {
            restore()
        }
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
        verify(Qt.colorEqual(find(list.itemAtIndex(1), "baselineBadge").background.border.color,
                             Theme.textDim), "outlined in the dim tone")
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

    // The baseline's name is as tall as the buttons beside it: a touch's size on a phone
    function test_theBaselineLabelIsATouchsSizeOnAPhone() {
        const page = createBookPage("")
        page.recordings = [startLateRow, startEarlyRow]
        openRow(page, startEarlyRow)
        mouseClick(find(page, "useAsBaseline"))
        const mine = find(page, "isBaseline")
        verify(mine.visible)
        compare(mine.height, Theme.controlHeight, "the desktop's")
        Theme.compact = true
        try {
            tryCompare(mine, "height", 48)
        } finally {
            Theme.compact = false
        }
        tryCompare(mine, "height", Theme.controlHeight)
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
        // The list ends About, past the page's first screen
        const scroll = find(page, "recordingsScroll")
        scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height)
        verify(waitForPolish(page.Window.window), "scrolled")
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
        verify(/^Frame test, 1 plotted in 1 pane at \d+ px: [\d.]+ frames\/s shown panning, [\d.]+ zooming; [\d.]+ charts drawn\/s/.test(result),
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

    // A row's detail is caption text, and says "replays as" only where
    // the profile differs from the address's own
    function test_aRowsDetailSaysReplaysAsOnlyWhereItDiffers() {
        const page = createPage()
        const row = (source, profile) => ({ title: "T", uri: "replay://x?profile=" + profile,
                                            folder: Harness.recordingFixtures + "/findings",
                                            source: source, profile: profile, frames: true })
        page.recordings = [row("mock://x?profile=mock", "mock"), row("mock://x?profile=raw", "mock"),
                           row("mock://x", "mock"), row("", "mock"), row("mock://x", "raw"),
                           row("elm327://COM3", "obd2")]
        const list = find(page, "recordingList")
        tryVerify(() => list.itemAtIndex(5) !== null, 2000, "the rows have delegates")
        const detail = i => find(list.itemAtIndex(i), "recordingDetail")
        compare(detail(0).text, "mock://x?profile=mock", "the same profile: the address alone")
        compare(detail(1).text, "mock://x?profile=raw · replays as mock")
        compare(detail(2).text, "mock://x", "a mock address replays as mock: nothing to say")
        compare(detail(3).text, "replays as mock")
        compare(detail(4).text, "mock://x · replays as raw", "a mock address under another profile: said")
        compare(detail(5).text, "elm327://COM3 · replays as obd2", "an adapter's address without one: said")
        compare(detail(0).font.pixelSize, Theme.fontCaption, "caption text, never under the floor")
        compare(find(list.itemAtIndex(0), "replayButton").text, "Replay")
    }

    // The viewer's Export opens its panel and shows as selected while it
    // is open, not as a second primary; its lower controls name what
    // they choose
    function test_exportShowsAsSelectedAndNamesItsChoices() {
        const page = openFindings()
        const button = find(page, "exportButton")
        compare(button.text, "Export…")
        verify(!button.checked && !button.highlighted, "closed: a plain button")
        const panel = openExport(page)
        verify(button.checked, "open: selected")
        verify(!button.highlighted, "never primary: Save as… is the panel's")
        verify(find(panel, "exportGo").highlighted)
        const signals = find(panel, "exportColumnsLabel")
        compare(signals.text, "Signals:")
        compare(signals.visible, find(panel, "exportColumns_own").visible, "with the switch it names")
        const was = Theme.developerMode
        Theme.developerMode = true
        const hint = find(panel, "exportRawHint")
        verify(hint.visible, "the scrub's hint beside its box")
        verify(hint.text.indexOf("identification") >= 0, hint.text)
        Theme.developerMode = false
        verify(!hint.visible)
        Theme.developerMode = was
        button.clicked()
        tryVerify(() => !panel.visible, 2000, "closed again")
        verify(!button.checked)
    }

    // The Frames view's table switch: List or By ID, under Values / Frames
    function test_theFramesTableSwitchReadsListOrById() {
        const page = openFindings()
        mouseClick(find(page, "detailView_frames"))
        verify(page.framesShown)
        compare(find(page, "framesTable_frames").text, "List")
        compare(find(page, "framesTable_byId").text, "By ID")
    }

    // On a phone with developer mode on (Frame test in the row) the
    // toolbar's hint keeps its words, wrapping onto a second caption line
    // instead of eliding
    function test_theCursorHintKeepsItsWordsOnAPhone() {
        const page = openFindings()
        const hint = find(page, "cursorTime")
        const was = Theme.developerMode
        Theme.compact = true
        Theme.dense = true
        Theme.developerMode = true
        try {
            page.width = 384
            verify(waitForPolish(page.Window.window), "laid out")
            tryVerify(() => find(page, "frameTest").visible, 2000, "Frame test in the row")
            compare(hint.text, "Tap for values")
            tryVerify(() => !hint.truncated, 2000, "every word shows")
            verify(hint.lineCount <= 2, hint.lineCount + " lines")
            compare(hint.font.pixelSize, Theme.fontCaption)
            // Where the row leaves it less than its words, it wraps onto a
            // second line rather than eliding
            compare(hint.wrapMode, Text.WordWrap, "wraps on a phone")
            compare(hint.maximumLineCount, 2, "two lines at most")
        } finally {
            Theme.developerMode = was
            Theme.compact = false
            Theme.dense = false
        }
    }

    // Compared with a recording that holds the signal's values, an empty
    // signal's dashed trace draws and the chart says nothing about it
    function test_aComparedRecordingDrawsASignalTheFirstLacks() {
        const page = createPage()
        const events = "ecuio session recording, format 2 (ADR-0049)\n"
                       + "started  2026-11-03 09:00:00.000 (local time)\n"
                       + "profile  test\n"
                       + "signal  RPM,number,rpm,0,0\n"
                       + "signal  Quiet,number,rpm,0,0\n"
                       + "\n00:00:01.000  +1.000  end  test: 0 frames, 1 values, 0 records "
                       + "dropped, 0 notes dropped\n"
        const first = Harness.writeFramesRecording("lacks-quiet", 1, 10000)
        const second = Harness.writeFramesRecording("has-quiet", 1, 10000)
        verify(first !== "" && second !== "")
        verify(Harness.writeScratchFile("lacks-quiet/events.log", events).toString() !== "")
        verify(Harness.writeScratchFile("lacks-quiet/signals.csv",
                   "host_us,signal,value,unit\n100000,RPM,1000,rpm\n").toString() !== "")
        verify(Harness.writeScratchFile("has-quiet/events.log", events).toString() !== "")
        verify(Harness.writeScratchFile("has-quiet/signals.csv",
                   "host_us,signal,value,unit\n100000,RPM,900,rpm\n100000,Quiet,5,rpm\n"
                   + "200000,Quiet,7,rpm\n").toString() !== "")
        page.open({ title: "Lacks", uri: "replay://x?profile=test", source: "", folder: first,
                    profile: "test" })
        tryVerify(() => find(page, "exportButton").visible, 10000, "the viewer opens")
        const quiet = page.shownSignals.find(s => s.name === "Quiet")
        const rpm = page.shownSignals.find(s => s.name === "RPM")
        page.togglePlotted(rpm.index)
        page.togglePlotted(quiet.index)
        compare(page.plottedNoValues, ["Quiet"], "alone, it has nothing to draw")
        compare(page.series(400).length, 0)
        page.compareWith({ title: "Has", folder: second })
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        compare(page.plottedNoValues, [], "the compared recording draws it")
        const all = page.series(400)
        compare(all.length, 1, "its dashed trace alone")
        verify(all[0].dash !== undefined, "dashed")
        compare(all[0].row, quiet.index)
        page.stopComparing()
        compare(page.plottedNoValues, ["Quiet"], "alone again")
    }

    // A ticked signal with no values in the recording: the chart says so
    // instead of staying blank
    function test_aPlottedSignalWithNoValuesIsSaidInTheChart() {
        const page = createPage()
        compare(page.noValuesText(["RPM"]), "RPM has no values in this recording")
        compare(page.noValuesText(["RPM", "Speed"]), "RPM and Speed have no values in this recording")
        compare(page.noValuesText(["RPM", "Speed", "Load"]),
                "RPM, Speed and Load have no values in this recording")
        // A values recording listing two signals, with rows for one: the
        // harness's frames folder, its events.log and signals.csv written over
        const folder = Harness.writeFramesRecording("no-values", 1, 10000)
        verify(folder !== "")
        verify(Harness.writeScratchFile("no-values/events.log",
                   "ecuio session recording, format 2 (ADR-0049)\n"
                   + "started  2026-11-03 09:00:00.000 (local time)\n"
                   + "profile  test\n"
                   + "signal  RPM,number,rpm,0,0\n"
                   + "signal  Quiet,number,rpm,0,0\n"
                   + "\n00:00:01.000  +1.000  end  test: 0 frames, 1 values, 0 records "
                   + "dropped, 0 notes dropped\n").toString() !== "")
        verify(Harness.writeScratchFile("no-values/signals.csv",
                   "host_us,signal,value,unit\n100000,RPM,1000,rpm\n").toString() !== "")
        page.open({ title: "Quiet", uri: "replay://x?profile=test", source: "", folder: folder,
                    profile: "test" })
        tryVerify(() => find(page, "exportButton").visible, 10000, "the viewer opens")
        const chart = find(page, "chart")
        const empty = page.shownSignals.find(s => s.name === "Quiet")
        verify(empty !== undefined && empty.samples === 0, "a listed signal with no rows")
        verify(page.plotted[empty.index] !== true, "not plotted on its own")
        // RPM is plotted by default and draws, so the chart says nothing
        // about Quiet until RPM is unticked: the words are for a chart
        // with nothing to draw
        page.togglePlotted(empty.index)
        compare(page.plottedNoValues, [], "RPM still draws")
        const rpm = page.shownSignals.find(s => s.name === "RPM")
        page.togglePlotted(rpm.index)
        tryCompare(chart, "emptyText", "Quiet has no values in this recording", 3000)
        compare(page.series(400).length, 0, "nothing to draw, so the words show")
        page.togglePlotted(empty.index)
        compare(chart.emptyText, "No signals plotted; tick a row to plot it")
    }
}
