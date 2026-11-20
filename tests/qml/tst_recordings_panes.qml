// File/layer: tests/qml/tst_recordings_panes.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.recordings_panes (test_qml)

import QtQuick
import QtTest
import App
import AppTest

RecordingsKit {
    id: testCase
    name: "Recordings"

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

    // A decode chosen while comparing on pulls asks again for the pulls
    // and aligns on them
    function test_aDecodeWhileComparingOnPullsAsksAgain() {
        const page = openFindings()
        page.recordings = [findingsRow, earlyRow]
        page.comparePart.compareWith(earlyRow)
        tryVerify(() => page.compareAligned, 10000, "the compared recording opens")
        const choice = find(page, "alignChoice")
        choice.currentIndex = choice.indexOfValue("pull")
        choice.activated(choice.currentIndex)
        tryVerify(() => page.pullsHere !== null && page.pullsThere !== null, 10000, "the pulls arrive")
        page.compareStatus = "stale"   // what the alignment says, until it is made again
        const decode = find(page, "decodeChoice")
        const obd2 = decode.indexOfValue("obd2")
        decode.activated(obd2)
        tryVerify(() => page.pullsHere === null, 10000, "the reopen clears the pulls")
        tryVerify(() => page.pullsHere !== null, 10000, "and they are asked for again")
        tryVerify(() => page.compareStatus !== "stale", 10000, "and the alignment is made again")
        verify(page.comparing, "still comparing")
    }

    // The page's keys are a focused box's own: Home and End move a
    // ComboBox's choice, never the chart's cursor
    function test_aFocusedBoxKeepsItsKeys() {
        const page = openEarly()
        page.forceActiveFocus()
        page.cursorsPart.setCursor(1000)
        // The box sits in Compare's controls, shown while Compare is open
        find(page, "compareButton").clicked()
        const choice = find(page, "compareChoice")
        verify(waitForPolish(page.Window.window), "the controls are laid out")
        verify(choice.visible, "the box is shown")
        choice.forceActiveFocus()
        verify(choice.activeFocus, "the box has the focus")
        // Left and Right it doesn't use: the cursor still steps
        keyClick(Qt.Key_Right)
        tryVerify(() => page.cursorMs > 1000, 2000, "Right steps the cursor beside a focused box")
        page.cursorsPart.setCursor(1000)
        keyClick(Qt.Key_End)
        verify(page.comparing, "End went to the box: it picked the last recording")
        keyClick(Qt.Key_Home)
        keyClick("b")
        compare(page.cursorMs, 1000, "the cursor stayed")
        verify(!page.arrowsMoveB, "B did nothing to the cursors")
        const before = page.maximizedTall
        keyClick("f")
        compare(page.maximizedTall, before, "F didn't maximize the chart")
        // no open of the compared recording is left running
        page.comparePart.stopComparing()
        compare(page.comparing, false, "the comparison is stopped")
    }

    // A mouse pick gives a box the focus; a press in the plot takes it back,
    // so the chart's keys work again without a Tab
    function test_aPressInThePlotTakesTheKeysBackFromABox() {
        const page = openFindings()
        const restore = growWindow(page)
        try {
            const chart = find(page, "chart")
            tryVerify(() => chart.drawn !== null, 5000, "the chart is drawn")
            const choice = find(page, "decodeChoice")
            tryVerify(() => choice.visible && choice.width > 0, 2000, "the box shows")
            mouseClick(choice)
            tryVerify(() => choice.popup.opened, 2000, "the box opens")
            verify(choice.activeFocus, "a mouse press gives the box the focus")
            choice.popup.close()
            tryVerify(() => !choice.popup.opened, 2000, "and closes")
            const frame = find(page, "chartFrame")
            const at = inPlot(chart, 0)
            mouseClick(frame, at.x, at.y)
            verify(!choice.activeFocus, "the press took the focus back")
            const placed = page.cursorMs
            verify(!isNaN(placed), "the press placed the cursor")
            keyClick(Qt.Key_Right)
            tryVerify(() => page.cursorMs > placed, 2000, "Right steps the cursor")
            const stepped = page.cursorMs
            keyClick(Qt.Key_Home)
            tryVerify(() => page.cursorMs < stepped, 2000, "so does Home")
        } finally {
            restore()
        }
    }

    // The decoded profile stays in the box after the chart is maximized
    // and back
    function test_theDecodeChoiceSurvivesAMaximize() {
        const page = openFindings()
        const choice = find(page, "decodeChoice")
        const obd2 = choice.indexOfValue("obd2")
        choice.activated(obd2)
        const info = find(page, "recordingInfo")
        tryVerify(() => info.visible && info.text.indexOf("values decoded from its frames through obd2") > 0,
                  10000, "decoded through obd2")
        compare(choice.currentIndex, obd2)
        page.refreshPart.toggleMaximized()
        verify(waitForPolish(page.Window.window), "maximized")
        page.refreshPart.toggleMaximized()
        verify(waitForPolish(page.Window.window), "and back")
        compare(choice.currentIndex, obd2, "the box still reads obd2")
    }

    // The first cursor's hint comes once, and the second cursor ends it
    function test_theSecondCursorsHintComesOnce() {
        const page = openEarly()
        page.spanHintSeen = false
        page.cursorsPart.setCursor(1000)
        verify(page.spanHintUp)
        verify(find(page, "spanLine").visible)
        compare(find(page, "spanText").text, "At 1.0 s. Shift+click to place the second cursor")
        verify(!find(page, "zoomToSpan").visible, "nothing to zoom to yet")
        page.cursorsPart.setCursorB(3000)
        verify(!page.spanHintUp, "the second cursor ends it")
        page.cursorsPart.clearCursorB()
        page.cursorMs = NaN
        page.cursorsPart.setCursor(2000)
        verify(!page.spanHintUp, "not a second time")
        verify(page.spanHintSeen, "and that's kept")
        verify(!find(page, "spanLine").visible)
    }

    // Maximized, each strip's cell gives A, B and the change
    function test_theStripShowsTheSpanMaximized() {
        const page = openEarly()
        spanned(page, 1000, 3000)
        page.refreshPart.toggleMaximized()
        try {
            const strip = find(page, "cursorStrip")
            tryVerify(() => strip.visible && strip.count > 0, 5000, "the strip shows")
            compare(strip.cellHeight, 56)
            tryVerify(() => strip.itemAtIndex(0) !== null, 2000, "a cell")
            const cell = strip.itemAtIndex(0)
            compare(find(cell, "stripValue").text, grouped(2001) + " rpm")
            tryCompare(find(cell, "stripSpan"), "text", "B " + grouped(1982) + " rpm · Δ -19 rpm")
            page.cursorsPart.clearCursorB()
            compare(strip.cellHeight, 40)
        } finally {
            page.refreshPart.toggleMaximized()
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
        page.openPart.open(lateRow)
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
            page.cursorsPart.setView(0, 5000)
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
            page.cursorsPart.clearCursorB()
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
        page.cursorsPart.setCursor(1000)
        tryVerify(() => chart.paints > paints, 2000, "drawn with the cursor")
        compare(chart.drawn.axisH, ChartLayout.axisH, "the desktop's stays")
        page.cursorMs = NaN
        Theme.compact = true
        try {
            page.cursorsPart.setCursor(NaN)
            page.cursorMs = NaN
            chart.requestPaint()
            tryVerify(() => chart.drawn && chart.drawn.axisH === ChartLayout.axisH, 2000, "none yet")
            page.cursorsPart.setCursor(1000)
            tryVerify(() => chart.drawn && chart.drawn.axisH === ChartLayout.axisHCursor, 2000, "grown")
        } finally {
            Theme.compact = false
        }
    }

    // The frame test plots a signal of each unit first, so it draws the
    // most panes the recording gives
    function test_theFrameTestPlotsOneSignalOfEachUnit() {
        const page = openEarly()
        compare(page.frameTestPart.frameTestSignals(find(page, "signalRows").model).map(s => s.name).join(","),
                "RPM,Throttle,Coolant")
        const s = (name, unit) => ({ name: name, unit: unit, isText: false, samples: 1 })
        const list = [s("A", "rpm"), s("B", "rpm"), s("C", "%"), s("D", "°C"), s("E", "°F"),
                      { name: "T", unit: "", isText: true, samples: 1 }, s("F", "V"), s("G", "V")]
        compare(page.frameTestPart.frameTestSignals(list).map(s => s.name).join(","), "A,C,D,F",
                "a signal of each pane first")
        compare(page.frameTestPart.frameTestSignals(list.slice(0, 3)).map(s => s.name).join(","), "A,C,B",
                "then the others")
    }

    // On a phone the axes' strip is a touch wide, past the captions: a
    // double tap there makes the range automatic again. The captions' width
    // is the font's, so where they already reach a touch the strip ends
    // with them and the tap lands inside them.
    function test_aPhonesAxisStripIsATouchWide() {
        const page = openEarly()
        const chart = panedChart(page)
        // The phone's target (48) is what the strip is measured against,
        // so the form is set before the sums
        Theme.compact = true
        try {
            verify(waitForPolish(page.Window.window), "laid out as a phone")
            compare(Theme.touchTarget, 48)
            page.panesPart.setPaneRange("unit %", { vMin: 0, vMax: 10 })
            const p = chart.drawn.panes[1]
            const left = chart.drawn.left
            verify(left < Theme.touchTarget, "the captions end before a touch's width: " + left)
            const x = (left + Theme.touchTarget) / 2
            const y = p.top + p.h / 2
            compare(page.panesPart.doubleTapAt(x, y), "axis")
            compare(page.paneRanges["unit %"], undefined)
        } finally {
            Theme.compact = false
        }
        verify(waitForPolish(page.Window.window), "laid out as a desktop")
        const left = chart.drawn.left
        const p = chart.drawn.panes[1]
        const y = p.top + p.h / 2
        page.panesPart.setPaneRange("unit %", { vMin: 0, vMax: 10 })
        // On the desktop the strip is the captions alone, so a point past
        // them, and past a touch where that is wider, is the plot
        compare(page.panesPart.doubleTapAt(Math.max(left, Theme.touchTarget) + 1, y), "maximize",
                "on the desktop that's the plot")
        page.refreshPart.toggleMaximized()
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
            fuzzyCompare(page.cursorsPart.timeAt(chart.x + chart.drawn.left), page.viewT0, 1e-6, "the plot's left edge")
            fuzzyCompare(page.cursorsPart.timeAt(chart.x + chart.drawn.left + chart.drawn.plotW - 1), page.viewT1, 1e-6,
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
            page.openPart.backToList()
            page.openPart.open(earlyRow)
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
            tryVerify(() => { store.sync(); return String(store.value("views", "")).indexOf("\"3\"") >= 0 },
                      3000, "written to the store, with the recording")
            store.destroy()

            const evenAt = seam.mapToItem(frame, seam.width / 2, seam.height / 2)
            doubleClick(frame, evenAt.x, evenAt.y)
            compare(page.paneShares["3"], undefined, "evened")
            verify(!page.chartMaximized, "a seam's double click maximizes nothing")
            tryVerify(() => Math.abs(chart.drawn.panes[0].h - chart.drawn.panes[1].h) < 0.5, 2000, "even")
        } finally {
            page.panesPart.setPaneShares(3, null)
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
        page.cursorsPart.setView(-5000, 1000)
        compare(page.viewT0, 0)
        compare(page.viewT1, 6000)
        page.cursorsPart.zoomAt(3000, 0.5)
        compare(page.viewT1 - page.viewT0, 3000)
        page.cursorsPart.setView(19900, 19950)
        compare(page.viewT1 - page.viewT0, page.minSpanMs)
        compare(page.viewT1, 20000)
        page.cursorsPart.zoomAt(10000, 1000)
        compare(page.viewT0, 0)
        compare(page.viewT1, 20000)

        // A drag left moves the view later, a tap places the cursor, the
        // wheel zooms in
        const frame = find(page, "chartFrame")
        page.cursorsPart.setView(0, 4000)
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
        page.cursorsPart.setView(4000, 8000)
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
        page.cursorsPart.setView(0, 4000)
        tryVerify(() => !chart.moving, 2000, "rested after opening")
        page.cursorsPart.setView(1000, 5000)
        verify(!chart.moving, "one change is not a move")
        verify(chart.antialiasing)
        page.cursorsPart.setView(1100, 5100)
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
                verify(Qt.colorEqual(set.line.strokeColor, Theme.seriesColor(tr.slot)),
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
                verify(Qt.colorEqual(set.edge.strokeColor, Theme.seriesColor(tr.slot)),
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
    // A trace's color follows the order signals are plotted in, not the
    // signal's row, so signals far apart in the list never share one
    function test_plottedSignalsNeverShareAColor() {
        const page = createPage()
        page.openPart.open({ title: "Pull", folder: Harness.recordingFixtures + "/pull-early" })
        const rows = find(page, "signalRows")
        tryVerify(() => rows.visible && rows.count === 4, 10000, "the recording opens")
        const numbers = [0, 1, 2]   // the fourth row is text
        verify(numbers.length >= 3, "three numbers to plot")
        // Every number plotted, a row far from its neighbors included
        for (const i of numbers) {
            if (page.plotted[i] !== true)
                page.cursorsPart.togglePlotted(i)
        }
        const colors = () => page.seriesPart.series(400).map(t => String(Theme.seriesColor(t.slot)))
        compare(new Set(colors()).size, numbers.length, "a color each")
        compare(page.seriesPart.series(400).length, numbers.length)
    }

    // Unplotting one signal recolors none of the others; plotting it
    // again takes the lowest free slot
    function test_aTraceKeepsItsColorWhileItStaysPlotted() {
        const page = createPage()
        page.openPart.open({ title: "Pull", folder: Harness.recordingFixtures + "/pull-early" })
        const rows = find(page, "signalRows")
        tryVerify(() => rows.visible && rows.count === 4, 10000, "the recording opens")
        const numbers = [0, 1, 2]   // the fourth row is text
        for (const i of numbers) {
            if (page.plotted[i] !== true)
                page.cursorsPart.togglePlotted(i)
        }
        const before = {}
        for (const i of numbers)
            before[i] = page.panesPart.slotOf(i)
        const gone = numbers[0]
        page.cursorsPart.togglePlotted(gone)
        for (const i of numbers.slice(1))
            compare(page.panesPart.slotOf(i), before[i], "signal " + i + " keeps its slot")
        page.cursorsPart.togglePlotted(gone)
        compare(page.panesPart.slotOf(gone), before[gone], "the freed slot is taken again")
    }

    // The slots follow the order of plotting: the first signal ticked
    // takes the first color whatever its row
    function test_theColorsFollowTheOrderOfPlotting() {
        const page = createPage()
        page.openPart.open({ title: "Pull", folder: Harness.recordingFixtures + "/pull-early" })
        const rows = find(page, "signalRows")
        tryVerify(() => rows.visible && rows.count === 4, 10000, "the recording opens")
        for (const i of [0, 1, 2]) {
            if (page.plotted[i] === true)
                page.cursorsPart.togglePlotted(i)
        }
        page.cursorsPart.togglePlotted(2)
        page.cursorsPart.togglePlotted(0)
        page.cursorsPart.togglePlotted(1)
        compare(page.panesPart.slotOf(2), 0, "the first ticked, the first color")
        compare(page.panesPart.slotOf(0), 1)
        compare(page.panesPart.slotOf(1), 2)
        compare(page.seriesPart.series(400).find(t => t.row === 2).slot, 0, "and the chart draws it so")
    }
}
