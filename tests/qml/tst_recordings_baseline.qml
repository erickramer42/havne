// File/layer: tests/qml/tst_recordings_baseline.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings_baseline (test_qml)

import QtQuick
import QtTest
import App
import AppTest

RecordingsKit {
    id: testCase
    name: "Recordings"

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
        page.cursorsPart.setView(0, 2000)
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

    readonly property var conditionsRow: ({
        title: "2026-11-13 09:00:00 · Conditions", uri: "replay://x?profile=obd2",
        folder: Harness.recordingFixtures + "/conditions", source: "", profile: "obd2" })

    // Opens the conditions fixture and its Conditions panel
    function openConditions() {
        const page = createPage()
        page.recordings = [conditionsRow]
        openRow(page, conditionsRow)
        const button = find(page, "jumpConditions")
        verify(button.visible, "a recording with conditions offers the panel")
        button.clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        return page
    }

    // The panel lists each entry by its start, in the viewer's words, and is
    // one of two with Stretches
    function test_theConditionsPanelListsTheRecordingsConditions() {
        const page = openConditions()
        verify(page.conditionsOpen)
        verify(find(page, "conditionsPanel").visible, "the panel is up")
        verify(find(page, "jumpConditions").checked, "its switch shows it open")
        verify(find(page, "chartFrame").visible, "the desktop keeps its chart")
        verify(!find(page, "conditionsCut").visible, "every entry is listed")
        const list = find(page, "conditionList")
        compare(list.count, 3)
        tryVerify(() => list.itemAtIndex(2) !== null, 2000, "the rows have delegates")
        const row = i => list.itemAtIndex(i)
        compare(find(row(0), "conditionTime").text, "7.3 s")
        compare(find(row(0), "conditionName").text, "Coolant high")
        compare(find(row(0), "conditionLevel").text, "warning")
        compare(find(row(0), "conditionLength").text, "ON for 10 s")
        compare(find(row(1), "conditionName").text, "Engine speed high")
        verify(!find(row(1), "conditionLevel").visible, "no alert, no level")
        compare(find(row(1), "conditionLength").text, "ON for 0.5 s")
        compare(find(row(2), "conditionName").text, "Engine speed high")
        compare(find(row(2), "conditionLength").text, "no new data for 3.1 s, to the end")

        // Drawn none, then warning, so the alert's level lies on top
        compare(page.conditionsByLevel.map(s => s.key + (s.on ? ":on" : ":off")),
                ["rpm_high:off", "rpm_high:on", "coolant_high:on"])

        // Export takes the chart's room, the panel's with it, and gives it back
        page.reportPart.openExport()
        verify(!find(page, "conditionsPanel").visible, "hidden under Export")
        page.reportPart.closeExport()
        verify(find(page, "conditionsPanel").visible, "back once Export closes")

        find(page, "jumpStretches").clicked()
        verify(page.stretchesOpen)
        verify(!page.conditionsOpen, "Stretches takes the panel's place")
        find(page, "jumpConditions").clicked()
        verify(page.conditionsOpen)
        verify(!page.stretchesOpen, "and Conditions takes it back")
        find(page, "conditionsClose").clicked()
        verify(!page.conditionsOpen)
    }

    // A row centers the view on its entry and puts the cursor at its start;
    // the desktop's panel stays up
    function test_aConditionsRowCentersTheView() {
        const page = openConditions()
        page.cursorsPart.setView(0, 2000)
        const list = find(page, "conditionList")
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "the row has a delegate")
        mouseClick(find(list.itemAtIndex(0), "conditionRowArea"))
        // Coolant high: 7.251 s to 17.501 s
        compare(page.viewT0, 11376)
        compare(page.viewT1, 13376)
        compare(page.cursorMs, 7251)
        compare(page.conditionPick, 0)
        verify(page.conditionsOpen, "the desktop's panel stays up")
        // A new recording drops the pick and the panel
        page.openPart.backToList()
        compare(page.conditionPick, -1)
        verify(!page.conditionsOpen)
    }

    // An alert's ON spans draw on the lower row in their level's role, the
    // rest 6 px above: a span with no alert inside an alert's keeps pixels of
    // its own. A context that records each rectangle stands in for the chart's
    function test_aSpanWithNoAlertKeepsItsOwnRow() {
        const page = createPage()
        page.recordings = [conditionsRow]
        openRow(page, conditionsRow)
        const ctx = {
            fillStyle: "", rects: [],
            fillRect(x, y, w, h) { this.rects.push({ x: x, y: y, w: w, h: h, style: String(this.fillStyle) }) }
        }
        // 100 ms to a pixel, the lower row's top at 100
        page.drawConditions(ctx, 100, 0, 400, t => t / 100, page.conditionsByLevel)
        const at = (x, y) => ctx.rects.filter(r => r.y === y && r.x <= x && x < r.x + r.w)
        // Engine speed high (no alert) ON from 12.876 s to 13.376 s, wholly
        // inside Coolant high's warning (7.251 s to 17.501 s)
        compare(at(130, 94).length, 1, "the span with no alert on the upper row")
        compare(at(130, 94)[0].style, String(Theme.textDim))
        compare(at(130, 100).length, 1, "the warning on the lower row")
        compare(at(130, 100)[0].style, String(Theme.alertWarnFill))
        compare(at(100, 94).length, 0, "the upper row empty outside its span")
        // The stretch with no new data, dashed on the upper row from 26.876 s
        verify(at(270, 94).length === 1 && at(273, 94).length === 0, "dashed 3 on, 3 off")
        verify(ctx.rects.every(r => r.h === 4 && (r.y === 94 || r.y === 100)), "two 4 px rows")
    }

    // An ON span's stale parts are dashed and the rest of it solid, all on
    // its row in its level's role
    function test_anOnSpansStalePartsAreDashed() {
        const page = createPage()
        page.recordings = [conditionsRow]
        openRow(page, conditionsRow)
        const ctx = {
            fillStyle: "", rects: [],
            fillRect(x, y, w, h) { this.rects.push({ x: x, y: y, w: w, h: h, style: String(this.fillStyle) }) }
        }
        page.drawConditions(ctx, 100, 0, 400, t => t / 100, [{
            key: "boost", name: "Boost", level: 2, on: true, fromMs: 1000, toMs: 9000,
            heldStart: false, openEnd: false, stale: [{ fromMs: 3000, toMs: 5000 }] }])
        const at = x => ctx.rects.filter(r => r.x <= x && x < r.x + r.w)
        verify(ctx.rects.every(r => r.y === 100 && r.style === String(Theme.alertDangerFill)),
               "the danger row, in danger's role")
        compare(at(20).length, 1, "solid before the part")
        compare(at(29.9).length, 1)
        compare(at(31).length, 1, "a dash")
        compare(at(34).length, 0, "a gap")
        compare(at(37).length, 1, "the next dash")
        compare(at(60).length, 1, "solid after the part")
        compare(at(95).length, 0, "nothing past the span")
    }

    // The chart's paint draws the findings and markers from the page's
    // plain copies, never the view's lists (the bridge map rule): what a
    // test plants in the copies is what a paint draws
    function test_theChartPaintsThePagesCopiesOfFindingsAndMarkers() {
        const page = createPage()
        page.recordings = [conditionsRow]
        openRow(page, conditionsRow)
        verify(Array.isArray(page.chartFindings) && Array.isArray(page.chartMarkers), "plain arrays")
        page.chartFindings = [{ kind: 1, atMs: 1000, lengthMs: 500 }]
        page.chartMarkers = [{ timeMs: 2000, label: "Planted" }]
        page.cursorsPart.setView(0, 4000)
        const ctx = {
            fillStyle: "", strokeStyle: "", lineWidth: 1, textAlign: "", textBaseline: "",
            rects: [], texts: [],
            fillRect(x, y, w, h) { this.rects.push({ x: x, y: y, w: w, h: h, style: String(this.fillStyle) }) },
            fillText(t, x, y) { this.texts.push(t) },
            setLineDash() {}, beginPath() {}, moveTo() {}, lineTo() {}, stroke() {}, arc() {}, fill() {}
        }
        // 10 ms to a pixel, the plot's foot at 100
        find(page, "chart").decorate(ctx, { left: 0, top: 0, plotW: 401, plotH: 100, traces: [] })
        compare(ctx.texts, ["Planted"], "the planted marker alone")
        const foot = ctx.rects.filter(r => r.y === 96 && r.h === 4)
        compare(foot.length, 1, "the planted finding alone, at the plot's foot")
        verify(foot[0].x === 100 && foot[0].w === 50, JSON.stringify(foot[0]))
        compare(foot[0].style, String(Theme.danger))
    }

    // The panel says when a span's stretches with no new data past the bound
    // are drawn as one, beside the entries' own cut
    function test_theCutLineNamesEachCut() {
        const page = openConditions()
        const panel = find(page, "conditionsPanel")
        const line = find(page, "conditionsCut")
        panel.info = { conditionPartsCut: true, conditionBound: 4096 }
        compare(line.text, "Past 4096 stretches with no new data, a span's later ones are drawn as one to its end")
        panel.info = { conditionSpansCut: true, conditionSpanCount: 5000, conditionPartsCut: true, conditionBound: 4096 }
        compare(line.text, "Showing the first 3 of 5000. Past 4096 stretches with no new data, "
                + "a span's later ones are drawn as one to its end")
        panel.info = { conditionsCut: true }
        compare(line.text, "Showing the first 3: the recording holds more condition events than the viewer reads")
        const lost = " lost while recording: an entry may be missing, or run on past its end"
        panel.info = { conditionEventsLost: 3 }
        verify(line.visible, "a loss alone shows the line")
        compare(line.text, "3 condition events were" + lost)
        panel.info = { conditionEventsLost: 1 }
        compare(line.text, "1 condition event was" + lost)
        panel.info = { conditionsCut: true, conditionEventsLost: 2 }
        compare(line.text, "Showing the first 3: the recording holds more condition events than the viewer reads. "
                + "2 condition events were" + lost)
        panel.info = { conditionEventsLost: 0 }
        verify(!line.visible, "no loss, no line")
    }

    // A recording whose every condition event was lost has no entry to list,
    // and its panel still says the loss
    function test_aRecordingWhoseConditionEventsAreAllLostSaysSo() {
        const row = { title: "2026-11-13 09:00:00 · All lost", uri: "replay://x?profile=obd2",
                      folder: Harness.recordingFixtures + "/conditions-all-lost", source: "", profile: "obd2" }
        const page = createPage()
        page.recordings = [row]
        openRow(page, row)
        compare(page.conditionList.length, 0, "no entry survived")
        const button = find(page, "jumpConditions")
        verify(button.visible, "the loss is offered")
        button.clicked()
        verify(waitForPolish(page.Window.window), "laid out")
        const line = find(page, "conditionsCut")
        verify(line.visible, "the panel says the loss")
        verify(line.text.indexOf("3 condition events were lost") === 0, line.text)
    }

    // A recording with no conditions offers no panel
    function test_aRecordingWithoutConditionsHasNoConditionsButton() {
        const page = openFindings()
        verify(!find(page, "jumpConditions").visible)
        compare(page.conditionsByLevel.length, 0)
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
        page.cursorsPart.setView(5000, 6000)
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
        page.openPart.open(earlyRow)
        tryVerify(() => find(page, "signalRows").visible, 10000, "the recording opens again")
        compare(find(page, "signalRows").count, all, "back to the list cleared the filter: text '"
                + bar.filter.text + "', plotted only " + bar.filter.plottedOnly
                + ", groups [" + bar.filter.groups + "]")
    }

    function test_aRefusedFolderSaysWhyAndBackGoesToTheList() {
        const page = createPage()
        page.openPart.open({ title: "Gone", folder: Harness.recordingFixtures + "/no-such-recording" })
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
        page.openPart.open({ title: "Lacks", uri: "replay://x?profile=test", source: "", folder: first,
                    profile: "test" })
        tryVerify(() => find(page, "exportButton").visible, 10000, "the viewer opens")
        const quiet = page.shownSignals.find(s => s.name === "Quiet")
        const rpm = page.shownSignals.find(s => s.name === "RPM")
        page.cursorsPart.togglePlotted(rpm.index)
        page.cursorsPart.togglePlotted(quiet.index)
        compare(page.plottedNoValues, ["Quiet"], "alone, it has nothing to draw")
        compare(page.seriesPart.series(400).length, 0)
        page.comparePart.compareWith({ title: "Has", folder: second })
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        compare(page.plottedNoValues, [], "the compared recording draws it")
        const all = page.seriesPart.series(400)
        compare(all.length, 1, "its dashed trace alone")
        verify(all[0].dash !== undefined, "dashed")
        compare(all[0].row, quiet.index)
        page.comparePart.stopComparing()
        compare(page.plottedNoValues, ["Quiet"], "alone again")
    }

    // A ticked signal with no values in the recording: the chart says so
    // instead of staying blank
    function test_aPlottedSignalWithNoValuesIsSaidInTheChart() {
        const page = createPage()
        compare(page.reportPart.noValuesText(["RPM"]), "RPM has no values in this recording")
        compare(page.reportPart.noValuesText(["RPM", "Speed"]), "RPM and Speed have no values in this recording")
        compare(page.reportPart.noValuesText(["RPM", "Speed", "Load"]),
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
        page.openPart.open({ title: "Quiet", uri: "replay://x?profile=test", source: "", folder: folder,
                    profile: "test" })
        tryVerify(() => find(page, "exportButton").visible, 10000, "the viewer opens")
        const chart = find(page, "chart")
        const empty = page.shownSignals.find(s => s.name === "Quiet")
        verify(empty !== undefined && empty.samples === 0, "a listed signal with no rows")
        verify(page.plotted[empty.index] !== true, "not plotted on its own")
        // RPM is plotted by default and draws, so the chart says nothing
        // about Quiet until RPM is unticked: the words are for a chart
        // with nothing to draw
        page.cursorsPart.togglePlotted(empty.index)
        compare(page.plottedNoValues, [], "RPM still draws")
        const rpm = page.shownSignals.find(s => s.name === "RPM")
        page.cursorsPart.togglePlotted(rpm.index)
        tryCompare(chart, "emptyText", "Quiet has no values in this recording", 3000)
        compare(page.seriesPart.series(400).length, 0, "nothing to draw, so the words show")
        page.cursorsPart.togglePlotted(empty.index)
        compare(chart.emptyText, "No signals plotted; tick a row to plot it")
    }
}
