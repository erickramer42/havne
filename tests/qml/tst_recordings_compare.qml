// File/layer: tests/qml/tst_recordings_compare.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings_compare (test_qml)

import QtQuick
import QtTest
import App
import AppTest

RecordingsKit {
    id: testCase
    name: "Recordings"

    // A values recording of `count` number signals, each sampled ten times
    // a second for a minute (600 rows a signal), and its row
    function denseRecording(name, count) {
        const folder = Harness.writeFramesRecording(name, 1, 10000)
        verify(folder !== "", "the folder is made")
        let header = "ecuio session recording, format 2 (ADR-0049)\n"
                     + "started  2026-11-03 09:00:00.000 (local time)\n"
                     + "profile  test\n"
        for (let s = 1; s <= count; ++s)
            header += "signal  Signal " + s + ",number,V,0,0\n"
        header += "\n00:01:00.000  +60.000  end  test: 0 frames, " + (600 * count)
                  + " values, 0 records dropped, 0 notes dropped\n"
        const rows = ["host_us,signal,value,unit"]
        for (let t = 1; t <= 600; ++t) {
            for (let s = 1; s <= count; ++s)
                rows.push((t * 100000) + ",Signal " + s + "," + ((t + s) % 50) + ",V")
        }
        verify(Harness.writeScratchFile(name + "/events.log", header).toString() !== "")
        verify(Harness.writeScratchFile(name + "/signals.csv", rows.join("\n") + "\n").toString() !== "")
        return { title: name, uri: "replay://x?profile=test", source: "", folder: folder,
                 profile: "test" }
    }

    // Columns stay narrower than a second while the rows a frame draws (the
    // plotted signals', both recordings' together) fit rawRowsPerFrame,
    // whatever the signals not plotted add, and go a second wide past it or
    // once either recording's rows in view outgrow the reader's cache
    function test_columnsGoASecondWideByTheRowsAFrameDraws() {
        const one = denseRecording("dense-one", 40)
        const two = denseRecording("dense-two", 40)
        const page = createPage()
        page.recordings = [two, one]
        openRow(page, one)
        const first = n => {
            const picks = {}
            for (let i = 0; i < n; ++i)
                picks[i] = true
            return picks
        }
        const columnMs = () => {
            const c = page.seriesPart.series(400)[0].columns
            return (c.t1 - c.t0) / c.lows.length
        }
        const spanS = (page.viewT1 - page.viewT0) / 1000
        verify(spanS * page.rowsPerSecond > page.rawRowsPerFrame,
               "every signal's rows in view pass the bound: " + spanS * page.rowsPerSecond)
        page.plotted = first(2)
        verify(columnMs() < 1000, "two of forty plotted, 1,200 rows: " + columnMs() + " ms a column")
        page.plotted = first(40)
        verify(columnMs() >= 1000, "all forty, 24,000 rows: " + columnMs() + " ms a column")

        // The cache's bound counts every signal's rows, plotted or not
        page.plotted = first(2)
        page.rawRowsCached = 20000
        verify(columnMs() >= 1000, "24,000 rows held against 20,000: " + columnMs() + " ms a column")
        page.rawRowsCached = 30000
        verify(columnMs() < 1000, "and against 30,000: " + columnMs() + " ms a column")

        // Compared, the rows drawn are both recordings'; the rows held are
        // each reader's own
        page.comparePart.compareWith(two)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        compare(page.seriesPart.series(400).filter(s => s.dash !== undefined).length, 2, "both are drawn dashed")
        verify(columnMs() < 1000, "24,000 rows in each reader, 2,400 drawn: " + columnMs() + " ms a column")
        page.plotted = first(20)
        verify(columnMs() >= 1000, "twenty of each, 24,000 rows drawn: " + columnMs() + " ms a column")
        page.comparePart.stopComparing()
        verify(columnMs() < 1000, "twenty alone, 12,000: " + columnMs() + " ms a column")
    }

    // A comparison's difference is the compared recording's mean less this
    // one's over one stretch of the axis in both. A recording against
    // itself half a second later, aligned, shows none, in columns of a
    // second or more (where the traces drawn hold each recording's own
    // whole seconds, half a second apart) and in narrower ones
    function test_aRecordingAgainstItselfHalfASecondLaterShowsNoDifference() {
        const here = swingRecording("swing", 0, 0, "rpm")
        const later = swingRecording("swing-later", 500, 0, "rpm")
        const higher = swingRecording("swing-higher", 0, 100, "rpm")
        const other = swingRecording("swing-other-unit", 500, 0, "1/min")
        const page = createPage()
        page.recordings = [other, higher, later, here]
        openRow(page, here)
        const rpm = page.shownSignals.find(s => s.name === "RPM").index
        const picks = {}
        picks[rpm] = true
        page.plotted = picks
        const box = find(page, "compareDifference")
        // The difference of the grid the chart draws, once it is read
        const differenceTrace = () => {
            let trace
            tryVerify(() => {
                trace = page.seriesPart.series(400).find(s => s.pane !== undefined)
                return trace !== undefined && page.difference !== null
                       && page.difference.key === page.differenceWanted.key
            }, 10000, "the view's difference is read")
            return trace
        }
        const columnMs = trace => (trace.columns.t1 - trace.columns.t0) / trace.columns.lows.length
        // The columns holding a difference, and how far the farthest is from `from`
        const judged = (trace, from) => {
            let count = 0, most = 0
            for (let c = 0; c < trace.columns.lows.length; ++c) {
                const d = trace.columns.lows[c]
                if (!isFinite(d))
                    continue
                ++count
                most = Math.max(most, Math.abs(d - from))
            }
            return { count: count, most: most }
        }

        page.comparePart.compareWith(later)
        verify(!box.enabled, "nothing to subtract until the two are lined up")
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        page.comparePart.setAlignOn("rise")
        page.alignHysteresis = 200
        page.alignLevel = 3500
        verify(page.compareAligned, page.compareStatus)
        compare(page.compareOffsetMs, -500, "the copy lies over the recording")
        verify(box.enabled && !box.checked && !page.differenceShown, "off until asked for")
        compare(page.seriesPart.series(400).filter(s => s.pane !== undefined).length, 0)
        box.toggled()
        verify(page.differenceShown && box.checked)

        // The whole ten minutes: columns of more than a second
        page.cursorsPart.showAll()
        let trace = differenceTrace()
        verify(columnMs(trace) >= 1000, columnMs(trace) + " ms a column")
        compare(trace.paneLabel, "\u0394 rpm")
        compare(trace.row, rpm)
        let found = judged(trace, 0)
        verify(found.count > 100, "columns holding both: " + found.count)
        verify(found.most < 1e-6, "a difference of " + found.most + " between the same samples")
        // What the chart draws there can't give it: the dashed trace holds
        // the copy's own whole seconds, half a second from this one's
        const all = page.seriesPart.series(400)
        const mine = all.find(s => s.dash === undefined && s.pane === undefined).columns
        const dashed = all.find(s => s.dash !== undefined).columns
        let apart = 0
        for (let c = 0; c < mine.lows.length; ++c) {
            const d = (dashed.lows[c] + dashed.highs[c]) / 2 - (mine.lows[c] + mine.highs[c]) / 2
            if (isFinite(d))
                apart = Math.max(apart, Math.abs(d))
        }
        verify(apart > 1, "the drawn columns differ by " + apart)

        // Zoomed in: columns under a second
        page.cursorsPart.setView(100000, 120000)
        trace = differenceTrace()
        verify(columnMs(trace) < 1000, columnMs(trace) + " ms a column")
        found = judged(trace, 0)
        verify(found.count > 20, "columns holding both: " + found.count)
        verify(found.most < 1e-6, "a difference of " + found.most + " between the same samples")

        // Switched off, the answer isn't drawn; on again, the one it held is
        const panes = () => page.seriesPart.series(400).filter(s => s.pane !== undefined).length
        box.toggled()
        verify(!page.differenceShown && !box.checked)
        compare(panes(), 0, "switched off")
        box.toggled()
        verify(page.differenceShown && box.checked)
        compare(panes(), 1, "switched on again")
        // Lined up another way, the answer for the offset before isn't drawn
        page.comparePart.setAlignOn("start")
        compare(page.compareOffsetMs, 0)
        compare(panes(), 0, "an answer for another offset")

        // A recording 100 rpm higher is 100 higher in every column: the
        // compared one less this one
        page.comparePart.compareWith(higher)
        compare(page.difference, null, "the recording before's difference goes with it")
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        page.comparePart.setAlignOn("start")
        compare(page.compareOffsetMs, 0)
        verify(page.differenceShown, "another recording keeps the choice")
        page.cursorsPart.showAll()
        trace = differenceTrace()
        found = judged(trace, 100)
        verify(found.count > 100, "columns holding both: " + found.count)
        verify(found.most < 1e-6, found.most + " from 100")

        // This recording opened again drops the answer too: its values can
        // be another decode's
        verify(page.difference !== null)
        page.openPart.open(here)
        compare(page.difference, null, "this recording's answer goes with it")
        tryVerify(() => find(page, "recordingInfo").visible, 10000, "it opens again")
        page.plotted = picks

        // The same name in another unit is nothing to subtract
        page.comparePart.compareWith(other)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        compare(page.seriesPart.differencePairs().length, 0)
        compare(page.seriesPart.series(400).filter(s => s.pane !== undefined).length, 0)
        compare(page.seriesPart.series(400).filter(s => s.dash !== undefined).length, 1, "still drawn dashed")

        find(page, "stopCompare").clicked()
        verify(!page.differenceShown && !box.checked, "Stop ends it")
        compare(page.difference, null)
    }

    function differencePanes(page) {
        return page.seriesPart.series(400).filter(s => s.pane !== undefined).length
    }

    // A view that refuses a difference's ask answers nothing, so nothing is
    // waited for: the grid is asked once more at a later paint and then
    // left. Another grid, with the view able again, is asked and answered
    function test_aRefusedDifferenceAskIsAskedOnceMoreThenLeft_data() {
        return [ { tag: "this recording", view: "recordingView" },
                 { tag: "the compared one", view: "compareView" } ]
    }

    function test_aRefusedDifferenceAskIsAskedOnceMoreThenLeft(data) {
        const page = swingCompared("refused-" + data.view)
        const view = find(page, data.view)
        verify(Harness.refuseViewAsks(view, true), "a RecordingView")
        find(page, "compareDifference").toggled()
        tryVerify(() => {
            page.seriesPart.series(400)
            return page.differenceRefused !== null && page.differenceRefused.count === 2
        }, 10000, "asked, and once more")
        const key = page.differenceRefused.key
        for (let k = 0; k < 10; ++k) {
            page.seriesPart.series(400)
            wait(20)
        }
        compare(page.differenceRefused.count, 2, "left after the second")
        compare(page.differenceRefused.key, key)
        compare(page.differenceAsk, null, "no answer is waited for")
        compare(page.difference, null)
        compare(differencePanes(page), 0)

        verify(Harness.refuseViewAsks(view, false))
        page.cursorsPart.setView(100000, 160000)
        differenceRead(page)
        compare(page.differenceRefused, null)
        compare(differencePanes(page), 1)
    }

    // An ask a view's task has no memory for is answered empty: the page
    // stops waiting, draws no difference for that grid and doesn't ask for
    // it again. Another grid, with memory again, is answered
    function test_aLostDifferenceAskIsAnsweredEmptyAndNotAskedAgain_data() {
        return [ { tag: "this recording", view: "recordingView" },
                 { tag: "the compared one", view: "compareView" } ]
    }

    function test_aLostDifferenceAskIsAnsweredEmptyAndNotAskedAgain(data) {
        const page = swingCompared("lost-" + data.view)
        const view = find(page, data.view)
        verify(Harness.loseViewMeans(view, true), "a RecordingView")
        find(page, "compareDifference").toggled()
        differenceRead(page)
        compare(Object.keys(page.difference.rows).length, 0, "no rows from a lost answer")
        compare(page.differenceAsk, null, "nothing is waited for")
        compare(differencePanes(page), 0)
        for (let k = 0; k < 5; ++k) {
            page.seriesPart.series(400)
            wait(20)
        }
        compare(page.differenceAsk, null, "the grid isn't asked again")
        compare(page.differenceRefused, null)

        verify(Harness.loseViewMeans(view, false))
        page.cursorsPart.setView(100000, 160000)
        differenceRead(page)
        compare(Object.keys(page.difference.rows).length, 1)
        compare(differencePanes(page), 1)
    }

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
        const all = page.seriesPart.series(400)
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
        compare(page.seriesPart.series(400).length, 3)
        compare(page.viewT1, 30000)
    }

    // Aligned on a rise, a marker or the UTC starts, where havne-cli view
    // lines the two pulls up (tests/golden/cli/view-rise.jsonl: 14.6 s and
    // 24.6 s; view-marker.jsonl; view-utc.jsonl: 90 s apart)
    function test_theAlignmentsAreHavneClisOwn() {
        const page = openEarly()
        page.comparePart.compareWith(lateRow)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        const status = find(page, "compareStatus")

        page.comparePart.setAlignOn("rise")
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
        page.cursorsPart.setCursor(14600)
        compare(page.compareReadout[rpm].text, page.readout[rpm].text)
        const row = find(page, "signalRows").itemAtIndex(rpm)
        compare(find(row, "compareValue").text, find(row, "cursorValue").text)
        page.cursorsPart.setCursor(-5000)
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
        compare(page.seriesPart.series(400).length, 3)

        page.comparePart.setAlignOn("marker")
        compare(page.alignLabel, "pull")
        verify(page.compareAligned)
        compare(page.compareOffsetMs, -10000, "11.5 s here, 21.5 s there")
        page.comparePart.setAlignOn("utc")
        compare(page.compareOffsetMs, 90000)
        compare(page.viewT1, 130000)
        verify(status.text.indexOf("1:30.0 apart") > 0, status.text)
        page.comparePart.setAlignOn("start")
        compare(page.compareOffsetMs, 0)

        // Back to the list ends the comparison and its alignment
        find(page, "backToList").clicked()
        verify(!page.comparing)
        compare(page.alignOn, "start")
        compare(page.alignLabel, "")
    }

    function test_aComparisonAlignsOnAPullChosenInEach() {
        const page = createPage()
        page.recordings = [twiceRow, earlyRow]
        openRow(page, twiceRow)
        page.comparePart.compareWith(earlyRow)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        const status = find(page, "compareStatus")
        const here = find(page, "alignPullHere")
        const there = find(page, "alignPullThere")
        verify(!here.visible && !there.visible, "no pull to choose until asked for")

        const choice = find(page, "alignChoice")
        choice.currentIndex = choice.indexOfValue("pull")
        verify(choice.currentIndex >= 0, "Align a pull is offered")
        choice.activated(choice.currentIndex)
        compare(page.alignOn, "pull")
        tryVerify(() => page.pullsHere !== null && page.pullsThere !== null, 10000, "the pulls arrive")
        compare(page.pullsHere.length, 2, "the stab and the part throttle are no pulls")
        verify(page.stretchList !== null, "the Jump row's list arrived with it")
        compare(page.pullsThere.length, 1)
        verify(here.visible && there.visible)
        compare(here.count, 2)
        compare(here.textAt(1), "Pull 2 here · 31.0 s · 4.0 s")
        compare(there.textAt(0), "Pull 1 there · 12.3 s · 5.6 s")

        // The first of each to begin with: 18.002 s here on 12.301 s there
        verify(page.compareAligned)
        compare(page.compareOffsetMs, 18002 - 12301)
        verify(status.text.indexOf("aligned on pull 1 here and pull 1 there, at 18.0 s here and 12.3 s there") > 0,
               status.text)

        // The box shows the page's place before any pick is made in it
        page.alignPullHere = 1
        compare(here.currentIndex, 1)
        compare(page.compareOffsetMs, 31002 - 12301)
        page.alignPullHere = 0
        compare(here.currentIndex, 0)
        compare(page.compareOffsetMs, 18002 - 12301)

        // The second here, the pull in top gear: the golden's two starts
        here.currentIndex = 1
        here.activated(1)
        compare(page.alignPullHere, 1)
        verify(page.compareAligned)
        compare(page.compareOffsetMs, 31002 - 12301)
        verify(status.text.indexOf("pull 2 here and pull 1 there, at 31.0 s here") > 0, status.text)
        // The boxes follow the page's choice, set from anywhere
        page.alignPullHere = 0
        compare(here.currentIndex, 0)
        compare(page.compareOffsetMs, 18002 - 12301)
        page.alignPullHere = 1
        compare(here.currentIndex, 1)
        compare(here.currentText, "Pull 2 here · 31.0 s · 4.0 s")
        compare(page.compareOffsetMs, 31002 - 12301)
        // An ask that failed is said as that, never as a recording with
        // no pull
        page.pullsThereFailed = true
        page.comparePart.align()
        verify(!page.compareAligned)
        compare(status.text, earlyRow.title + " isn't drawn: its pulls couldn't be read")
        page.pullsHereFailed = true
        page.comparePart.align()
        compare(status.text, earlyRow.title + " isn't drawn: neither recording's pulls could be read")
        page.pullsThereFailed = false
        page.comparePart.align()
        compare(status.text, earlyRow.title + " isn't drawn: this recording's pulls couldn't be read")
        page.pullsHereFailed = false
        page.comparePart.align()
        verify(page.compareAligned)
        // At the pulls' start the compared one reads its own start
        const rpm = find(page, "signalRows").model.find(s => s.name === "RPM").index
        page.cursorsPart.setCursor(31002)
        compare(page.compareReadout[rpm].atMs, 12300, "pull-early's sample in force at its pull's start")

        // A recording with no pull says so and isn't drawn: the cruise
        page.comparePart.stopComparing()
        page.comparePart.compareWith({ title: "Cruise", uri: "", folder: Harness.recordingFixtures + "/cruise",
                           source: "", profile: "test" })
        tryVerify(() => page.pullsThere !== null && page.pullsHere !== null, 10000, "its pulls are looked for")
        verify(!page.compareAligned)
        compare(status.text, "Cruise isn't drawn: it has no pull (full throttle for a second while it accelerates)")
        verify(!there.enabled, "nothing to choose there")
        compare(page.alignPullHere, 1, "this recording's choice is kept")

        // Another alignment leaves the pulls; back to the list forgets them
        page.comparePart.setAlignOn("start")
        verify(page.compareAligned)
        verify(!here.visible)
        find(page, "backToList").clicked()
        compare(page.pullsHere, null)
        compare(page.pullsThere, null)
        compare(page.alignPullHere, 0)
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
        page.comparePart.compareWith(startLateRow)
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

        page.comparePart.compareWith(earlyRow)
        tryVerify(() => status.text.indexOf("isn't drawn") > 0, 10000, status.text)
        compare(status.text, earlyRow.title + " isn't drawn: it has no engine start"
                + " (RPM reaching 500 after being below 100)")
        verify(!page.compareAligned)
        compare(page.viewT0, 0, "the first alone")
        compare(page.seriesPart.series(400).length, 2, "its own traces alone")

        openRow(page, earlyRow)
        page.comparePart.setAlignOn("engine")
        page.comparePart.compareWith(startEarlyRow)
        tryVerify(() => status.text.indexOf("isn't drawn") > 0, 10000, status.text)
        verify(status.text.indexOf("this recording has no engine start") > 0, status.text)
        page.comparePart.compareWith(lateRow)
        tryVerify(() => status.text.indexOf("neither") > 0, 10000, status.text)
        verify(!page.compareAligned)
    }
}
