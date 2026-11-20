// File/layer: tests/qml/tst_recordings.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings (test_qml)

import QtQuick
import QtTest
import App
import AppTest

RecordingsKit {
    id: testCase
    name: "Recordings"

    SignalSpy {
        id: replaySpy
        signalName: "replayRequested"
    }

    // Compare's controls open above the chart from the title row's button (a
    // phone's About holds the button), close with it, and open for a compare
    // started from anywhere; Stop closes them
    function test_theComparesButtonOpensItsControlsAboveTheChart() {
        const page = createPage()
        page.recordings = [twiceRow, earlyRow]
        openRow(page, twiceRow)
        const choice = find(page, "compareChoice")
        const chart = find(page, "chartFrame")
        verify(!choice.visible, "the controls wait for the button")
        find(page, "compareButton").clicked()
        verify(page.compareOpen)
        verify(waitForPolish(page.Window.window), "laid out")
        verify(choice.visible, "open")
        verify(choice.mapToItem(page, 0, 0).y < chart.mapToItem(page, 0, 0).y, "above the chart")
        find(page, "compareButton").clicked()
        verify(!page.compareOpen && !choice.visible, "closed again")
        page.comparePart.compareWith(earlyRow)
        verify(page.compareOpen, "a started compare opens them")
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        verify(waitForPolish(page.Window.window), "laid out")
        verify(find(page, "compareStatus").visible, "with the status")
        find(page, "stopCompare").clicked()
        verify(!page.compareOpen, "Stop closes them")
        verify(!find(page, "compareStatus").visible)
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
        // Its row by address, not by place: a CSV import made in the same
        // second sorts above it ("<stamp> CSV import" after "<stamp>")
        const at = page.recordings.findIndex(r => r.source === "mock://recordings-page")
        verify(at >= 0, "the recording that ended is in the list")
        tryVerify(() => list.itemAtIndex(at) !== null, 2000, "the row has a delegate")
        compare(find(list.itemAtIndex(at), "recordingDetail").text, "mock://recordings-page",
                "a mock address replays as mock: the address alone")
        verify(page.recordings[at].folder !== "", "a row knows its folder")
    }

    // A CSV import (ADR-0054's appendix "the CSV import as built") is
    // listed and opens from its row: values alone, so no Replay, no frames
    // check, no decode through a profile, and its numbers plotted
    function test_aCsvImportIsListedAndOpensWithoutFrames() {
        const folder = Harness.importCsvFixture("logger-a", "20261105-220322 Logger A")
        verify(folder !== "", "the fixture imports")
        const page = createPage()
        page.refreshPart.refresh()
        const at = page.recordings.findIndex(r => r.folder.endsWith("/20261105-220322 Logger A"))
        verify(at >= 0, "the import is listed")
        compare(page.recordings[at].title, "2026-11-05 22:03:22 · Logger A")
        compare(page.recordings[at].frames, false)
        const row = listRow(page, at)
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
        compare(page.seriesPart.series(400).length, 3, "its numbers are plotted")
    }

    // Through the phone's picker: the panel names the file and the map as the
    // picker does (a provider's URL ends in an id), and a picker with no
    // document says so
    function test_aPickedLogAndMapAreNamedAsThePickerNamesThem() {
        const page = createPage()
        const csv = Harness.writeScratchFile("picked-log.csv", "time,rpm\n0,800\n0.5,900\n")
        const map = Harness.writeScratchFile("picked-map.json",
            '{"havne_column_map": 1, "time": {"column": "time", "unit": "s"}, '
            + '"signals": [{"column": "rpm", "name": "RPM", "unit": "rpm"}]}')
        DocumentPicker.setPickerForTesting(true)
        const files = find(page, "importDialog")
        files.usePicker = true
        files.open()
        DocumentPicker.deliver(files.request, -1, csv.toString(), "logger (1).csv")
        verify(find(page, "importPanel").visible)
        compare(page.importFile, csv, "the picked URL as it came")
        compare(find(page, "importFileName").text, "Import logger (1).csv")
        tryVerify(() => !page.importGuessing, 10000, "the guess ends")

        const maps = find(page, "mapDialog")
        maps.usePicker = true
        maps.open()
        DocumentPicker.deliver(maps.request, -1, map.toString(), "map (1).json")
        compare(page.importMap, map)
        compare(find(page, "importMapName").text, "map (1).json")
        maps.open()
        DocumentPicker.deliver(maps.request, -1, "", "")
        compare(find(page, "importNote").text, "The picker returned no document")
        compare(page.importMap, map, "nothing read")

        page.importNote = ""
        files.open()
        DocumentPicker.deliver(files.request, -1, "", "")
        compare(find(page, "importNote").text, "The picker returned no document",
                "the log's own picker says so too")
        compare(page.importFile, csv, "the log picked before stays")
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
        page.openPart.chooseImportFile(csv)
        verify(panel.visible)
        compare(find(page, "importFileName").text, "Import import-log.csv")
        verify(find(page, "importKind_csv").selected, "guessed from its first line")
        compare(find(page, "importMapName").text, "Guessed from the header")
        verify(!find(page, "importInterface").visible)
        tryVerify(() => !Bridge.recordingImport.busy, 10000, "the guess ends")
        verify(!find(page, "importStart").enabled, "a time with fractions has no unit yet")
        page.openPart.chooseImportMap(map)
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
        verify(page.viewing, "the import opens its recording")
        verify(note.visible, "its words stay above the viewer")
        page.openPart.backToList()
        verify(!note.visible)
        verify(page.recordings.some(r => r.title.endsWith(" · CSV import")),
               "it joins the list")

        const log = Harness.writeScratchFile("import-two.log",
            "(1.000000) can0 7E8#01\n(1.100000) can1 123#02\n(1.600000) can1 123#03\n")
        page.openPart.chooseImportFile(log)
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
        page.openPart.backToList()
        verify(page.recordings.some(r => r.title.endsWith(" · candump import")))

        // The next log needs no interface: the one typed for the last is gone
        const one = Harness.writeScratchFile("import-one.log", "(1.000000) can0 7E8#01\n")
        page.openPart.chooseImportFile(one)
        compare(find(page, "importInterface").text, "")
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(note.text, "Imported 1 frames over 0.0 s")
        page.openPart.backToList()

        page.openPart.chooseImportFile(log)
        find(page, "importKind_csv").clicked()
        compare(page.importKind, RecordingImport.Csv)
        tryVerify(() => !Bridge.recordingImport.busy, 10000, "the guess ends")
        verify(!find(page, "importStart").enabled, "a candump log guesses no map")
        find(page, "importCancel").clicked()
        verify(!panel.visible)
        verify(!note.visible)
    }

    // The panel's Import and Cancel stand at its head, above the columns,
    // and a typed label names the folder
    function test_theImportPanelPutsItsActionsFirstAndTakesALabel() {
        const page = createPage()
        const csv = Harness.writeScratchFile("head-log.csv",
            "Time (ms),RPM [rpm],Load [%]
0,800,12
500,900,30
")
        page.openPart.chooseImportFile(csv)
        const preview = guessed(page)
        const start = find(page, "importStart")
        const label = find(page, "importLabel")
        verify(label.visible)
        const head = start.mapToItem(page, 0, 0).y
        verify(head < find(preview, "guessRow_0").mapToItem(page, 0, 0).y,
               "Import is above the first column")
        verify(find(page, "importCancel").mapToItem(page, 0, 0).y
               < find(preview, "guessRow_0").mapToItem(page, 0, 0).y,
               "Cancel is above the first column too")
        const box = find(preview, "include")
        verify(box.indicator.x < 40, "the include box starts its row")
        label.text = "Cold start"
        start.clicked()
        tryVerify(() => page.viewing, 10000, "the import opens its recording")
        verify(page.openedTitle.endsWith(" · Cold start"), page.openedTitle)
        page.openPart.backToList()
    }

    // A row says its length, its kind and signals, and its marks
    function test_aRowSaysItsLengthKindAndMarks() {
        const page = createPage()
        compare(page.rowInfoPart.rowFacts({ durationMs: 60000, log: true, signals: 13, marks: 0, frames: true }),
                "1 min 0 s · log, 13 signals")
        compare(page.rowInfoPart.rowFacts({ durationMs: 12000, log: false, signals: 5, marks: 2, frames: true }),
                "12 s · session · 2 marks")
        compare(page.rowInfoPart.rowFacts({ durationMs: -1, log: false, signals: 3, marks: 1, frames: false }),
                "3 signals · 1 mark")
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
        page.openPart.chooseImportFile(csv)
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

    // A CSV's rows with no time (one mid-file, a last one of padding) are
    // left out: the guess's notes say so before the import, and the
    // finished line counts them, after the ignored columns when both
    function test_anImportSaysTheRowsWithNoTimeItLeftOut() {
        const page = createPage()
        const text = "Time (ms),RPM [rpm],Load [%],Note\n0,800,12,a\n,850,20,b\n"
                     + "500,900,30,c\n , , , \n"
        page.openPart.chooseImportFile(Harness.writeScratchFile("untimed-log.csv", text))
        const preview = guessed(page)
        verify(find(page, "importStart").enabled)
        verify(preview.guess.notes.some(
                   n => n.startsWith("2 of the 4 rows read have no time")),
               "the guess says so: " + preview.guess.notes.join("; "))
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(find(page, "importNote").text,
                "Imported 6 values, 2 rows with no time left out")
        verify(Qt.colorEqual(find(page, "importNote").color, Theme.success))

        // One such row is said as one
        page.openPart.chooseImportFile(Harness.writeScratchFile("untimed-log-1.csv",
            "Time (ms),RPM [rpm],Load [%],Note\n0,800,12,a\n,850,20,b\n500,900,30,c\n"))
        guessed(page)
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(find(page, "importNote").text,
                "Imported 6 values, 1 row with no time left out")

        page.openPart.chooseImportFile(Harness.writeScratchFile("untimed-log-2.csv", text))
        guessed(page)
        page.openPart.chooseImportMap(Harness.writeScratchFile("untimed-map.json",
            '{"havne_column_map": 1, "time": {"column": "Time (ms)", "unit": "ms"}, '
            + '"signals": [{"column": "RPM [rpm]", "name": "RPM", "unit": "rpm"}]}'))
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        compare(find(page, "importNote").text,
                "Imported 2 values; columns ignored: 2; 2 rows with no time left out")
    }

    // A time named so, of whole numbers with no unit, reads as ms: Import is ready,
    // the unit shows chosen and changes with a tap
    function test_aTimeWithoutAUnitReadsAsMilliseconds() {
        const page = createPage()
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-plain.csv",
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
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-fraction.csv",
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
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-save-unit.csv",
            "time,rpm\n0,800\n1,900\n2,950\n"))
        let preview = guessed(page)
        verify(preview.ready)
        verify(find(preview, "saveMapButton").enabled, "ms, as the guess read it")
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-save-zone.csv",
            "time,rpm\n2026-11-05T22:03:22,800\n2026-11-05T22:03:23,900\n"))
        preview = guessed(page)
        verify(!preview.ready)
        verify(preview.included > 0)
        verify(!find(preview, "saveMapButton").enabled, "no zone to read it by")
    }

    // A column whose header the file has twice is left out for good
    function test_aDuplicatedHeaderColumnCantBePutBack() {
        const page = createPage()
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-dup.csv",
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
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-zone.csv",
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
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-none.csv",
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
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-back.csv",
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
            page.openPart.chooseImportFile(Harness.writeScratchFile("nostart.csv",
                "time (s),rpm\n0,800\n1,900\n"))
            verify(page.importFailed)
            verify(!Bridge.recordingImport.busy)
            verify(note.visible)
            verify(note.text.indexOf("The file couldn't be read") === 0, note.text)
            verify(Qt.colorEqual(note.color, Theme.danger))

            page.openPart.chooseImportFile(Harness.writeScratchFile("nostart.log",
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

    // Cancel on an import under way stops it: nothing is imported, the
    // words say so in neither the made nor the failed color, and the panel
    // stays with its choices; the next Import runs, and an idle Cancel
    // closes the panel
    function test_cancelStopsAnImportUnderWay() {
        const page = createPage()
        const panel = find(page, "importPanel")
        const note = find(page, "importNote")
        const cancel = find(page, "importCancel")
        const busy = find(page, "importBusy")
        page.refreshPart.refresh()
        const before = page.recordings.length
        Harness.holdImports(Bridge.recordingImport, 15000)
        try {
            page.openPart.chooseImportFile(Harness.writeScratchFile("cancel-one.log",
                "(1.000000) can0 7E8#01\n"))
            verify(cancel.enabled)
            find(page, "importStart").clicked()
            verify(Bridge.recordingImport.busy)
            verify(cancel.enabled, "an import under way can be stopped")
            compare(busy.text, "Importing...")
            cancel.clicked()
            verify(page.importStopping)
            verify(!cancel.enabled, "asked once")
            compare(busy.text, "Stopping...")
            verify(panel.visible, "the panel stays while it stops")
            tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                      "the canceled import ends")
            compare(note.text, "Import canceled: nothing was imported")
            verify(note.visible)
            verify(Qt.colorEqual(note.color, Theme.textDim), "neither made nor failed")
            verify(!page.importFailed)
            verify(!page.importStopping)
            verify(panel.visible, "its choices are kept for another try")
            verify(!page.importAsksInterface, "a canceled log isn't asked for its interface")
            verify(cancel.enabled)
            verify(find(page, "importStart").enabled)
        } finally {
            // A worker took its hold when it started: after a failed check
            // it is freed here, or the shared importer stays busy for the
            // cases after this one
            Harness.holdImports(Bridge.recordingImport, 0)
            Bridge.recordingImport.cancel()
            tryVerify(() => !Bridge.recordingImport.busy, 20000, "the held import ends")
        }
        page.refreshPart.refresh()
        compare(page.recordings.length, before, "nothing was imported")

        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the next import ends")
        compare(note.text, "Imported 1 frames over 0.0 s")
        verify(Qt.colorEqual(note.color, Theme.success))
        verify(!panel.visible)
        page.openPart.backToList()   // the import opened its recording
        compare(page.recordings.length, before + 1)

        page.openPart.chooseImportFile(Harness.writeScratchFile("cancel-two.log",
            "(1.000000) can0 7E8#01\n"))
        verify(panel.visible)
        cancel.clicked()
        verify(!panel.visible, "with nothing under way Cancel closes the panel")
        verify(!Bridge.recordingImport.busy)
    }

    // A refused import's words stay with the import: opening a recording
    // from the list afterwards shows none of them
    function test_aRefusedImportsWordsDoNotSitOverTheNextRecording() {
        const page = createPage()
        page.openPart.chooseImportFile(Harness.writeScratchFile("note-refused.csv",
            "time (s),rpm
0,800
1,900
2,950
"))
        const preview = guessed(page)
        find(guessRow(preview, 0), "name").text = "n".repeat(40)
        find(page, "importStart").clicked()
        tryVerify(() => !Bridge.recordingImport.busy && page.importNote !== "", 10000,
                  "the import ends")
        verify(page.importFailed)
        verify(find(page, "importNote").visible, "the refusal is said")
        page.recordings = [twiceRow]
        openRow(page, twiceRow)
        compare(page.importNote, "", "opening a recording clears it")
        verify(!find(page, "importNote").visible, "nothing over the recording")
    }

    function test_aRefusedGuessImportKeepsItsEdits() {
        const page = createPage()
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-refused.csv",
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
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-save.csv",
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
        page.openPart.chooseImportFile(Harness.writeScratchFile("guess-next.csv",
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
        page.openPart.chooseImportMap(page.savedMaps[at].url)
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
        mouseClick(find(listRow(page, 0), "replayButton"))
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
        page.cursorsPart.setView(2500, 7250)
        button.clicked()
        compare(replaySpy.signalArguments[1][0], findingsRow.uri + "&from=2.5&to=7.25")
        page.cursorsPart.setView(0, 4000)
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
}
