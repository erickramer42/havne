// File/layer: tests/qml/tst_recordings_export.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings_export (test_qml)

import QtQuick
import QtTest
import App
import AppTest

RecordingsKit {
    id: testCase
    name: "Recordings"

    // Opens pull-early and waits for its viewer
    function openEarlyViewer() {
        const page = createPage()
        page.openPart.open(earlyRow)
        tryVerify(() => find(page, "exportButton").visible, 10000, "the viewer opens")
        return page
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

    // The viewer's two saves through the save picker: a picker with no
    // document says so in Save report's note and in the export's
    function test_theViewersSavePickersSayWhyNothingWasSaved() {
        const page = openEarlyViewer()
        DocumentPicker.setPickerForTesting(true)
        const report = find(page, "reportDialog")
        report.usePicker = true
        report.open()
        compare(DocumentPicker.titleForTesting(), "health-report.txt")
        DocumentPicker.deliver(report.request, -1, "", "")
        compare(page.reportNote, "Report not saved: The picker returned no document")
        verify(page.reportFailed)

        const panel = openExport(page)
        const save = find(panel, "exportSaveDialog")
        save.usePicker = true
        save.open()
        compare(DocumentPicker.mimeTypeForTesting(), "text/csv")
        DocumentPicker.deliver(save.request, -1, "", "")
        compare(panel.note, "Not exported: The picker returned no document")
        verify(panel.failed)
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
        page.openPart.open({ title: "Fast", folder: folder })
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
        page.openPart.open({ title: "Slow", folder: folder })
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
        const sizedAt = ms => panel.sizes.some(s => s.rowMs === ms)
        panel.customText = "50"
        tryVerify(() => sizedAt(50), 10000, "the custom interval sized")
        compare(textOf("exportRows_custom"), "no fewer rows than every sweep",
                "a typed interval finer than the recording says so too")
        panel.customText = "1000"
        tryVerify(() => sizedAt(1000), 10000, "the custom interval sized again")
        verify(/ rows?, /.test(textOf("exportRows_custom")), "a slower interval still saves: "
               + textOf("exportRows_custom"))
        // No row at all (a range holding no sample) has no rate to be slower than
        panel.sizes = [{ rowMs: 0, rows: 0, bytes: 80 }, { rowMs: 100, rows: 0, bytes: 80 }]
        compare(panel.sizeOf(100), "0 rows, 80 B", "no rows is a size, not the note")
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
        page.openPart.open({ title: "Frames", uri: "replay://x?profile=raw", source: "", folder: folder, profile: "raw" })
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
        page.decodePart.decodeWith("obd2")
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

    // A report that can't start (here no recording is open) says so, in
    // place of an earlier note
    function test_aReportThatCantStartSaysSo() {
        const page = createPage()
        page.reportNote = "Report saved: 1 ECU, 0 trouble codes, 0 key values"
        const url = Harness.writeScratchFile("never.json", "")
        verify(!page.reportPart.saveReportTo(url, 1), "nothing to report on")
        compare(page.reportNote, "Report not saved: the report couldn't start")
        verify(page.reportFailed, "in the failure's color")
    }

    // Save report: the whole recording's (the zoom never narrows it), on
    // the view's thread, the picked vehicle's baseline named, the note and
    // one log line when it ends
    function test_saveReportWritesTheWholeRecording() {
        const page = openFindings()
        const button = find(page, "saveReport")
        verify(button.visible)
        verify(button.enabled)
        page.cursorsPart.zoomAt(5000, 0.25)
        verify(page.viewT1 - page.viewT0 < 20000, "zoomed in")
        const url = Harness.writeScratchFile("findings.json", "")
        verify(url.toString() !== "")
        page.vehicles = vehicleBook(startEarlyRow.folder)
        verify(page.reportPart.saveReportTo(url, 1))
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
        page.openPart.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the next opens")
        compare(note.text, "")
    }
}
