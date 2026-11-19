// File/layer: app/qml/RecordingViews.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick

pragma ComponentBehavior: Bound

// The two RecordingViews, the recording and the compared one, and their answers, a part of RecordingsPage

Item {
    id: root
    required property RecordingsPage host
    visible: false
    property alias recording: recording
    property alias compare: compare
    RecordingView {
        id: recording
        objectName: "recordingView"
        unitSystem: Bridge.unitSystem
        onReportSaved: result => root.host.reportPart.reportSaved(result)
        onColumnMeansReady: (request, means) => root.host.seriesPart.differenceAnswered(true, request, means)
        onStatsReady: (request, stats) => {
            if (request !== root.host.statsAsk)
                return
            const byIndex = {}
            for (const s of stats)
                byIndex[s.index] = s
            root.host.spanStats = byIndex
            root.host.chart.repaintMarks()
        }
        onStepReady: (request, atMs) => {
            if (atMs >= 0)
                root.host.cursorsPart.moveKeyed(atMs, root.host.stepsB)
        }
        onStretchesReady: (request, found, ok) => {
            if (request !== root.host.stretchAsk)
                return
            root.host.stretchAsk = 0
            root.host.stretchFailed = !ok
            root.host.stretchList = found
            root.host.jumpIndex = -1
            root.host.alignPullHere = 0
            root.host.pullsHereFailed = !ok
            root.host.pullsHere = found.filter(s => s.kind === "pull")
            if (root.host.comparing)
                root.host.comparePart.align()
        }
        onStateChanged: {
            root.host.seriesPart.forgetDifference()
            root.host.reportNote = ""
            root.host.reportFailed = false
            root.host.pullsHere = null
            root.host.pullsHereFailed = false
            root.host.stretchAsk = 0
            root.host.stretchList = null
            root.host.stretchFailed = false
            root.host.jumpIndex = -1
            root.host.stretchesOpen = false
            root.host.conditionsOpen = false
            root.host.conditionPick = -1
            root.host.alignPullHere = 0
            if (root.recording.state !== RecordingView.Open)
                return
            const defaults = {}
            for (const s of root.recording.signalList)
                defaults[s.index] = s.plotted
            root.host.slots = ({})
            root.host.plotted = defaults
            // A kept arrangement is used again: stamped, so the oldest goes first
            root.host.viewerSettings.views = JSON.stringify(ChartLayout.viewsTouched(
                root.host.viewsBook, root.host.panesPart.viewNameOf(root.recording.folder), Date.now()))
            root.host.readout = ({})
            let rows = 0
            for (const s of root.recording.signalList)
                rows += s.samples
            root.host.rowsPerSecond = rows / (root.host.cursorsPart.durationMs() / 1000)
            root.host.cursorMs = NaN
            root.host.spanHintUp = false
            root.host.cursorsPart.clearCursorB()
            // A decode chosen in the pinned form's About reopens into About
            root.host.detailView = root.host.decodeReopen && root.host.detailView === "about" ? "about" : "values"
            root.host.decodeReopen = false
            root.host.framesById = false
            root.host.framesFilterOpen = false
            root.host.framesFilterPanel.clear()
            root.host.cursorsPart.showAll()
            root.host.comparePart.askStretches()
        }
        onSignalListChanged: {
            root.host.comparePairs = ComparePairs.made(root.recording, root.compare)
            root.host.alignSignal = ComparePairs.alignId(root.recording.signalList, root.host.alignSignal, root.host.alignSignalName)
            // An open, a close or new units: a range set before means nothing
            root.host.paneRanges = ({})
            if (root.host.hasCursor)
                root.host.cursorsPart.setCursor(root.host.cursorMs)
            if (root.host.hasCursorB)
                root.host.cursorsPart.setCursorB(root.host.cursorBMs)
            if (root.recording.state === RecordingView.Open && root.host.stretchUnits !== Bridge.unitSystem)
                root.host.comparePart.reaskStretches()
            root.host.chart.requestPaint()
        }
    }

    RecordingView {
        id: compare
        objectName: "compareView"
        unitSystem: Bridge.unitSystem
        onColumnMeansReady: (request, means) => root.host.seriesPart.differenceAnswered(false, request, means)
        onStretchesReady: (request, found, ok) => {
            if (request !== root.host.pullsThereAsk)
                return
            root.host.pullsThereAsk = 0
            root.host.alignPullThere = 0
            root.host.pullsThereFailed = !ok
            root.host.pullsThere = found
            root.host.comparePart.align()
        }
        onStateChanged: {
            root.host.seriesPart.forgetDifference()
            root.host.pullsThere = null
            root.host.pullsThereFailed = false
            root.host.pullsThereAsk = 0
            root.host.alignPullThere = 0
            root.host.comparePairs = ComparePairs.made(root.recording, root.compare)
            if (root.compare.state === RecordingView.Open) {
                let rows = 0
                for (const s of root.compare.signalList)
                    rows += s.samples
                root.host.compareRowsPerSecond = rows / Math.max(1, root.compare.durationMs / 1000)
            }
            if (root.compare.state !== RecordingView.Opening)
                root.host.comparePart.align()
        }
        onSignalListChanged: {
            root.host.comparePairs = ComparePairs.made(root.recording, root.compare)
            if (root.host.hasCursor)
                root.host.cursorsPart.setCursor(root.host.cursorMs)
        }
    }
}
