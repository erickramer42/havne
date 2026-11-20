// File/layer: app/qml/RecordingFrameTest.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// The frame test (developer mode): a scripted pan and zoom of the chart, timed, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    // Four signals of four panes where the recording has them, so the test
    // draws the most panes the chart shows, then the first others
    function frameTestSignals(list) {
        const numbers = list.filter(s => !s.isText && s.samples > 0)
        const keys = []
        const picked = []
        for (const s of numbers) {
            const key = ChartLayout.paneKey(s.unit)
            if (keys.indexOf(key) < 0 && picked.length < 4) {
                keys.push(key)
                picked.push(s)
            }
        }
        for (const s of numbers) {
            if (picked.length < 4 && picked.indexOf(s) < 0)
                picked.push(s)
        }
        return picked
    }

    function startFrameTest() {
        const four = {}
        for (const s of part.frameTestSignals(part.host.recording.signalList))
            four[s.index] = true
        part.host.frameTest = { begun: Date.now(), frames: [0, 0], plotted: part.host.plotted, signals: Object.keys(four).length,
                      panes: 0 }
        part.host.plotted = four
        part.host.chart.paints = 0
        part.host.chart.paintMs = 0
        part.host.chart.paintMaxMs = 0
        part.host.cursorsPart.setView(0, part.host.cursorsPart.durationMs() / 10)
        part.host.frameTestResult = qsTr("Measuring…")
    }

    function frameStep(elapsedS) {
        const whole = part.host.cursorsPart.durationMs()
        const span = part.host.viewT1 - part.host.viewT0
        if (elapsedS < part.host.frameTestSeconds / 2) {
            // A tenth of the recording a second, back to the start at its end
            let t0 = whole / 10 * elapsedS
            t0 = t0 % Math.max(1, whole - span)
            part.host.cursorsPart.setView(t0, t0 + span)
        } else {
            // In for 0.75 s, out for 0.75 s, about the middle
            const inward = Math.floor((elapsedS - part.host.frameTestSeconds / 2) / 0.75) % 2 === 0
            part.host.cursorsPart.zoomAt((part.host.viewT0 + part.host.viewT1) / 2, inward ? 0.97 : 1 / 0.97)
        }
    }

    function finishFrameTest() {
        const t = part.host.frameTest
        const seconds = (Date.now() - t.begun) / 1000
        const panes = part.host.chart.drawn ? part.host.chart.drawn.panes.length : 0
        part.host.frameTest = null
        part.host.plotted = t.plotted
        part.host.cursorsPart.showAll()
        // Frames shown in each half, panning then zooming
        const half = seconds / 2
        part.host.frameTestResult = qsTr("Frame test, %1 plotted in %8 at %2 px: %3 frames/s shown panning, "
                               + "%4 zooming; %5 charts drawn/s, %6 ms a chart on average, %7 ms at most")
            .arg(t.signals).arg(Math.round(part.host.chart.shownW))
            .arg((t.frames[0] / half).toFixed(1)).arg((t.frames[1] / half).toFixed(1))
            .arg((part.host.chart.paints / seconds).toFixed(1))
            .arg((part.host.chart.paintMs / Math.max(1, part.host.chart.paints)).toFixed(1)).arg(part.host.chart.paintMaxMs).arg(panes === 1 ? qsTr("1 pane") : qsTr("%1 panes").arg(panes))
        Bridge.logEvent(part.host.frameTestResult)
    }
}
