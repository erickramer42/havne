// File/layer: app/qml/RecordingCursors.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// The viewer's view, zoom, cursors and span: the time axis functions, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    function durationMs() {
        return part.host.recording.durationMs > 0 ? part.host.recording.durationMs : 1000
    }

    // The axis: the recording, and an aligned compared one on it
    function axisStart() {
        return part.host.compareAligned ? Math.min(0, part.host.compareOffsetMs) : 0
    }
    function axisEnd() {
        return part.host.compareAligned ? Math.max(part.durationMs(), part.host.compareOffsetMs + part.host.compare.durationMs)
                              : part.durationMs()
    }

    // The Frames view's rows again for the part in view, once it has rested
    // for settleMs: a drag or a pinch asks for none until it stops
    function refillFramesSoon() {
        if (part.host.framesShown)
            part.host.framesRefill.restart()
    }
    function refillFrames() {
        part.host.framesRefill.stop()
        if (part.host.framesShown)
            part.host.viewFrames.load(part.host.recording.folder, part.host.viewT0, part.host.viewT1)
    }

    function setView(t0, t1) {
        const lo = part.axisStart()
        const hi = part.axisEnd()
        const span = Math.min(hi - lo, Math.max(part.host.minSpanMs, t1 - t0))
        const start = Math.min(Math.max(lo, t0), hi - span)
        part.host.viewT0 = start
        part.host.viewT1 = start + span
        if (part.host.settle.running)
            part.host.chart.moving = true
        part.host.settle.restart()
        part.host.chart.requestPaint()
    }
    function zoomAt(t, factor) {
        part.setView(t - (t - part.host.viewT0) * factor, t + (part.host.viewT1 - t) * factor)
    }
    function showAll() {
        part.setView(part.axisStart(), part.axisEnd())
    }
    // A time placed in the middle of the view, the view's span kept
    function centerOn(t) {
        const half = (part.host.viewT1 - part.host.viewT0) / 2
        part.setView(t - half, t + half)
    }
    // A Conditions row: the view centered on its entry and the cursor at its
    // start, as a finding's row; a phone's panel gives the chart back
    function showCondition(i) {
        const s = part.host.conditionList[i]
        if (s === undefined)
            return
        part.host.conditionPick = i
        part.centerOn((s.fromMs + s.toMs) / 2)
        part.setCursor(s.fromMs)
        if (Theme.compact)
            part.host.conditionsOpen = false
    }

    // The cursors' time tags on the axis row, for the layout to keep its
    // captions out from under them: the time and the tag's width
    function cursorTags() {
        const out = []
        for (const ms of [part.host.cursorMs, part.host.cursorBMs]) {
            if (isFinite(ms) && ms >= part.host.viewT0 && ms <= part.host.viewT1)
                out.push({ ms: ms, width: Math.max(part.host.phoneShape ? Theme.touchTarget : 24,
                                                   part.timeTextWidth(ms) + 8) })
        }
        return out
    }
    FontMetrics { id: tagMetrics; font.pixelSize: ChartLayout.labelPx; font.family: Theme.valueFamily }
    function timeTextWidth(ms) {
        return tagMetrics.advanceWidth(part.host.timeText(ms, true))
    }

    // A tapped time moved to the nearest sample of the first signal
    // plotted, so the cursor stands where the line has a sample and reads
    // the value drawn there; as it is with nothing plotted
    function snapSignal() {
        return part.host.recording.signalList.find(s => !s.isText && part.host.plotted[s.index] === true)
    }
    function snapped(t) {
        const first = part.snapSignal()
        if (!first)
            return t
        const at = part.host.recording.nearestSampleMs(first.index, t)
        return at >= 0 ? at : t
    }
    function valuesAtTime(t) {
        const values = {}
        if (t >= 0) {
            for (const v of part.host.recording.valuesAt(part.host.recording.signalList.map(s => s.index), t))
                values[v.index] = v
        }
        return values
    }
    function setCursor(t) {
        part.host.cursorMs = Math.min(Math.max(part.axisStart(), t), part.axisEnd())
        part.host.readout = part.valuesAtTime(part.host.cursorMs)
        part.refreshCompareReadout()
        if (!part.host.hasCursorB && !part.host.spanHintSeen) {
            part.host.spanHintSeen = true
            part.host.spanHintUp = true
        }
        part.askSpanStats()
        part.host.chart.requestPaint()
    }
    function setCursorB(t) {
        part.host.cursorBMs = Math.min(Math.max(part.axisStart(), t), part.axisEnd())
        part.host.readoutB = part.valuesAtTime(part.host.cursorBMs)
        part.refreshCompareReadout()
        part.host.spanHintUp = false
        part.askSpanStats()
        part.host.chart.requestPaint()
    }
    // The compared recording's values at the cursor the rows read: the first,
    // or the earlier of two
    function refreshCompareReadout() {
        part.host.compareReadout = part.host.compareValuesAt(part.host.hasSpan ? part.host.spanFromMs : part.host.cursorMs)
    }
    function clearCursorB() {
        part.host.cursorBMs = NaN
        part.host.readoutB = ({})
        part.refreshCompareReadout()
        part.host.spanStats = ({})
        part.host.statsAsk = 0
        part.host.arrowsMoveB = false
        part.host.chart.requestPaint()
    }
    // The second gesture at time t: with no first cursor yet it places that
    function placeSecond(t) {
        if (part.host.hasCursor)
            part.setCursorB(t)
        else
            part.setCursor(t)
    }
    function askSpanStats() {
        if (!part.host.hasSpan) {
            part.host.statsAsk = 0
            part.host.spanStats = ({})
            return
        }
        part.host.statsAsk = part.host.recording.stats(part.host.recording.signalList.map(s => s.index), part.host.spanFromMs, part.host.spanToMs)
        if (part.host.statsAsk === 0)
            part.host.spanStats = ({})
    }
    function zoomToSpan() {
        const pad = (part.host.spanToMs - part.host.spanFromMs) * 0.05
        part.setView(part.host.spanFromMs - pad, part.host.spanToMs + pad)
    }
    // Keeps a cursor a key moved in the view
    function bringIntoView(t) {
        if (t < part.host.viewT0 || t > part.host.viewT1)
            part.centerOn(t)
    }
    function moveKeyed(t, isB) {
        if (isB)
            part.setCursorB(t)
        else
            part.setCursor(t)
        part.bringIntoView(t)
    }
    function stepCursor(forward) {
        const s = part.snapSignal()
        const from = part.host.arrowsMoveB ? part.host.cursorBMs : part.host.cursorMs
        if (!s || isNaN(from))
            return
        part.host.stepsB = part.host.arrowsMoveB
        part.host.recording.step(s.index, from, forward)
    }
    function cursorToEnd(last) {
        part.moveKeyed(part.snapped(last ? part.durationMs() : 0), part.host.arrowsMoveB)
    }
    // A cursor's handle let go: on a sample, and one dropped on the other
    // leaves a single cursor
    // The nearest sample can lie past the view's end by half a sample when the
    // end is off the grid (a wheel zoom): then the one in force at the end,
    // so the handle never drops out of view
    function snappedInView(t) {
        const s = part.snapped(t)
        if (s < part.host.viewT0)
            return part.host.viewT0
        const first = part.snapSignal()
        if (s <= part.host.viewT1 || !first)
            return Math.min(s, part.host.viewT1)
        const at = part.host.recording.valuesAt([first.index], part.host.viewT1)
        return at.length > 0 && at[0].atMs >= 0 ? at[0].atMs : part.host.viewT1
    }
    function dropHandle(isB) {
        const t = part.snappedInView(isB ? part.host.cursorBMs : part.host.cursorMs)
        if (isB)
            part.setCursorB(t)
        else
            part.setCursor(t)
        if (part.host.hasCursorB && Math.abs(part.xOfTime(part.host.cursorMs) - part.xOfTime(part.host.cursorBMs)) <= part.host.handleDropPx)
            part.clearCursorB()
    }
    // A time's x in the chart's frame
    function xOfTime(t) {
        return part.host.chart.x + part.plotLeft() + (t - part.host.viewT0) / (part.host.viewT1 - part.host.viewT0) * Math.max(1, part.plotWidth() - 1)
    }
    function finishBox() {
        const a = part.snapped(Math.min(part.host.boxFromMs, part.host.boxToMs))
        const b = part.snapped(Math.max(part.host.boxFromMs, part.host.boxToMs))
        part.host.boxFromMs = NaN
        part.host.boxToMs = NaN
        part.setCursor(a)
        if (b !== a)
            part.setCursorB(b)
        else if (part.host.hasCursorB)
            part.clearCursorB()
    }
    function togglePlotted(index) {
        const next = Object.assign({}, part.host.plotted)
        next[index] = !next[index]
        part.host.plotted = next
        part.host.chart.requestPaint()
    }

    // The plot's width as last drawn, or the chart's before its first
    function plotWidth() {
        return part.host.chart.drawn ? part.host.chart.drawn.plotW : part.host.chart.shownW
    }
    // The plot's left edge in the chart: the panes' axes are left of it
    function plotLeft() {
        return part.host.chart.drawn ? part.host.chart.drawn.left : 0
    }
    function timeAt(x) {
        return part.host.viewT0 + (x - part.host.chart.x - part.plotLeft()) / Math.max(1, part.plotWidth() - 1) * (part.host.viewT1 - part.host.viewT0)
    }
}
