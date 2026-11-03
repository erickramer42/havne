// File/layer: app/qml/RecordingsPage.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_recordings.qml): the list from
//          Bridge.recentRecordings, a recording that ends joining it, Replay
//          handing its URI on, Open reaching the viewer (signals, the
//          default plotted, the chart's series over the view), the cursor's
//          readout, pan and zoom kept inside the recording, a second
//          recording overlaid and aligned (starts, UTC, a marker, a rise or
//          fall), a finding's row centering the view, the signal rows'
//          filter (SignalFilterBar.qml, ADR-0050), a refused folder's
//          error, back to the list;
//          every item inside a phone's width (tst_phone.qml). RecordingView
//          itself: test_bridge (RecordingViewTest); the chart's layout:
//          tst_chartlayout.qml; the drawing is manual smoke (docs/testing.md).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import App

pragma ComponentBehavior: Bound

// The recordings a session saved (ADR-0049), newest first: Open shows one
// in the viewer, Replay puts its replay:// URI in Settings' field. The
// viewer: the recording's signals (ticked ones plotted, the cursor's
// values beside each), the chart over the part in view (drag to pan, the
// wheel or a pinch to zoom, a tap to place the cursor), markers and the
// frames check's findings on it, and the findings listed. A second
// recording can be overlaid, its traces dashed in the first's colors,
// lined up as havne-cli view --align-on lines two up.
Page {
    id: root
    background: Rectangle { color: Theme.bg }

    // Main puts the URI in Settings' field and shows it
    signal replayRequested(string uri)

    readonly property int recordingsListed: 20
    // [{ title, uri, folder, source, profile }] (Bridge.recentRecordings)
    property var recordings: []
    property string openedTitle: ""
    readonly property bool viewing: recording.state !== RecordingView.Closed

    // The part of the axis in view, in ms since the recording started
    // (before it too, with a compared recording that starts earlier), and
    // the cursor's time (NaN for none)
    property real viewT0: 0
    property real viewT1: 1000
    property real cursorMs: NaN
    readonly property bool hasCursor: !isNaN(cursorMs)
    // Signal index -> plotted, from the recording's default at the open
    property var plotted: ({})
    // The signal rows the filter leaves (ADR-0050): the recording's own
    // list while it's off. Every filter property shares one change
    // signal, so reading active re-filters on any change; the ticks are
    // read only under Plotted only, so a tick otherwise keeps the list
    readonly property var shownSignals: {
        const f = signalFilter
        if (!f.active)
            return recording.signalList
        const marks = f.plottedOnly ? root.plotted : ({})
        return recording.signalList.filter(s => f.matches(s.name, s.unit, s.group,
                                                          marks[s.index] === true))
    }
    // Signal index -> { text, unit } at the cursor
    property var readout: ({})
    readonly property real minSpanMs: 200
    readonly property int rowHeight: 40
    // A column narrower than a second reads the recording's raw rows of
    // every signal in it (ADR-0049 section 5); a second or wider, the
    // reader's per-second summaries. Past this many rows in view, columns
    // stay a second wide: a 50-signal 10 Hz hour panned at two pixels a
    // column read about 180,000 rows a frame and drew at 30 frames/s on the
    // laptop (ADR-0049's appendix "the viewer")
    readonly property int rawRowsPerFrame: 20000
    // The recording's rows a second, every signal's together
    property real rowsPerSecond: 0

    // A second recording overlaid (ADR-0049 step 5): its signals of the
    // same names drawn dashed beside the first's, sharing their scales,
    // on the first's axis at compareOffsetMs (its own time plus the
    // offset). alignOn says how the two line up: "start", "utc", "marker"
    // (alignLabel), "rise" or "fall" (alignSignal reaching alignLevel, in
    // display units, after being alignHysteresis beyond it)
    readonly property bool comparing: compare.state !== RecordingView.Closed
    property string compareTitle: ""
    property bool compareAligned: false
    property real compareOffsetMs: 0
    property string compareStatus: ""
    property var compareByName: ({})
    property real compareRowsPerSecond: 0
    // The first's signal index -> the compared one's value at the cursor
    property var compareReadout: ({})
    property string alignOn: "start"
    property string alignLabel: ""
    property string alignSignal: ""
    property real alignLevel: NaN
    property real alignHysteresis: 0
    onAlignOnChanged: align()
    onAlignLabelChanged: align()
    onAlignSignalChanged: align()
    onAlignLevelChanged: align()
    onAlignHysteresisChanged: align()

    // The frame test (developer mode; ADR-0049 section 10 step 5's 60 fps
    // on pan and pinch): four signals plotted, then for frameTestSeconds
    // the view moves every frame, panning a tenth of the recording a second
    // for the first half, then zooming in and out about its middle as a
    // pinch does. It counts the frames the window presents and times each
    // chart paint, and says so on the page and in the activity log.
    property real frameTestSeconds: 6
    property string frameTestResult: ""
    property var frameTest: null
    readonly property bool frameTesting: frameTest !== null

    function startFrameTest() {
        const four = {}
        for (const s of recording.signalList.filter(s => !s.isText && s.samples > 0).slice(0, 4))
            four[s.index] = true
        frameTest = { begun: Date.now(), frames: [0, 0], plotted: plotted, signals: Object.keys(four).length }
        plotted = four
        chart.paints = 0
        chart.paintMs = 0
        chart.paintMaxMs = 0
        setView(0, durationMs() / 10)
        frameTestResult = qsTr("Measuring…")
    }

    function frameStep(elapsedS) {
        const whole = durationMs()
        const span = viewT1 - viewT0
        if (elapsedS < frameTestSeconds / 2) {
            // A tenth of the recording a second, back to the start at its end
            let t0 = whole / 10 * elapsedS
            t0 = t0 % Math.max(1, whole - span)
            setView(t0, t0 + span)
        } else {
            // In for 0.75 s, out for 0.75 s, about the middle
            const inward = Math.floor((elapsedS - frameTestSeconds / 2) / 0.75) % 2 === 0
            zoomAt((viewT0 + viewT1) / 2, inward ? 0.97 : 1 / 0.97)
        }
    }

    function finishFrameTest() {
        const t = frameTest
        const seconds = (Date.now() - t.begun) / 1000
        frameTest = null
        plotted = t.plotted
        showAll()
        // Frames shown in each half, panning then zooming
        const half = seconds / 2
        frameTestResult = qsTr("Frame test, %1 plotted at %2 px: %3 frames/s shown panning, %4 zooming; "
                               + "%5 charts drawn/s, %6 ms a chart on average, %7 ms at most")
            .arg(t.signals).arg(Math.round(chart.shownW))
            .arg((t.frames[0] / half).toFixed(1)).arg((t.frames[1] / half).toFixed(1))
            .arg((chart.paints / seconds).toFixed(1))
            .arg((chart.paintMs / Math.max(1, chart.paints)).toFixed(1)).arg(chart.paintMaxMs)
        Bridge.logEvent(frameTestResult)
    }

    function refresh() {
        recordings = Bridge.recentRecordings(recordingsListed)
    }

    function open(row) {
        openedTitle = row.title
        recording.open(row.folder)
    }

    function backToList() {
        recording.close()
        stopComparing()
        alignChoice.currentIndex = 0
        alignOn = "start"
        alignLabel = ""
        alignSignal = ""
        alignLevel = NaN
        alignHysteresis = 0
        openedTitle = ""
        signalFilter.clear()
        refresh()
    }

    function durationMs() {
        return recording.durationMs > 0 ? recording.durationMs : 1000
    }

    // The axis: the recording, and an aligned compared one on it
    function axisStart() {
        return compareAligned ? Math.min(0, compareOffsetMs) : 0
    }
    function axisEnd() {
        return compareAligned ? Math.max(durationMs(), compareOffsetMs + compare.durationMs)
                              : durationMs()
    }

    // The view, kept inside the axis and no narrower than minSpanMs.
    // A change within settleMs of the last is the view moving (a drag, a
    // pinch, the wheel): the chart draws it without antialiasing until the
    // view rests
    readonly property int settleMs: 150
    function setView(t0, t1) {
        const lo = axisStart()
        const hi = axisEnd()
        const span = Math.min(hi - lo, Math.max(minSpanMs, t1 - t0))
        const start = Math.min(Math.max(lo, t0), hi - span)
        viewT0 = start
        viewT1 = start + span
        if (settle.running)
            chart.moving = true
        settle.restart()
        chart.requestPaint()
    }
    function zoomAt(t, factor) {
        setView(t - (t - viewT0) * factor, t + (viewT1 - t) * factor)
    }
    function showAll() {
        setView(axisStart(), axisEnd())
    }
    // A time placed in the middle of the view, the view's span kept
    function centerOn(t) {
        const half = (viewT1 - viewT0) / 2
        setView(t - half, t + half)
    }

    function setCursor(t) {
        cursorMs = Math.min(Math.max(axisStart(), t), axisEnd())
        const values = {}
        const indexes = recording.signalList.map(s => s.index)
        if (cursorMs >= 0) {
            for (const v of recording.valuesAt(indexes, cursorMs))
                values[v.index] = v
        }
        readout = values
        compareReadout = compareValuesAt(cursorMs)
        chart.requestPaint()
    }

    function togglePlotted(index) {
        const next = Object.assign({}, plotted)
        next[index] = !next[index]
        plotted = next
        chart.requestPaint()
    }

    // The plot's width as last drawn, or the chart's before its first
    function plotWidth() {
        return chart.drawn ? chart.drawn.plotW : chart.shownW
    }
    function timeAt(x) {
        return viewT0 + (x - chart.x) / Math.max(1, plotWidth() - 1) * (viewT1 - viewT0)
    }

    // The chart's series: each plotted number's columns over the view, its
    // label the cursor's value while that's in view, then the compared
    // recording's of the same names, dashed. Two pixels a column, or a
    // second when that would read more than rawRowsPerFrame rows
    function series(widthPx) {
        if (recording.state !== RecordingView.Open)
            return []
        const sigs = recording.signalList
        const indexes = sigs.filter(s => !s.isText && root.plotted[s.index]).map(s => s.index)
        if (indexes.length === 0)
            return []
        const spanS = (viewT1 - viewT0) / 1000
        let n = Math.max(1, Math.floor((chart.drawn ? chart.drawn.plotW : widthPx * 0.8) / 2))
        if (spanS * (rowsPerSecond + (compareAligned ? compareRowsPerSecond : 0)) > rawRowsPerFrame)
            n = Math.max(1, Math.min(n, Math.floor(spanS)))
        const inView = cursorMs >= viewT0 && cursorMs <= viewT1
        const labelOf = at => inView && at && at.text !== "" ? Number(at.text) : NaN
        const out = recording.columns(indexes, viewT0, viewT1, n).map(c => {
            const s = sigs[c.index]
            return { columns: { lows: c.lows, highs: c.highs }, row: c.index, scaleKey: c.index,
                     decimals: s.decimals, unit: s.unit, labelValue: labelOf(root.readout[c.index]) }
        })
        if (!compareAligned)
            return out
        const theirs = compare.signalList
        const pairs = indexes.map(i => ({ mine: i, theirs: root.compareByName[sigs[i].name] }))
                             .filter(p => p.theirs !== undefined && !theirs[p.theirs].isText)
        if (pairs.length === 0)
            return out
        const cols = compare.columns(pairs.map(p => p.theirs), viewT0 - compareOffsetMs,
                                     viewT1 - compareOffsetMs, n)
        for (let k = 0; k < cols.length; ++k) {
            const mine = pairs[k].mine
            const s = theirs[pairs[k].theirs]
            out.push({ columns: { lows: cols[k].lows, highs: cols[k].highs }, row: mine,
                       scaleKey: mine, dash: Theme.seriesCompareDash(mine), decimals: s.decimals,
                       unit: s.unit, labelValue: labelOf(root.compareReadout[mine]) })
        }
        return out
    }

    // The compared recording's values at axis time t, by the first's
    // signal index, the names matched
    function compareValuesAt(t) {
        const out = {}
        if (!compareAligned || isNaN(t) || t - compareOffsetMs < 0)
            return out
        const mine = recording.signalList.filter(s => root.compareByName[s.name] !== undefined)
        const got = compare.valuesAt(mine.map(s => root.compareByName[s.name]), t - compareOffsetMs)
        for (let k = 0; k < got.length; ++k)
            out[mine[k].index] = got[k]
        return out
    }

    function compareWith(row) {
        compareTitle = row.title
        compareAligned = false
        compareStatus = qsTr("Opening %1…").arg(row.title)
        compare.open(row.folder)
    }
    function stopComparing() {
        compare.close()
        compareTitle = ""
        compareStatus = ""
        compareAligned = false
        compareReadout = ({})
        if (recording.state === RecordingView.Open)
            showAll()
    }

    // The first recording's markers' labels, each once, and its number
    // signals' names: what the alignment can be on
    function markerLabels() {
        const out = []
        for (const m of recording.markers) {
            if (out.indexOf(m.label) < 0)
                out.push(m.label)
        }
        return out
    }
    function numberNames() {
        return recording.signalList.filter(s => !s.isText).map(s => s.name)
    }
    function setAlignOn(on) {
        if (on === "marker" && alignLabel === "")
            alignLabel = markerLabels()[0] || ""
        if ((on === "rise" || on === "fall") && alignSignal === "")
            alignSignal = numberNames()[0] || ""
        alignOn = on
    }
    // A number as typed, in the locale or plain; NaN for none
    function numberOf(text) {
        if (text.trim() === "")
            return NaN
        try {
            return Number.fromLocaleString(Qt.locale(), text.trim())
        } catch (e) {
            const v = Number(text.trim())
            return isFinite(v) ? v : NaN
        }
    }

    // Lines the compared recording up on the first: its offset, whether
    // it's drawn, and the status saying so
    function align() {
        compareAligned = false
        compareStatus = compare.state === RecordingView.Failed
                        ? qsTr("%1 can't be opened: %2").arg(compareTitle).arg(compare.error) : ""
        if (compare.state === RecordingView.Open && recording.state === RecordingView.Open)
            alignOpen()
        if (recording.state !== RecordingView.Open)
            return
        showAll()
        if (hasCursor)
            setCursor(cursorMs)
    }
    function alignOpen() {
        let here = 0
        let there = 0
        let what = qsTr("their starts")
        if (alignOn === "utc") {
            what = qsTr("their UTC starts")
            here = recording.info.utcStartMs > 0 ? recording.info.utcStartMs : -1
            there = compare.info.utcStartMs > 0 ? compare.info.utcStartMs : -1
        } else if (alignOn === "marker") {
            what = qsTr("the marker %1").arg(alignLabel)
            here = alignLabel !== "" ? recording.findMarker(alignLabel) : -1
            there = alignLabel !== "" ? compare.findMarker(alignLabel) : -1
        } else if (alignOn === "rise" || alignOn === "fall") {
            const s = recording.signalList.find(x => x.name === alignSignal)
            if (!s || isNaN(alignLevel)) {
                compareStatus = qsTr("Dashed: %1. Choose a signal and type a level to align on").arg(compareTitle)
                return
            }
            const verb = alignOn === "rise" ? qsTr("rising to %1") : qsTr("falling to %1")
            what = qsTr("%1 %2").arg(alignSignal).arg(verb.arg(valueText(alignLevel, s.decimals, s.unit)))
            const j = compareByName[alignSignal]
            here = recording.findThreshold(s.index, alignLevel, alignHysteresis, alignOn === "rise")
            there = j !== undefined ? compare.findThreshold(j, alignLevel, alignHysteresis, alignOn === "rise") : -1
        }
        if (here < 0 || there < 0) {
            compareStatus = alignOn === "utc"
                ? qsTr("%1 isn't drawn: an older recording has no UTC start").arg(compareTitle)
                : here < 0 && there < 0 ? qsTr("%1 isn't drawn: neither has %2").arg(compareTitle).arg(what)
                : here < 0 ? qsTr("%1 isn't drawn: this recording has no %2").arg(compareTitle).arg(what)
                : qsTr("%1 isn't drawn: it has no %2").arg(compareTitle).arg(what)
            return
        }
        compareOffsetMs = alignOn === "utc" ? there - here : here - there
        compareAligned = true
        compareStatus = alignOn === "start" ? qsTr("Dashed: %1, aligned on %2").arg(compareTitle).arg(what)
            : alignOn === "utc" ? qsTr("Dashed: %1, aligned on %2, %3 apart").arg(compareTitle).arg(what)
                                    .arg(timeText(Math.abs(compareOffsetMs), true))
            : qsTr("Dashed: %1, aligned on %2, at %3 here and %4 there").arg(compareTitle).arg(what)
                  .arg(timeText(here, true)).arg(timeText(there, true))
    }

    function valueText(v, decimals, unit) {
        const n = Number(v).toLocaleString(Qt.locale(), 'f', decimals)
        return unit !== "" ? n + " " + unit : n
    }

    // 12.3 s, 1:02.3, 1:02:03; before the start (an aligned axis), -12.3 s
    function timeText(ms, tenths) {
        if (ms < 0)
            return "-" + timeText(-ms, tenths)
        const s = ms / 1000
        if (s < 60)
            return tenths ? qsTr("%1 s").arg(s.toFixed(1)) : qsTr("%1 s").arg(Math.round(s))
        const whole = Math.floor(s)
        const sec = tenths ? (s % 60).toFixed(1).padStart(4, "0") : String(whole % 60).padStart(2, "0")
        if (s < 3600)
            return Math.floor(s / 60) + ":" + sec
        return Math.floor(s / 3600) + ":" + String(Math.floor(s / 60) % 60).padStart(2, "0") + ":" + sec
    }
    function captionOf(seconds) {
        return timeText(seconds * 1000, false)
    }
    function lengthText(ms) {
        const s = Math.round(ms / 1000)
        if (s < 60) return ms < 10000 ? qsTr("%1 s").arg((ms / 1000).toFixed(1)) : qsTr("%1 s").arg(s)
        if (s < 3600) return qsTr("%1 min %2 s").arg(Math.floor(s / 60)).arg(s % 60)
        return qsTr("%1 h %2 min").arg(Math.floor(s / 3600)).arg(Math.floor(s / 60) % 60)
    }

    function idText(f) {
        return "0x" + f.id.toString(16).toUpperCase().padStart(f.extended ? 8 : 3, "0")
    }
    // A finding in a few words (ECUIO_CHECK_* kinds)
    function findingText(f) {
        switch (f.kind) {
        case 1: return qsTr("No frames for %1").arg(lengthText(f.lengthMs))
        case 2: return qsTr("%1 slowed for %2").arg(idText(f)).arg(lengthText(f.lengthMs))
        case 3: return qsTr("%1 first heard late").arg(idText(f))
        case 4: return qsTr("Adapter overflow: frames lost")
        case 5: return qsTr("%1: a message broken off").arg(idText(f))
        case 6: return qsTr("Link: %1").arg(Bridge.statusString(f.code))
        case 7: return qsTr("Asleep for %1").arg(lengthText(f.lengthMs))
        default: return qsTr("Finding of kind %1").arg(f.kind)
        }
    }
    function findingColor(kind) {
        return kind === 7 ? Theme.info : kind === 1 || kind === 4 || kind === 5 ? Theme.danger : Theme.warning
    }

    function infoText() {
        const info = recording.info
        let text = qsTr("%1 long").arg(lengthText(recording.durationMs))
        if (info.utcStartMs > 0)
            text += qsTr(" · started %1").arg(new Date(info.utcStartMs).toLocaleString(Qt.locale(), Locale.ShortFormat))
        if (info.recordsDropped > 0)
            text += qsTr(" · %1 records dropped while recording").arg(info.recordsDropped)
        if (info.noSummary)
            text += qsTr(" · it ended without its summary (the app or the power stopped)")
        return text
    }
    function checkText() {
        const check = recording.check
        if (!check.ran)
            return qsTr("Frames check: didn't run (%1)").arg(check.error)
        const n = recording.findings.length
        return n === 0 ? qsTr("Frames check: nothing found in %1 frames").arg(check.frames)
                       : n === 1 ? qsTr("Frames check: 1 finding")
                                 : qsTr("Frames check: %1 findings").arg(n)
    }

    // The signal rows' filter: Live has its own
    SignalFilter {
        id: signalFilter
    }

    RecordingView {
        id: recording
        unitSystem: Bridge.unitSystem
        onStateChanged: {
            if (recording.state !== RecordingView.Open)
                return
            const defaults = {}
            for (const s of recording.signalList)
                defaults[s.index] = s.plotted
            root.plotted = defaults
            root.readout = ({})
            let rows = 0
            for (const s of recording.signalList)
                rows += s.samples
            root.rowsPerSecond = rows / (root.durationMs() / 1000)
            root.cursorMs = NaN
            root.showAll()
        }
        onSignalListChanged: {
            if (root.hasCursor)
                root.setCursor(root.cursorMs)
            chart.requestPaint()
        }
    }

    RecordingView {
        id: compare
        unitSystem: Bridge.unitSystem
        onStateChanged: {
            if (compare.state === RecordingView.Open) {
                const names = {}
                let rows = 0
                for (const s of compare.signalList) {
                    names[s.name] = s.index
                    rows += s.samples
                }
                root.compareByName = names
                root.compareRowsPerSecond = rows / Math.max(1, compare.durationMs / 1000)
            }
            if (compare.state !== RecordingView.Opening)
                root.align()
        }
        onSignalListChanged: {
            if (root.hasCursor)
                root.setCursor(root.cursorMs)
        }
    }

    Connections {
        target: Bridge
        // A recording that ended joins the list
        function onRecordingStateChanged() {
            if (!Bridge.recordingActive)
                root.refresh()
        }
    }
    onVisibleChanged: if (visible && !viewing) refresh()
    Component.onCompleted: refresh()

    Timer {
        id: settle
        interval: root.settleMs
        onTriggered: chart.moving = false
    }

    FrameAnimation {
        running: root.frameTesting
        onTriggered: {
            const elapsed = (Date.now() - root.frameTest.begun) / 1000
            if (elapsed >= root.frameTestSeconds)
                root.finishFrameTest()
            else
                root.frameStep(elapsed)
        }
    }
    Connections {
        target: root.Window.window
        enabled: root.frameTesting
        function onFrameSwapped() {
            const zooming = (Date.now() - root.frameTest.begun) / 1000 >= root.frameTestSeconds / 2
            root.frameTest.frames[zooming ? 1 : 0] += 1
        }
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 10

        PageHeader { title: qsTr("Recordings") }

        // ---- The list ----
        Label {
            objectName: "recordingsHint"
            visible: !root.viewing
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: root.recordings.length > 0
                  ? qsTr("Newest first. Open one to look through it, or replay it as a session.")
                  : qsTr("No recordings yet: turn Record on in Settings, then connect.")
        }

        ListView {
            id: recordingList
            objectName: "recordingList"
            visible: !root.viewing
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            currentIndex: -1
            highlightFollowsCurrentItem: false
            boundsBehavior: Flickable.StopAtBounds
            model: root.recordings
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: recordingRow
                required property var modelData
                width: recordingList.width
                height: Math.max(56, rowLayout.implicitHeight + 2 * Theme.spacing)
                radius: Theme.radius
                color: rowArea.hovered ? Theme.elevatedLight : Theme.elevated

                // The row opens the recording: a button under the labels, so
                // UI Automation and a screen reader can press it
                AbstractButton {
                    id: rowArea
                    objectName: "recordingRowArea"
                    anchors.fill: parent
                    hoverEnabled: true
                    Accessible.name: qsTr("Open %1").arg(recordingRow.modelData.title)
                    onClicked: root.open(recordingRow.modelData)
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }

                RowLayout {
                    id: rowLayout
                    anchors.fill: parent
                    anchors.margins: Theme.spacing
                    spacing: Theme.spacing

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            objectName: "recordingTitle"
                            text: recordingRow.modelData.title
                            color: Theme.text
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            objectName: "recordingDetail"
                            text: recordingRow.modelData.source !== ""
                                  ? qsTr("%1 · replays as %2").arg(recordingRow.modelData.source)
                                                              .arg(recordingRow.modelData.profile)
                                  : qsTr("replays as %1").arg(recordingRow.modelData.profile)
                            color: Theme.textDim
                            font.pointSize: 8
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }
                    ThemedButton {
                        objectName: "replayButton"
                        text: qsTr("Replay")
                        // Darker than its row, so it reads as a button there
                        baseColor: Theme.surface
                        enabled: !Bridge.connected && !Bridge.connecting
                        Layout.maximumWidth: 96
                        Accessible.name: qsTr("Replay %1").arg(recordingRow.modelData.title)
                        onClicked: root.replayRequested(recordingRow.modelData.uri)
                    }
                }
            }
        }

        // ---- The viewer ----
        RowLayout {
            visible: root.viewing
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "backToList"
                text: qsTr("All recordings")
                onClicked: root.backToList()
            }
            Label {
                objectName: "openedTitle"
                text: root.openedTitle
                color: Theme.text
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        RowLayout {
            visible: recording.state === RecordingView.Opening
            Layout.fillWidth: true
            ProgressBar {
                objectName: "openProgress"
                value: recording.progress
                Layout.fillWidth: true
            }
            ThemedButton {
                objectName: "cancelOpen"
                text: qsTr("Cancel")
                onClicked: root.backToList()
            }
        }

        Label {
            objectName: "openError"
            visible: recording.state === RecordingView.Failed
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.danger
            text: qsTr("This recording can't be opened: %1").arg(recording.error)
        }

        Label {
            objectName: "recordingInfo"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: recording.state === RecordingView.Open ? root.infoText() : ""
        }

        // Another recording to overlay, and how the two line up
        RowLayout {
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            spacing: Theme.spacing
            ComboBox {
                id: compareChoice
                objectName: "compareChoice"
                readonly property var others: root.recordings.filter(r => r.folder !== recording.folder)
                Layout.fillWidth: true
                model: [root.comparing ? root.compareTitle : qsTr("Compare with…")]
                       .concat(others.map(r => r.title))
                enabled: others.length > 0
                Accessible.name: qsTr("Compare with another recording")
                onActivated: index => {
                    if (index > 0)
                        root.compareWith(others[index - 1])
                    currentIndex = 0
                }
            }
            ThemedButton {
                objectName: "stopCompare"
                visible: root.comparing
                text: qsTr("Stop")
                Accessible.name: qsTr("Stop comparing")
                onClicked: root.stopComparing()
            }
        }
        Flow {
            visible: root.comparing && recording.state === RecordingView.Open
            Layout.fillWidth: true
            spacing: 6
            ComboBox {
                id: alignChoice
                objectName: "alignChoice"
                width: 170
                textRole: "text"
                valueRole: "value"
                model: [{ text: qsTr("Align the starts"), value: "start" },
                        { text: qsTr("Align UTC starts"), value: "utc" },
                        { text: qsTr("Align a marker"), value: "marker" },
                        { text: qsTr("Align a rise"), value: "rise" },
                        { text: qsTr("Align a fall"), value: "fall" }]
                onActivated: root.setAlignOn(currentValue)
            }
            ComboBox {
                objectName: "alignMarker"
                visible: root.alignOn === "marker"
                width: 150
                model: recording.state === RecordingView.Open ? root.markerLabels() : []
                onActivated: root.alignLabel = currentText
            }
            ComboBox {
                objectName: "alignSignal"
                visible: root.alignOn === "rise" || root.alignOn === "fall"
                width: 150
                model: recording.state === RecordingView.Open ? root.numberNames() : []
                onActivated: root.alignSignal = currentText
            }
            TextField {
                objectName: "alignLevel"
                visible: root.alignOn === "rise" || root.alignOn === "fall"
                width: 100
                placeholderText: qsTr("Level")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                onEditingFinished: root.alignLevel = root.numberOf(text)
            }
            TextField {
                objectName: "alignHysteresis"
                visible: root.alignOn === "rise" || root.alignOn === "fall"
                width: 100
                placeholderText: qsTr("Hysteresis")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                onEditingFinished: {
                    const v = root.numberOf(text)
                    root.alignHysteresis = v >= 0 ? v : 0
                }
            }
        }
        Label {
            objectName: "compareStatus"
            visible: root.comparing && recording.state === RecordingView.Open
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: root.compareStatus
        }

        SignalFilterBar {
            objectName: "recordingFilter"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            filter: signalFilter
            codes: recording.signalList.map(s => s.group)
        }

        // The recording's signals: a tick plots one, and beside each its
        // value at the cursor
        ListView {
            id: signalRows
            objectName: "signalRows"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            Layout.preferredHeight: count * root.rowHeight
            Layout.maximumHeight: Math.max(root.rowHeight, column.height * 0.3)
            clip: true
            currentIndex: -1
            highlightFollowsCurrentItem: false
            boundsBehavior: Flickable.StopAtBounds
            model: root.shownSignals
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: signalRow
                required property var modelData
                required property int index
                readonly property int signalIndex: modelData.index
                readonly property var at: root.readout[signalIndex]
                width: signalRows.width
                height: root.rowHeight
                color: index % 2 === 0 ? Theme.surface : Theme.bg

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 12
                    CheckBox {
                        objectName: "plotCheck"
                        checked: root.plotted[signalRow.signalIndex] === true
                        enabled: !signalRow.modelData.isText
                        onToggled: root.togglePlotted(signalRow.signalIndex)
                        Accessible.name: qsTr("Plot %1").arg(signalRow.modelData.name)
                    }
                    Rectangle {
                        objectName: "swatch"
                        readonly property bool shown: root.plotted[signalRow.signalIndex] === true
                        implicitWidth: 10; implicitHeight: 10
                        radius: 2
                        color: shown ? Theme.seriesColor(signalRow.signalIndex) : "transparent"
                        border.width: 1
                        border.color: Theme.seriesColor(signalRow.signalIndex)
                        opacity: shown ? 1.0 : 0.5
                    }
                    Label {
                        text: signalRow.modelData.name
                        color: Theme.text
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "cursorValue"
                        text: !signalRow.at || signalRow.at.atMs < 0 ? qsTr("—")
                              : signalRow.modelData.isText ? signalRow.at.text
                              : root.valueText(Number(signalRow.at.text), signalRow.modelData.decimals,
                                               signalRow.at.unit)
                        Layout.maximumWidth: Math.round(signalRow.width * (root.compareAligned ? 0.3 : 0.5))
                        elide: Text.ElideRight
                        color: Theme.text
                        font.family: Theme.monoFamily
                    }
                    // The compared recording's, at the cursor
                    Label {
                        objectName: "compareValue"
                        readonly property var at: root.compareReadout[signalRow.signalIndex]
                        visible: root.compareAligned
                        text: !at || at.atMs < 0 ? qsTr("—")
                              : signalRow.modelData.isText ? at.text
                              : root.valueText(Number(at.text), signalRow.modelData.decimals, at.unit)
                        Layout.maximumWidth: Math.round(signalRow.width * 0.3)
                        elide: Text.ElideRight
                        color: Theme.textDim
                        font.family: Theme.monoFamily
                    }
                }
            }
        }

        // Zoom, and where the cursor is
        RowLayout {
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            spacing: 6
            ThemedButton {
                objectName: "zoomAll"
                text: qsTr("All")
                Layout.maximumWidth: 72
                onClicked: root.showAll()
            }
            ThemedButton {
                objectName: "zoomOut"
                text: "−"
                Layout.maximumWidth: 48
                Accessible.name: qsTr("Zoom out")
                onClicked: root.zoomAt((root.viewT0 + root.viewT1) / 2, 2)
            }
            ThemedButton {
                objectName: "zoomIn"
                text: "+"
                Layout.maximumWidth: 48
                Accessible.name: qsTr("Zoom in")
                onClicked: root.zoomAt((root.viewT0 + root.viewT1) / 2, 0.5)
            }
            ThemedButton {
                objectName: "frameTest"
                visible: Theme.developerMode
                enabled: !root.frameTesting
                text: qsTr("Frame test")
                Layout.maximumWidth: 120
                onClicked: root.startFrameTest()
            }
            Label {
                objectName: "cursorTime"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                color: Theme.textDim
                text: !root.hasCursor ? qsTr("Tap the chart to read the values at a time")
                                      : qsTr("At %1").arg(root.timeText(root.cursorMs, true))
            }
        }

        Rectangle {
            id: chartFrame
            objectName: "chartFrame"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 140
            color: Theme.surface
            radius: 4
            border.color: Theme.textDim
            border.width: 1

            StripChart {
                id: chart
                objectName: "chart"
                x: 4
                y: 4
                shownW: parent.width - 8
                shownH: parent.height - 8
                emptyText: qsTr("No signals plotted; tick a row to plot it")
                fetch: function(widthPx) { return root.series(widthPx) }
                layoutOpts: ({
                    gapMs: Theme.staleMs, minSpanUnits: 20, format: root.valueText,
                    span: { t0: root.viewT0, t1: root.viewT1 }, absolute: true,
                    caption: root.captionOf })
                // Findings as bars along the plot's foot, markers as dashed
                // lines with their labels, the cursor as a solid line
                decorate: function(ctx, l) {
                    const span = root.viewT1 - root.viewT0
                    const xOf = t => (t - root.viewT0) / span * (l.plotW - 1)
                    for (const f of recording.findings) {
                        const x0 = xOf(f.atMs), x1 = xOf(f.atMs + f.lengthMs)
                        if (x1 < 0 || x0 > l.plotW) continue
                        ctx.fillStyle = root.findingColor(f.kind)
                        const left = Math.max(0, x0)
                        ctx.fillRect(left, l.top + l.plotH - 4, Math.max(2, Math.min(l.plotW, x1) - left), 4)
                    }
                    ctx.strokeStyle = Theme.info
                    ctx.fillStyle = Theme.info
                    ctx.lineWidth = 1
                    ctx.setLineDash([4, 3])
                    ctx.textAlign = "left"
                    ctx.textBaseline = "top"
                    for (const m of recording.markers) {
                        const x = xOf(m.timeMs)
                        if (x < 0 || x > l.plotW) continue
                        ctx.beginPath()
                        ctx.moveTo(x, l.top)
                        ctx.lineTo(x, l.top + l.plotH)
                        ctx.stroke()
                        ctx.fillText(m.label, x + 3, l.top + 1)
                    }
                    ctx.setLineDash([])
                    if (root.cursorMs >= root.viewT0 && root.cursorMs <= root.viewT1) {
                        ctx.strokeStyle = Theme.text
                        ctx.beginPath()
                        ctx.moveTo(xOf(root.cursorMs), l.top)
                        ctx.lineTo(xOf(root.cursorMs), l.top + l.plotH)
                        ctx.stroke()
                    }
                }
            }

            // Drag pans, the wheel and a pinch zoom about the pointer, a
            // tap places the cursor
            DragHandler {
                id: pan
                target: null
                property real startT0: 0
                onActiveChanged: if (active) startT0 = root.viewT0
                onActiveTranslationChanged: {
                    if (!active) return
                    const span = root.viewT1 - root.viewT0
                    const t0 = startT0 - activeTranslation.x / Math.max(1, root.plotWidth() - 1) * span
                    root.setView(t0, t0 + span)
                }
            }
            PinchHandler {
                target: null
                property real lastScale: 1
                onActiveChanged: lastScale = 1
                onActiveScaleChanged: {
                    if (!active || activeScale <= 0) return
                    root.zoomAt(root.timeAt(centroid.position.x), lastScale / activeScale)
                    lastScale = activeScale
                }
            }
            WheelHandler {
                target: null
                onWheel: event => root.zoomAt(root.timeAt(event.x), event.angleDelta.y > 0 ? 0.8 : 1.25)
            }
            TapHandler {
                onTapped: (eventPoint, button) => root.setCursor(root.timeAt(eventPoint.position.x))
            }
        }

        Label {
            objectName: "frameTestResult"
            visible: Theme.developerMode && root.frameTestResult !== ""
                     && recording.state === RecordingView.Open
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: root.frameTestResult
        }

        // The frames check: what it found, each row centering the view on
        // its finding
        Label {
            objectName: "checkSummary"
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: recording.state === RecordingView.Open ? root.checkText() : ""
        }
        ListView {
            id: findingList
            objectName: "findingList"
            visible: recording.state === RecordingView.Open && count > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(count, 3) * 32
            clip: true
            currentIndex: -1
            highlightFollowsCurrentItem: false
            boundsBehavior: Flickable.StopAtBounds
            model: recording.findings
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: findingRow
                required property var modelData
                width: findingList.width
                height: 32
                color: findingArea.hovered ? Theme.elevatedLight : "transparent"
                AbstractButton {
                    id: findingArea
                    objectName: "findingRowArea"
                    anchors.fill: parent
                    hoverEnabled: true
                    Accessible.name: qsTr("Show %1").arg(root.findingText(findingRow.modelData))
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    onClicked: {
                        const f = findingRow.modelData
                        root.centerOn(f.atMs + f.lengthMs / 2)
                        root.setCursor(f.atMs)
                    }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    Rectangle {
                        implicitWidth: 10; implicitHeight: 4
                        color: root.findingColor(findingRow.modelData.kind)
                    }
                    Label {
                        text: root.timeText(findingRow.modelData.atMs, true)
                        color: Theme.textDim
                        font.family: Theme.monoFamily
                    }
                    Label {
                        objectName: "findingText"
                        text: root.findingText(findingRow.modelData)
                        color: Theme.text
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}
