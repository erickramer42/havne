// File/layer: tests/qml/tst_liveview_pause.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.liveview_pause (test_qml)

import QtCore
import QtQuick
import QtTest
import App
import AppTest

LiveViewKit {
    id: testCase
    name: "LiveView"

    // Live's store, for the first pause's hint
    Settings {
        id: liveStore
        category: "live"
    }

    // A danger alert on the demo's RPM, saved as the app's conditions
    // (cleanup() empties them)
    function saveRpmDanger() {
        const saved = Bridge.saveConditions(
            '{"havne_conditions":1,"conditions":[{"key":"rpm","name":"Engine on",' +
            '"when":{"signal":["id:1001"],"unit":"RPM","above":-1},' +
            '"alert":{"level":"danger"}}]}')
        verify(saved.ok, saved.words)
    }

    // A page in the test window, grown to the page's size so a pointer
    // lands on the chart; cleanup() puts the size back
    function pageInGrownWindow() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const win = page.Window.window
        grownFrom = [win.width, win.height]
        win.width = page.width
        win.height = page.height
        tryVerify(() => win.width >= page.width && win.height >= page.height, 5000, "the window grown")
        verify(waitForPolish(win), "the grown window laid out")
        return page
    }

    // The plotted history spans at least ms
    function waitForHistory(ms) {
        tryVerify(() => {
            const s = Bridge.signalModel.chartData(0, 0)
            return s.length > 0 && s[0].samples.length >= 6
                   && s[0].samples[s[0].samples.length - 2] - s[0].samples[0] >= ms
        }, 20000, "a history of " + ms + " ms to pause on")
    }

    // A drag in steps, as a pointer moves (mouseDrag's two moves don't carry
    // a DragHandler past its threshold)
    function dragAcross(frame, dx) {
        const x = frame.width / 2, y = frame.height / 2
        mousePress(frame, x, y)
        const step = dx > 0 ? 10 : -10
        for (let d = step; Math.abs(d) <= Math.abs(dx); d += step)
            mouseMove(frame, x + d, y, 10)
        mouseRelease(frame, x + dx, y)
    }

    // The pause keeps each plotted signal's history as it stood, times
    // counted back from its newest sample; the session goes on and the
    // chart holds still, until Live follows now again
    function test_thePauseKeepsTheHistoryAsItStood() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectMock(page)
        const chart = findChild(page, "chart")
        const live = findChild(page, "liveButton")
        verify(!live.visible, "no Live button while the chart follows")
        waitForHistory(500)
        const before = Bridge.signalModel.chartData(0, 0)
        page.pause()
        verify(page.paused, "paused")
        compare(page.snapshot.length, before.length, "a series for each plotted signal")
        let newest = -Infinity
        for (let i = 0; i < before.length; ++i) {
            const sp = before[i].samples, kept = page.snapshot[i].samples
            compare(kept.length, sp.length, "every sample of row " + before[i].row)
            if (sp.length < 2)
                continue
            compare(kept[0] + page.snapshotEndMs, sp[0], "the first sample's time")
            compare(kept[kept.length - 2] + page.snapshotEndMs, sp[sp.length - 2], "the last's")
            newest = Math.max(newest, sp[sp.length - 2])
        }
        compare(page.snapshotEndMs, newest, "the pause is at the newest sample")
        verify(live.visible && live.highlighted, "Live shows, primary")
        verify(page.statusText().indexOf("Paused · ") === 0, page.statusText())

        tryVerify(() => {
            const s = Bridge.signalModel.chartData(0, 0)
            return s[0].samples[s[0].samples.length - 2] > page.snapshotEndMs + 300
        }, 5000, "the session goes on")
        for (const s of chart.fetch(400)) {
            verify(s.samples.length === 0 || s.samples[s.samples.length - 2] <= 0,
                   "the chart holds at the pause: row " + s.row)
        }
        live.clicked()
        verify(!page.paused, "Live follows now again")
        compare(page.snapshot, null, "the snapshot dropped")
        verify(!live.visible, "and goes")
        verify(page.statusText().indexOf("Paused") < 0, page.statusText())
    }

    // A leftward drag on a following chart pauses nothing and doesn't spend
    // the first pause's hint, which the next pause shows
    function test_aLeftwardDragSpendsNoHint() {
        liveStore.setValue("pauseHintSeen", false)
        liveStore.sync()
        const page = pageInGrownWindow()
        connectMock(page)
        waitForHistory(500)
        dragAcross(findChild(page, "chartFrame"), -200)
        verify(!page.paused, "a leftward drag on a following chart pauses nothing")
        liveStore.sync()
        compare(liveStore.value("pauseHintSeen", false), false, "and spends no hint")
        page.pause()
        verify(findChild(page, "pauseHint").visible, "the first pause shows it")
    }

    // Under a replay Start log isn't offered, so neither the hint nor its flag
    function test_aReplaysPauseHasNoHint() {
        liveStore.setValue("pauseHintSeen", false)
        liveStore.sync()
        const page = createTemporaryObject(pageComponent, testCase.parent)
        compare(Bridge.connectSession("replay://" + Harness.replayFixtures
                                      + "/2026-10-25-engine-start?profile=obd2&speed=max"), "",
                "the replay connects")
        waitForHistory(500)
        page.pause()
        verify(page.paused)
        verify(!findChild(page, "pauseHint").visible, "no hint where Start log isn't offered")
        page.resume()
        liveStore.sync()
        compare(liveStore.value("pauseHintSeen", false), false, "the flag is kept for a session that records")
    }

    // A drag pauses and pans back through the snapshot; a drag pushed past
    // its right edge follows now again
    function test_aDragPausesPansAndResumesAtTheEdge() {
        const page = pageInGrownWindow()
        compare(Bridge.connectSession("replay://" + Harness.replayFixtures
                                      + "/2026-10-25-engine-start?profile=obd2&speed=max"), "",
                "the replay connects")
        const kept = page.chartWindowS
        // Live's stored window is put back whatever the checks find
        try {
            findChild(page, "chartWindow_10").clicked()
            waitForHistory(25000)
            const frame = findChild(page, "chartFrame")
            dragAcross(frame, 200)
            verify(page.paused, "a drag pauses")
            verify(page.viewEndMs < -1000, "and pans back: " + page.viewEndMs)
            const panned = page.viewEndMs
            dragAcross(frame, 80)
            verify(page.paused && page.viewEndMs < panned, "a second drag pans on: " + page.viewEndMs)
            dragAcross(frame, -600)
            verify(!page.paused, "a drag past the right edge follows now again")
        } finally {
            findChild(page, "chartWindow_" + kept).clicked()
        }
    }

    // A resume leaves no pan behind: the next drag starts from the live edge,
    // so a leftward one pauses nothing
    function test_aDragAfterAResumeStartsFromNow() {
        const page = pageInGrownWindow()
        compare(Bridge.connectSession("replay://" + Harness.replayFixtures
                                      + "/2026-10-25-engine-start?profile=obd2&speed=max"), "",
                "the replay connects")
        const kept = page.chartWindowS
        try {
            findChild(page, "chartWindow_10").clicked()
            waitForHistory(25000)
            const frame = findChild(page, "chartFrame")
            dragAcross(frame, 200)
            verify(page.paused && page.viewEndMs < -1000, "panned back: " + page.viewEndMs)
            page.resume()
            compare(page.viewEndMs, 0, "a resume forgets the pan")
            dragAcross(frame, -100)
            verify(!page.paused, "a leftward drag on the following chart pauses nothing")
        } finally {
            findChild(page, "chartWindow_" + kept).clicked()
        }
    }

    // Space on a focused check box toggles it and leaves the chart following
    function test_spaceOnAFocusedCheckBoxIsTheBoxsNotThePauses() {
        if (Theme.mobile)
            skip("Space is a desktop's key")
        const page = pageInGrownWindow()
        const list = connectMock(page)
        waitForHistory(1500)
        const box = findChild(list.itemAtIndex(0), "plotCheck")
        verify(box.enabled, "the first mock row can be plotted")
        const before = box.checked
        findChild(page, "filterText").focus = false
        box.forceActiveFocus()
        verify(box.activeFocus, "the box has the focus")
        keyClick(Qt.Key_Space)
        tryCompare(box, "checked", !before, 2000, "Space toggles the box")
        verify(!page.paused, "and the chart is not paused")
        // the page's own focus still pauses
        box.focus = false
        page.forceActiveFocus()
        keyClick(Qt.Key_Space)
        verify(page.paused, "Space off a button pauses as before")
    }

    // Space pauses and resumes on a desktop, but types a space into Find a
    // signal; a tap on the paused chart reads each trace's value at the
    // cursor into its label
    function test_spaceAndTheCursor() {
        if (Theme.mobile)
            skip("Space is a desktop's key")
        const page = pageInGrownWindow()
        connectMock(page)
        waitForHistory(1500)
        const field = findChild(page, "filterText")
        field.focus = false
        page.forceActiveFocus()
        keyClick(Qt.Key_Space)
        verify(page.paused, "Space pauses")
        keyClick(Qt.Key_Space)
        verify(!page.paused, "and resumes")
        field.forceActiveFocus()
        keyClick(" ")
        compare(field.text, " ", "a space goes into the filter")
        verify(!page.paused, "and leaves the chart following")
        Bridge.liveSignalModel.filter.clear()
        field.focus = false
        page.forceActiveFocus()
        keyClick(Qt.Key_Space)
        verify(page.paused, "paused again")

        const frame = findChild(page, "chartFrame")
        const chart = findChild(page, "chart")
        tryVerify(() => chart.drawn !== null, 2000, "the paused chart drawn")
        // Halfway into the snapshot, where every trace has samples
        const w = page.chartWindowS * 1000
        const x = Math.round(chart.x + (page.snapshotStartMs / 2 + w) / w * (chart.drawn.plotW - 1))
        // Taken before the tap: the labels it fills change the plot's width
        const tapped = page.timeAt(x)
        mouseClick(frame, x, frame.height / 2)
        verify(!isNaN(page.cursorMs), "a tap places the cursor")
        compare(page.cursorMs, tapped, "where the tap was")
        verify(page.statusText().indexOf("cursor at −") > 0, page.statusText())
        let read = 0
        for (const s of page.snapshot) {
            const sp = s.samples
            let best = 0
            for (let i = 0; i + 1 < sp.length; i += 2) {
                if (Math.abs(sp[i] - page.cursorMs) < Math.abs(sp[best] - page.cursorMs))
                    best = i
            }
            if (sp.length < 2 || Math.abs(sp[best] - page.cursorMs) > page.staleMs)
                continue
            tryVerify(() => chart.drawn && chart.drawn.traces.some(tr => tr.row === s.row
                                                                    && tr.labelMs !== undefined),
                      2000, "row " + s.row + " read at the cursor")
            const tr = chart.drawn.traces.find(t => t.row === s.row)
            compare(tr.labelMs, sp[best], "row " + s.row + "'s label reads the nearest sample")
            compare(tr.label, page.valueText(sp[best + 1], s.decimals, s.unit),
                    "row " + s.row + "'s value")
            ++read
        }
        verify(read > 0, "some trace has a sample at the cursor")
    }

    // The paused view reads only the samples in view, found by a binary
    // search: the reads follow the window, not the ring, and the picked
    // samples are the linear walk's
    function test_aPausedViewReadsOnlyTheWindowsSamples() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const n = 30000                        // a full ring's samples
        const t0 = -n * 10
        const plain = []
        for (let i = 0; i < n; ++i)
            plain.push(t0 + i * 10, 50 + 40 * Math.sin(i / 7))
        let reads = 0
        const counted = new Proxy(plain, {
            get(target, key) {
                if (typeof key === "string" && key.length > 0 && key.charCodeAt(0) <= 57
                        && key.charCodeAt(0) >= 48)
                    ++reads
                return target[key]
            }
        })
        const window = page.chartWindowS * 1000
        const viewEnd = t0 + n * 5             // the middle of the ring
        page.viewEndMs = viewEnd
        page.snapshot = [{ row: 0, unit: "x", decimals: 0, samples: counted }]
        reads = 0
        const out = page.pausedSeries(600)
        const inView = Math.round(window / 10)
        verify(reads > 0, "the search read samples")
        verify(reads < 8 * inView + 500, "reads " + reads + " for " + inView + " in view")
        // The same by grouping every sample in view by its column (columns
        // are counted from time 0, and the times here are negative)
        const colMs = window / 600
        const groups = new Map()
        for (let i = 0; i + 1 < plain.length; i += 2) {
            if (plain[i] < viewEnd - window || plain[i] > viewEnd)
                continue
            const c = Math.floor(plain[i] / colMs)
            const g = groups.get(c) || { lo: i, hi: i, last: i }
            if (plain[i + 1] < plain[g.lo + 1]) g.lo = i
            if (plain[i + 1] > plain[g.hi + 1]) g.hi = i
            g.last = i
            groups.set(c, g)
        }
        const want = []
        for (const g of groups.values()) {
            const keep = [g.lo, g.hi, g.last].filter((v, k, all) => all.indexOf(v) === k)
            for (const i of keep.sort((x, y) => x - y))
                want.push(plain[i], plain[i + 1])
        }
        compare(JSON.stringify(out[0].samples), JSON.stringify(want))
        verify(want.length > 0, "the window holds samples")
        // A window before every sample and one after hold none
        page.viewEndMs = t0 - 1000
        compare(page.pausedSeries(600)[0].samples.length, 0, "before the ring")
        page.viewEndMs = 0
        page.snapshot = [{ row: 0, unit: "x", decimals: 0, samples: [-20000, 1, -5000, 2] }]
        compare(page.pausedSeries(600)[0].samples.length, 4, "both in a 30 s window")
    }

    // The sample nearest the cursor is found by a search, with the answer
    // a walk of every sample gives: the earlier of two equally near, none
    // past staleMs, none without samples
    function test_theCursorsSampleIsFoundBySearch() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const n = 30000
        const plain = []
        for (let i = 0; i < n; ++i)
            plain.push(-300000 + i * 10, i)
        let reads = 0
        const counted = new Proxy(plain, {
            get(target, key) {
                if (typeof key === "string" && key.charCodeAt(0) >= 48 && key.charCodeAt(0) <= 57)
                    ++reads
                return target[key]
            }
        })
        const walk = (samples, at) => {
            let best = -1
            for (let i = 0; i + 1 < samples.length; i += 2) {
                if (best < 0 || Math.abs(samples[i] - at) < Math.abs(samples[best] - at))
                    best = i
            }
            return best >= 0 && Math.abs(samples[best] - at) <= page.staleMs ? best : -1
        }
        // On a sample, between two (equally near: the earlier), at both
        // ends, past either end by less and by more than staleMs
        const cursors = [-150000, -149995, -149996, -149994, -300000, -10, -300003,
                         -300000 - page.staleMs, -300000 - page.staleMs - 1,
                         -10 + page.staleMs, -10 + page.staleMs + 1, -12345]
        for (const at of cursors) {
            page.cursorMs = at
            reads = 0
            const got = page.cursorSample(counted)
            compare(got, walk(plain, at), "cursor " + at)
            verify(reads > 0, "the search read samples")
            verify(reads < 200, "cursor " + at + " read " + reads)
        }
        page.cursorMs = -150000
        compare(page.cursorSample([]), -1, "no samples")
        compare(page.cursorSample([-150000, 1]), 0, "one sample")
        const twice = [-150100, 1, -150000, 2, -150000, 3, -149900, 4]
        compare(page.cursorSample(twice), 2, "the first of equal times")
        page.cursorMs = NaN
        compare(page.cursorSample(plain), -1, "no cursor")
    }

    // The snapshot goes with what it was taken of: a unit change, leaving
    // the page, a disconnect
    function test_thePauseDropsWithItsSession() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectMock(page)
        waitForHistory(300)
        page.pause()
        verify(page.paused)
        Bridge.unitSystem = 1
        verify(!page.paused, "a unit change drops it")
        Bridge.unitSystem = 0
        page.pause()
        verify(page.paused)
        page.visible = false
        verify(!page.paused, "leaving the page drops it")
        page.visible = true
        page.pause()
        verify(page.paused)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.connected, 5000, "disconnected")
        verify(!page.paused, "a disconnect drops it")
        compare(page.snapshot, null)
    }

    // The first pause of all names Start log, once: not at the next pause,
    // nor on a page made later
    function test_theFirstPauseHintShowsOnce() {
        liveStore.setValue("pauseHintSeen", false)
        liveStore.sync()
        const page = createTemporaryObject(pageComponent, testCase.parent)
        connectMock(page)
        waitForHistory(300)
        const hint = findChild(page, "pauseHint")
        verify(!hint.visible, "no hint while live")
        page.pause()
        verify(hint.visible, "the first pause shows it")
        compare(hint.text, "Start log records the session for the viewer")
        page.resume()
        verify(!hint.visible, "gone at resume")
        page.pause()
        verify(!hint.visible, "not at the second pause")
        page.resume()
        const later = createTemporaryObject(pageComponent, testCase.parent)
        later.pause()
        verify(later.paused)
        verify(!findChild(later, "pauseHint").visible, "nor on a page made later")
    }

    // The empty state: plain words, and a Connect that isn't a second
    // primary beside the header's
    function test_theEmptyStateOffersAConnectInPlainWords() {
        const page = createTemporaryObject(pageComponent, testCase.parent)
        compare(page.statusText(), "Connect a session to read live values")
        const button = findChild(page, "liveConnect")
        verify(button && button.visible, "Connect… is offered")
        verify(!button.highlighted, "secondary: the header holds the primary")
        compare(button.text, "Connect…")
    }

    // An alert ON on a signal fills its value's cell in the level's role,
    // the mark beside a danger; the name keeps its color, other rows none
    function test_anAlertFillsTheValueCellAlone_data() {
        return Theme.paletteNames.map(p => ({ tag: p, palette: p }))
    }

    function test_anAlertFillsTheValueCellAlone(data) {
        usePalette(data.palette)
        // In the window: under the TestCase item every visible reads false
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const list = connectMock(page)
        let rpm = null, other = null
        for (let i = 0; i < list.count; ++i) {
            const item = list.itemAtIndex(i)
            if (findChild(item, "nameLabel").text === "RPM")
                rpm = item
            else if (other === null)
                other = item
        }
        verify(rpm && other, "the demo's RPM row and another")
        tryVerify(() => rpm.hasValue, 5000, "RPM has a value")
        const value = findChild(rpm, "valueLabel")
        const fill = findChild(rpm, "valueAlert")
        const mark = findChild(rpm, "valueAlertMark")
        verify(!fill.visible, "no alert, no fill")

        page.alertLevels = { "1001": 2 }
        verify(fill.visible, "a danger fills the cell")
        sameColor(fill.color, Theme.alertDangerFill, "the danger's fill")
        sameColor(value.color, Theme.textOnAlertDanger, "the value's text on it")
        verify(mark.visible, "the mark beside a danger")
        sameColor(mark.color, Theme.textOnAlertDanger, "the mark in the text's color")
        sameColor(findChild(rpm, "nameLabel").color, Theme.text, "the name keeps its color")
        verify(rpm.Accessible.name.indexOf(", danger alert") > 0, rpm.Accessible.name)
        verify(!findChild(other, "valueAlert").visible, "a row no alert reads has no fill")

        page.alertLevels = { "1001": 1 }
        sameColor(fill.color, Theme.alertWarnFill, "the warning's fill")
        sameColor(value.color, Theme.textOnAlertWarn, "the value's text on it")
        verify(!mark.visible, "a warning has no mark")

        page.alertLevels = ({})
        verify(!fill.visible, "the fill goes with the alert")
        sameColor(value.color, Theme.text, "and the value's own color is back")
    }

    // The page's levels follow the bridge's: a saved alert on the demo's RPM
    // fills its row's value through Bridge.alertLevels
    function test_theBridgesAlertFillsTheRowItReads() {
        saveRpmDanger()
        const page = createTemporaryObject(pageComponent, testCase.parent)
        const list = connectMock(page)
        let rpm = null
        for (let i = 0; i < list.count; ++i) {
            const item = list.itemAtIndex(i)
            if (findChild(item, "nameLabel").text === "RPM")
                rpm = item
        }
        verify(rpm, "the demo's RPM row")
        const fill = findChild(rpm, "valueAlert")
        tryVerify(() => fill.visible, 10000, "the RPM's danger fills its value")
        verify(findChild(rpm, "valueAlertMark").visible, "with the mark")
        Bridge.disconnectSession()
        tryVerify(() => !fill.visible, 10000, "and leaves at the disconnect")
    }
}
