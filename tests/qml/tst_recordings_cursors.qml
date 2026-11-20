// File/layer: tests/qml/tst_recordings_cursors.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings_cursors (test_qml)

import QtQuick
import QtTest
import App
import AppTest

RecordingsKit {
    id: testCase
    name: "Recordings"

    // Open shows the recording: its signal plotted by default, the chart's
    // series over the whole recording, the check's findings listed
    function test_openShowsTheViewer() {
        const page = openFindings()
        compare(find(page, "openedTitle").text, findingsRow.title)
        compare(find(page, "signalRows").count, 1)
        verify(page.plotted[0], "RPM, ranked, is plotted")
        compare(page.viewT0, 0)
        compare(page.viewT1, 20000)
        const series = page.seriesPart.series(400)
        compare(series.length, 1)
        compare(series[0].row, 0)
        verify(series[0].columns.lows.length > 0)
        compare(find(page, "checkSummary").text, "Frames check: 8 findings")
        compare(find(page, "findingList").count, 8)
        verify(find(page, "recordingInfo").text.indexOf("20 s long") === 0,
               find(page, "recordingInfo").text)

        // Unticked, nothing is plotted
        page.cursorsPart.togglePlotted(0)
        compare(page.seriesPart.series(400).length, 0)
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
        verify(page.seriesPart.series(400).length === 1, "RPM is plotted")
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
        const before = page.seriesPart.series(400)[0].columns
        const colMs = (before.t1 - before.t0) / before.lows.length
        verify(colMs > 0 && Math.abs(before.t0 / colMs - Math.round(before.t0 / colMs)) < 1e-6,
               "the grid starts on a whole column: " + before.t0)
        page.viewT0 += colMs * 0.4
        page.viewT1 += colMs * 0.4
        const after = page.seriesPart.series(400)[0].columns
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
        page.openPart.open({ title: "Pull", folder: Harness.recordingFixtures + "/pull-early" })
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
        compare(page.seriesPart.series(400).length, 3)
    }

    // The cursor reads each signal's value in force at its time
    function test_theCursorReadsTheValuesAtItsTime() {
        const page = openFindings()
        compare(find(page, "cursorTime").text, "Click for values · drag to pan · wheel to zoom · double-click a pane to enlarge it")
        const row = find(page, "signalRows").itemAtIndex(0)
        compare(find(row, "cursorValue").text, "—")
        page.cursorsPart.setCursor(5000)
        compare(find(page, "cursorTime").text, "At 5.0 s")
        const at = page.readout[0]
        verify(at.atMs >= 0 && at.atMs <= 5000, "the sample in force: " + at.atMs)
        compare(find(row, "cursorValue").text, page.formatPart.valueText(Number(at.text), 0, "rpm"))
        compare(page.seriesPart.series(400)[0].labelValue, Number(at.text), "the label shows it too")
    }

    // A tap lands on the first plotted signal's sample nearest it, so its
    // value is the one the line has there; each trace's label carries its
    // value's sample time, where the chart puts a dot. With nothing
    // plotted a tap stays where it fell
    // An enum signal's cursor value is its label, not its raw number
    function test_theCursorReadsAnEnumsLabel() {
        const page = createPage()
        page.openPart.open({ title: "Fuel", folder: Harness.recordingFixtures + "/fuel-status" })
        const rows = find(page, "signalRows")
        tryVerify(() => rows.visible && rows.count === 2, 10000, "the recording opens")
        let row = null
        for (let i = 0; i < 2 && row === null; ++i) {
            tryVerify(() => rows.itemAtIndex(i) !== null, 2000, "row " + i + " has a delegate")
            if (rows.itemAtIndex(i).modelData.name === "Fuel System 1")
                row = rows.itemAtIndex(i)
        }
        verify(row !== null, "the enum's row")
        page.cursorsPart.setCursor(1000)
        compare(find(row, "cursorValue").text, "Open loop, cold")
        page.cursorsPart.setCursor(4200)
        compare(find(row, "cursorValue").text, "Closed loop")
    }

    function test_aTapSnapsToTheNearestSample() {
        const page = openEarly()
        const sigs = find(page, "signalRows").model
        const rpm = sigs.find(s => s.name === "RPM").index
        const throttle = sigs.find(s => s.name === "Throttle").index
        page.plotted = ({ [rpm]: true, [throttle]: true })
        // RPM every 100 ms from 100 ms, Throttle 1 ms after each
        compare(page.cursorsPart.snapped(1140), 1100)
        compare(page.cursorsPart.snapped(1160), 1200)
        page.cursorsPart.setView(0, 4000)
        page.cursorsPart.setCursor(page.cursorsPart.snapped(1160))
        compare(page.readout[rpm].atMs, 1200)
        const labelMs = row => page.seriesPart.series(400).find(s => s.row === row).labelMs
        compare(labelMs(rpm), 1200, "RPM's dot on its sample, at the cursor")
        compare(labelMs(throttle), 1101, "Throttle's on its own, 99 ms behind")
        page.cursorsPart.setView(2000, 4000)
        verify(isNaN(labelMs(rpm)), "no dot for a cursor out of view")
        page.plotted = ({})
        compare(page.cursorsPart.snapped(1160), 1160)
    }

    // Maximized on a wide page, the cursor's values sit in a strip beside
    // the chart; its button folds it to a tab, giving the plot the width,
    // and opens it again, and the choice is kept
    function test_theCursorsStripFoldsAwayAndBack() {
        const page = openFindings()
        page.height = 600
        page.stripFolded = false
        page.refreshPart.toggleMaximized()
        verify(page.chartMaximized, "maximized")
        page.cursorsPart.setCursor(5000)
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
        page.refreshPart.toggleMaximized()
        page.height = 900
    }

    // On a tall page (a phone upright) the values go below the chart in a
    // band, so the plot keeps the width; the fold gives the height back
    function test_theCursorsStripGoesBelowTheChartOnATallPage() {
        const page = openFindings()
        page.width = 400
        page.stripFolded = false
        page.refreshPart.toggleMaximized()
        page.cursorsPart.setCursor(5000)
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
        page.refreshPart.toggleMaximized()
        page.width = 900
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
        page.comparePart.compareWith(lateRow)
        tryVerify(() => page.compareAligned && chart.drawn
                        && chart.drawn.traces.some(t => t.dash !== undefined), 10000, "the compare drawn")
        for (const tr of chart.drawn.traces.filter(t => t.dash !== undefined))
            compare(chart.drawn.panes[tr.pane].label, rows[tr.row].unit, "a compared trace in its namesake's pane")
    }

    // ---- Two cursors and the span's statistics

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
        const labelOf = () => page.seriesPart.series(400).filter(t => t.row === rpm.index)[0]
        spanned(page, 1000, 3000)
        compare(labelOf().labelValue, Number(page.readout[rpm.index].text), "A is the earlier")
        page.cursorsPart.setCursor(5000)
        verify(page.cursorMs > page.cursorBMs, "A is past B")
        const earlier = Number(page.readoutB[rpm.index].text)
        verify(earlier !== Number(page.readout[rpm.index].text), "the two cursors read differently")
        compare(find(rpm.item, "cursorValue").text, page.formatPart.valueText(earlier, 0, "rpm"),
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
        compare(find(rpm.item, "cursorValue").text, grouped(2001) + " rpm")
        tryCompare(find(rpm.item, "cursorBValue"), "text", grouped(1982) + " rpm")
        tryCompare(find(rpm.item, "cursorDelta"), "text", "-19 rpm")

        const table = find(page, "spanStats")
        tryVerify(() => table.visible, 2000, "the table shows with a span")
        const row = find(table, "statRow_" + rpm.index)
        compare(find(row, "statChange").text, "-19 rpm")
        compare(find(row, "statLow").text, grouped(1982) + " rpm")
        compare(find(row, "statHigh").text, grouped(2012) + " rpm")
        compare(find(row, "statMean").text, grouped(2003) + " rpm")
        compare(find(row, "statRate").text, decimal(-9.5, 1) + " rpm/s")
        // A rising signal's change and rate carry their sign
        const up = page.textsPart.changeText(12.5, 1, "kPa")
        compare(up, "+" + decimal(12.5, 1) + " kPa")

        // Text has B and no change; a row not plotted has both
        const vin = rowOf(page, "VIN")
        compare(find(vin.item, "cursorDelta").text, "")
        compare(findChild(table, "statRow_" + vin.index), null, "text isn't in the table")
        page.cursorsPart.togglePlotted(rpm.index)
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
        compare(find(rpm.item, "cursorValue").text, grouped(2001) + " rpm")
        compare(find(rpm.item, "cursorBValue").text, grouped(1982) + " rpm")
        tryCompare(find(rpm.item, "cursorDelta"), "text", "-19 rpm")
    }

    // A second cursor on the first is no span: no line, no statistics
    function test_twoCursorsTogetherAreNoSpan() {
        const page = openEarly()
        spanned(page, 2000, 3000)
        page.cursorsPart.setCursor(3000)
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
        page.cursorsPart.askSpanStats()
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

            page.cursorsPart.setView(5000, 25000)
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
            page.cursorsPart.setCursor(5000)
            mousePress(frame, x, y)
            wait(200)
            mouseRelease(frame, x, y)
            verify(!page.hasCursorB, "the desktop's long press is nothing")

            Theme.compact = true
            verify(waitForPolish(page.Window.window), "laid out")
            const px = atTime(page, chart, 15000)
            const py = inPlot(chart, 0).y
            mousePress(frame, px, py)
            tryVerify(() => page.hasCursorB, 2000, "the hold places it")
            verify(Math.abs(page.cursorBMs - 15000) < 300, "where it was held: " + page.cursorBMs)
            compare(page.cursorMs, 5000, "the first stays")
            mouseRelease(frame, px, py)

            page.cursorsPart.clearCursorB()
            const view = [page.viewT0, page.viewT1]
            const to = atTime(page, chart, 25000)
            mousePress(frame, px, py)
            tryVerify(() => tap.held, 2000, "the hold is on")
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
        page.cursorsPart.setCursor(1000)
        keyClick(Qt.Key_Right)
        tryCompare(page, "cursorMs", 1100, 2000, "a sample on")
        keyClick(Qt.Key_Left)
        tryCompare(page, "cursorMs", 1000, 2000, "a sample back")

        keyClick("b")
        verify(!page.arrowsMoveB, "with no second cursor B does nothing")
        page.cursorsPart.setCursorB(3000)
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

        page.cursorsPart.setView(10000, 12000)
        page.cursorsPart.setCursor(11900)
        // A step reads the cursor's time when the key is pressed and lands
        // later: the second key waits for the first's step, or both read
        // 11900 and neither goes off the view
        keyClick(Qt.Key_Right)
        tryCompare(page, "cursorMs", 12000, 2000, "the last sample in view")
        keyClick(Qt.Key_Right)
        tryVerify(() => page.cursorMs > 12000 && page.cursorMs <= page.viewT1 && page.viewT0 > 10000,
                  2000, "a step off the view brings it back")
        page.cursorsPart.clearCursorB()
    }

    // The compared recording's value is read where the rows read this
    // one's: at the earlier of two cursors
    function test_theComparedValueFollowsTheEarlierCursor() {
        const page = openEarly()
        page.comparePart.compareWith(lateRow)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        page.comparePart.setAlignOn("rise")
        page.alignHysteresis = 500
        page.alignLevel = 4000
        verify(page.compareAligned)
        const rpm = find(page, "signalRows").model.find(s => s.name === "RPM").index
        const atTen = page.comparePart.compareValuesAt(10000)[rpm].atMs
        const atTwenty = page.comparePart.compareValuesAt(20000)[rpm].atMs
        verify(atTen !== atTwenty, "the two times read different samples there")
        spanned(page, 20000, 10000)
        compare(page.compareReadout[rpm].atMs, atTen, "the earlier cursor's sample, not the first's")
        // The dashed label's dot is at the earlier cursor too, shown while
        // that one is in view, whatever the first cursor does
        page.cursorsPart.setView(5000, 15000)
        const dashed = page.seriesPart.series(400).filter(s => s.dash !== undefined && s.row === rpm)
        compare(dashed.length, 1, "the compared RPM is drawn")
        compare(dashed[0].labelMs, atTen + page.compareOffsetMs, "its label is at the earlier cursor")
        page.cursorsPart.showAll()
        page.cursorsPart.setCursorB(30000)
        compare(page.compareReadout[rpm].atMs, atTwenty, "the first is the earlier again")
        page.cursorsPart.setCursorB(10000)
        page.cursorsPart.clearCursorB()
        compare(page.compareReadout[rpm].atMs, atTwenty, "one cursor: the first's")
    }

    // A handle dragged past the plot's edge stays in the view and ends on
    // a sample
    function test_aHandleDraggedPastTheEdgeStaysInView() {
        const page = openEarly()
        const restore = growWindow(page)
        let pressed = false
        try {
            const chart = panedChart(page)
            // an end off the sample grid: the nearest sample to it lies past it
            page.cursorsPart.setView(10000, 19960)
            verify(waitForPolish(page.Window.window), "laid out")
            page.cursorsPart.setCursor(12000)
            page.cursorsPart.setCursorB(15000)
            const frame = find(page, "chartFrame")
            const handle = find(page, "cursorHandleB")
            const grab = find(handle, "handleDrag")
            const from = grab.mapToItem(frame, grab.width / 2, grab.height / 2)
            mousePress(frame, from.x, from.y)
            pressed = true
            const edge = frame.width - 2
            for (let k = 1; k <= 8; ++k)
                mouseMove(frame, from.x + (edge - from.x) * k / 8, from.y, 10)
            verify(page.cursorBMs <= page.viewT1 + 1e-6, "inside the view: " + page.cursorBMs)
            verify(handle.visible, "the handle is still shown")
            mouseRelease(frame, edge, from.y)
            pressed = false
            verify(page.cursorBMs <= page.viewT1 + 1e-6, "ends inside the view: " + page.cursorBMs)
            verify(handle.visible, "and the drop leaves it shown")
            compare(page.cursorBMs, page.cursorsPart.snapped(page.cursorBMs), "on a sample")
        } finally {
            if (pressed)
                mouseRelease(find(page, "chartFrame"), 0, 0)   // a failed check leaves no button held
            restore()
        }
    }

    // A time is rounded to tenths as a whole: never 1:60.0
    function test_aTimeNeverReadsSixtySeconds() {
        const page = openEarly()
        compare(page.formatPart.timeText(119960, true), "2:00.0")
        compare(page.formatPart.timeText(59960, true), "1:00.0")
        compare(page.formatPart.timeText(59940, true), "59.9 s")
        compare(page.formatPart.timeText(3599960, true), "1:00:00.0")
        compare(page.formatPart.timeText(-59960, true), "-1:00.0")
        compare(page.formatPart.timeText(12300, true), "12.3 s")
        compare(page.formatPart.timeText(62300, true), "1:02.3")
    }
}
