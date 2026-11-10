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

        compare(Relay.start("elm327://bt:00:11:22:33:44:55", 0), "", "the relay starts")
        tryVerify(() => !button.enabled, 2000, "off while relaying")
        compare(button.text, "Stop the relay to replay")
        Relay.stop()
        tryVerify(() => button.enabled, 2000, "on again")

        Bridge.connectSessionAsync("mock://replay-words")
        verify(Bridge.connecting, "the connect is under way")
        verify(!button.enabled)
        compare(button.text, "Replay", "nothing to disconnect yet")
        tryVerify(() => Bridge.connected, 10000, "the mock connects")
        compare(button.text, "Disconnect to replay")
        Bridge.disconnectSession()
        verify(Bridge.disconnecting, "the teardown is under way")
        verify(!button.enabled)
        compare(button.text, "Replay", "already disconnecting")
        tryVerify(() => !Bridge.connected && !Bridge.disconnecting, 5000)
        verify(button.enabled, "on again")
        compare(button.text, "Replay")
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
        verify(/ rows, /.test(grid.text), grid.text)
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

    // One order on both forms: the chart, its zoom row, the switch and the
    // filter, the rows, then About this recording. On a phone Replay and
    // Save report sit in About; on the desktop in the title row
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
            const order = ["openedTitle", "chartFrame", "chartSplitter", "zoomAll"]
                .concat(data.phone ? ["detailView_values"] : [])
                .concat(["recordingFilter", "signalRows", "aboutHeading", "recordingInfo",
                         "decodeChoice", "compareChoice", "checkSummary"])
            const ys = order.map(yOf)
            for (let i = 1; i < order.length; ++i)
                verify(ys[i] > ys[i - 1],
                       order[i] + " (" + ys[i] + ") under " + order[i - 1] + " (" + ys[i - 1] + ")")
            const save = yOf("saveReport")
            const replay = yOf("replayView")
            if (data.phone) {
                verify(save > yOf("recordingInfo") && save < yOf("decodeChoice"), "Save report in About")
                compare(replay, save, "Replay beside it")
            } else {
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

    // The chart's height until the splitter sets one: 45 % of the page on
    // the desktop, 40 % on a phone, never under 248 there (three panes of
    // 72 and the time axis)
    function test_theChartTakesAShareOfThePage_data() {
        return [{ tag: "desktop", w: 1280, h: 720, phone: false, want: 0.45 * 720 },
                { tag: "phone", w: 384, h: 832, phone: true, want: 0.4 * 832 },
                { tag: "short phone", w: 384, h: 500, phone: true, want: 248 }]
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
            tryVerify(() => Math.abs(chart.height - data.want) < 1, 2000,
                      "the chart is " + data.want)
            verify(Math.abs(chart.height - data.want) < 1, "the chart is " + chart.height)
        } finally {
            Theme.compact = false
            page.chartHeight = kept
            page.width = 900
            page.height = 900
        }
    }

    // The Export panel opens under the title row, so the page goes to its
    // top, wherever it was scrolled
    function test_theExportPanelOpensAtThePagesTop() {
        const page = openFindings()
        Theme.compact = true
        try {
            page.width = 384
            page.height = 600
            verify(waitForPolish(page.Window.window), "laid out")
            const scroll = find(page, "recordingsScroll")
            verify(scroll.contentHeight > scroll.height, "the page scrolls")
            scroll.contentY = scroll.contentHeight - scroll.height
            page.openExport()
            compare(scroll.contentY, 0, "at the top")
            const panel = find(page, "exportPanel")
            verify(panel.visible, "the panel")
            verify(panel.mapToItem(scroll, 0, 0).y < scroll.height / 2, "on the first screen")
            page.closeExport()
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
            verify(find(preview, "guessWaiting").visible, "the unit is asked for")
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
