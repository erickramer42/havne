// File/layer: app/qml/RecordingsPage.qml | Qt6 Quick, UI layer
// Testing: BRIDGE: RecordingViewTest (test_bridge); QML: QmlTest.recordings,
//          QmlTest.phone, QmlTest.chartlayout (test_qml); Manual smoke

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import App

pragma ComponentBehavior: Bound

// The recordings a session saved (ADR-0049), newest first: Open shows one
// in the viewer, Replay puts its replay:// URI in Settings' field, and the
// viewer's Replay in Settings the part in view's (?from=&to=). The
// viewer: the recording's signals (ticked ones plotted, the cursor's
// values beside each), the chart over the part in view (drag to pan, the
// wheel or a pinch to zoom, a tap to place the cursor), markers and the
// frames check's findings on it, and the findings listed. A second
// recording can be overlaid, its traces dashed in the first's colors,
// lined up as havne-cli view --align-on lines two up. The vehicle picked
// in Settings keeps a baseline recording (ADR-0049's appendix of
// 2026-11-06): an open recording can become it, and the baseline can be
// overlaid on another, lined up on their engine starts.
Page {
    id: root
    background: Rectangle { color: Theme.bg }

    // Main puts the URI in Settings' field and shows it
    signal replayRequested(string uri)

    // The vehicles, Settings' VehicleBook document: Main binds it, and a
    // baseline set or cleared here goes back through vehiclesEdited for
    // Settings to store, so the page never writes the store itself
    property var vehicles: VehicleBook.empty()
    signal vehiclesEdited(var doc)
    readonly property var pickedVehicle: VehicleBook.picked(vehicles)
    readonly property string baselineFolder: pickedVehicle !== null ? pickedVehicle.baseline : ""
    // The baseline's row (Bridge.recordingRow), null with none set or its
    // folder gone; looked at again at each refresh and open, since a
    // folder can go while the app runs
    property var baselineRow: null
    readonly property bool baselineGone: baselineFolder !== "" && baselineRow === null
    readonly property bool isBaseline: baselineFolder !== "" && recording.folder === baselineFolder
    onBaselineFolderChanged: checkBaseline()

    readonly property int recordingsListed: 20

    // Import: another logger's file made a recording. The file picked, its
    // kind (RecordingImport.Kind: guessed from its first line, switched
    // here), a CSV's column map, and the last import's words: what it
    // made, or why not
    property url importFile: ""
    property int importKind: RecordingImport.Csv
    property url importMap: ""
    property bool importOpen: false
    // A CSV with no column map file is imported through the map guessed
    // from its header (CsvGuessPreview); picking a map file skips the guess
    property bool importGuessing: false
    readonly property bool importUsesGuess: importKind === RecordingImport.Csv
                                            && importMap.toString() === ""
                                            && guessPreview.guess !== null
    // [{ name, url }]: the maps kept under the settings folder's maps/
    property var savedMaps: []
    // After a candump import is refused, the interface field shows: core
    // names a log's interfaces when it has several
    property bool importAsksInterface: false
    property string importNote: ""
    property bool importFailed: false
    // [{ title, uri, folder, source, profile }] (Bridge.recentRecordings)
    property var recordings: []
    property string openedTitle: ""
    property string openedUri: ""
    readonly property bool viewing: recording.state !== RecordingView.Closed

    // The chart maximized: it takes the page but for the zoom row, and the
    // cursor's values move to a strip beside it. Kept apart for a short
    // layout (a phone on its side, where it starts maximized) and a tall
    // one, as Live keeps them. Only while a recording is open
    property bool maximizedTall: false
    property bool maximizedShort: true
    readonly property bool chartMaximized: recording.state === RecordingView.Open
                                           && (Theme.shortLayout ? maximizedShort : maximizedTall)
    function toggleMaximized() {
        if (Theme.shortLayout)
            maximizedShort = !maximizedShort
        else
            maximizedTall = !maximizedTall
    }
    // F on a keyboard; a text field takes its own letters first
    Shortcut {
        sequence: "F"
        enabled: root.visible && recording.state === RecordingView.Open && !Theme.mobile
        onActivated: root.toggleMaximized()
    }
    // The splitter below the chart: its height once dragged, -1 for the
    // page's own (the default), kept between runs
    Settings {
        id: viewerSettings
        category: "recordings"
        property real chartHeight: -1
        // The maximized chart's strip of the cursor's values folded to a
        // tab, so the plot has the width
        property bool stripFolded: false
    }
    property alias chartHeight: viewerSettings.chartHeight
    property alias stripFolded: viewerSettings.stripFolded

    // The part of the axis in view, in ms since the recording started
    // (before it too, with a compared recording that starts earlier), and
    // the cursor's time (NaN for none)
    property real viewT0: 0
    property real viewT1: 1000
    // What the signal area shows: the values at the cursor, or the frames
    // of the part in view (a recording with a frames.csv). The values each
    // time a recording opens
    property string detailView: "values"
    readonly property bool framesShown: detailView === "frames"
                                        && recording.state === RecordingView.Open
                                        && recording.hasFrames
    // The Frames view's By ID table and filter panel
    property bool framesById: false
    property bool framesFilterOpen: false
    onViewT0Changed: refillFramesSoon()
    onViewT1Changed: refillFramesSoon()
    onFramesShownChanged: {
        if (framesShown)
            refillFrames()
        else
            viewFrames.clear()
    }
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
    // The dot marking the cursor's sample on each trace
    readonly property real cursorDotR: 3.5
    readonly property int rowHeight: Theme.dense ? 36 : 40
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
    // display units, after being alignHysteresis beyond it), or "engine"
    // (each engine's first start: RPM reaching 500 after being below 100)
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
        checkBaseline()
    }

    // A URL's last part, as the picker named it (a content:// URI's too)
    function fileNameOf(url) {
        const text = decodeURIComponent(url.toString())
        return text.substring(text.lastIndexOf("/") + 1)
    }

    // A file picked to import: its kind guessed, the rest asked for
    function chooseImportFile(url) {
        importFile = url
        importKind = Bridge.recordingImport.guessKind(url)
        importMap = ""
        importAsksInterface = false
        importInterface.text = ""
        importNote = ""
        importFailed = false
        importOpen = true
        savedMaps = Bridge.recordingImport.savedMaps()
        guessImportFile()
    }

    // A CSV's map guessed from its header, unless a map file is chosen
    function guessImportFile() {
        guessPreview.clear()
        if (importKind !== RecordingImport.Csv || importMap.toString() !== "")
            return
        importGuessing = Bridge.recordingImport.guess(importFile, "")
    }

    // A map file chosen (the dialog, or one saved earlier) instead of the
    // guess
    function chooseImportMap(url) {
        importMap = url
        importNote = ""
        importFailed = false
        guessPreview.clear()
    }

    // Back to the guess after a map file was chosen
    function useTheGuess() {
        importMap = ""
        importNote = ""
        importFailed = false
        guessImportFile()
    }

    readonly property bool importReady: importKind === RecordingImport.Candump
                                        || importMap.toString() !== ""
                                        || (importUsesGuess && guessPreview.ready)

    function startImport() {
        importNote = ""
        importFailed = false
        if (importUsesGuess) {
            Bridge.recordingImport.startWithMap(importFile, guessPreview.mapText())
            return
        }
        Bridge.recordingImport.start(importFile, importKind, importMap,
                                     importKind === RecordingImport.Candump
                                     ? importInterface.text : "")
    }

    // What an import made, in words
    function importedWords(result) {
        if (importKind === RecordingImport.Candump)
            return qsTr("Imported %1 frames over %2 s")
                       .arg(result.frames).arg((result.durationUs / 1e6).toFixed(1))
        return result.columnsIgnored > 0
               ? qsTr("Imported %1 values; columns ignored: %2")
                     .arg(result.values).arg(result.columnsIgnored)
               : qsTr("Imported %1 values").arg(result.values)
    }

    function checkBaseline() {
        // Not a ternary of the map and ({}): qmlcachegen 6.12.0 crashes on it
        if (baselineFolder === "") {
            baselineRow = null
            return
        }
        const row = Bridge.recordingRow(baselineFolder)
        baselineRow = row.folder !== undefined ? row : null
    }
    function useAsBaseline() {
        vehiclesEdited(VehicleBook.setBaseline(vehicles, recording.folder))
    }
    function clearBaseline() {
        vehiclesEdited(VehicleBook.clearBaseline(vehicles))
    }
    // The baseline overlaid on the open recording, lined up on their
    // engine starts
    function compareWithBaseline() {
        checkBaseline()
        if (baselineRow === null)
            return
        alignChoice.currentIndex = alignChoice.indexOfValue("engine")
        setAlignOn("engine")
        compareWith(baselineRow)
    }

    function open(row) {
        checkBaseline()
        openedTitle = row.title
        openedUri = row.uri !== undefined ? row.uri : ""
        recording.open(row.folder)
    }

    // A folder just recorded (a stopped log), opened from the list as a
    // tap on its row would; one the list can't show stays on the list
    function openFolder(folder) {
        if (viewing)
            backToList()
        else
            refresh()
        const row = Bridge.recordingRow(folder)
        if (row.folder !== undefined)
            open(row)
    }

    // Seconds to the ms with no trailing zeros, as core's replay takes them
    function secondsText(ms) {
        return String(Number((ms / 1000).toFixed(3)))
    }

    // The opened recording's replay:// URI for the part in view
    // (ADR-0023's appendix "a stretch replayed"): from and to in the
    // recording's own seconds, none while the whole recording is in view
    function stretchUri() {
        const from = Math.max(0, Math.floor(viewT0))
        const to = Math.min(durationMs(), Math.ceil(viewT1))
        if (from <= 0 && to >= durationMs())
            return openedUri
        return openedUri + (openedUri.indexOf("?") >= 0 ? "&" : "?")
               + "from=" + secondsText(from) + "&to=" + secondsText(to)
    }

    function backToList() {
        closeExport()
        recording.close()
        viewFrames.clear()
        detailView = "values"
        stopComparing()
        alignChoice.currentIndex = 0
        alignOn = "start"
        alignLabel = ""
        alignSignal = ""
        alignLevel = NaN
        alignHysteresis = 0
        openedTitle = ""
        openedUri = ""
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

    // The Frames view's rows again for the part in view, once it has rested
    // for settleMs: a drag or a pinch asks for none until it stops
    function refillFramesSoon() {
        if (framesShown)
            framesRefill.restart()
    }
    function refillFrames() {
        framesRefill.stop()
        if (framesShown)
            viewFrames.load(recording.folder, viewT0, viewT1)
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

    // A tapped time moved to the nearest sample of the first signal
    // plotted, so the cursor stands where the line has a sample and reads
    // the value drawn there; as it is with nothing plotted
    function snapped(t) {
        const first = recording.signalList.find(s => !s.isText && root.plotted[s.index] === true)
        if (!first)
            return t
        const at = recording.nearestSampleMs(first.index, t)
        return at >= 0 ? at : t
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
        // Where that value's sample lies on the axis: a dot marks it
        const labelMsOf = (at, offset) => inView && at && at.text !== "" && at.atMs >= 0
                                          ? at.atMs + offset : NaN
        // The columns lie on a grid from time 0, not from the view's
        // start, so a pan carries each column's samples along with it
        // rather than regrouping them, and a noisy trace's peaks hold
        // still (Live's slices do the same). One more column covers the
        // view's ends; the chart cuts it to the view
        const colMs = (viewT1 - viewT0) / n
        const g0 = Math.floor(viewT0 / colMs) * colMs
        const gn = Math.max(1, Math.ceil((viewT1 - g0) / colMs - 1e-9))
        const g1 = g0 + gn * colMs
        const out = recording.columns(indexes, g0, g1, gn).map(c => {
            const s = sigs[c.index]
            return { columns: { lows: c.lows, highs: c.highs, t0: g0, t1: g1 }, row: c.index,
                     scaleKey: c.index, decimals: s.decimals, unit: s.unit,
                     labelValue: labelOf(root.readout[c.index]),
                     labelMs: labelMsOf(root.readout[c.index], 0) }
        })
        if (!compareAligned)
            return out
        const theirs = compare.signalList
        const pairs = indexes.map(i => ({ mine: i, theirs: root.compareByName[sigs[i].name] }))
                             .filter(p => p.theirs !== undefined && !theirs[p.theirs].isText)
        if (pairs.length === 0)
            return out
        const cols = compare.columns(pairs.map(p => p.theirs), g0 - compareOffsetMs,
                                     g1 - compareOffsetMs, gn)
        for (let k = 0; k < cols.length; ++k) {
            const mine = pairs[k].mine
            const s = theirs[pairs[k].theirs]
            out.push({ columns: { lows: cols[k].lows, highs: cols[k].highs, t0: g0, t1: g1 },
                       row: mine,
                       scaleKey: mine, dash: Theme.seriesCompareDash(mine), decimals: s.decimals,
                       unit: s.unit, labelValue: labelOf(root.compareReadout[mine]),
                       labelMs: labelMsOf(root.compareReadout[mine], compareOffsetMs) })
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
        } else if (alignOn === "engine") {
            what = qsTr("their engine starts")
            here = recording.findEngineStart()
            there = compare.findEngineStart()
        }
        if (here < 0 || there < 0) {
            compareStatus = alignOn === "utc"
                ? (here < 0 && there < 0 ? qsTr("%1 isn't drawn: neither has a UTC start").arg(compareTitle)
                   : here < 0 ? qsTr("%1 isn't drawn: this recording has no UTC start").arg(compareTitle)
                   : qsTr("%1 isn't drawn: it has no UTC start").arg(compareTitle))
                : alignOn === "engine"
                ? (here < 0 && there < 0
                   ? qsTr("%1 isn't drawn: neither has an engine start (RPM reaching 500 after being below 100)")
                   : here < 0 ? qsTr("%1 isn't drawn: this recording has no engine start (RPM reaching 500 after being below 100)")
                   : qsTr("%1 isn't drawn: it has no engine start (RPM reaching 500 after being below 100)"))
                  .arg(compareTitle)
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

    // Where the values can come from (ADR-0049's appendix of 2026-11-06):
    // as recorded (signals.csv), or the frames decoded through the
    // recording's own signal file or a profile the app has loaded
    function decodeChoices() {
        const out = [{ text: qsTr("Values as recorded"), value: "" }]
        if (recording.ownSignalFile)
            out.push({ text: qsTr("Its frames through its own signal file"), value: "signals.json" })
        for (const p of Bridge.decodeProfiles())
            out.push({ text: qsTr("Its frames through %1").arg(p.name !== "" ? p.name : p.profile),
                       value: p.profile })
        return out
    }
    function decodeWith(value) {
        if (value === recording.decodedWith && recording.state === RecordingView.Open)
            return
        if (value === "")
            recording.open(recording.folder)
        else
            recording.openFrames(recording.folder, value)
    }

    function infoText() {
        const info = recording.info
        let text = qsTr("%1 long").arg(lengthText(recording.durationMs))
        if (recording.decodedWith === "signals.json")
            text += qsTr(" · values decoded from its frames through its own signal file")
        else if (recording.decodedWith !== "")
            text += qsTr(" · values decoded from its frames through %1").arg(recording.decodedWith)
        else if (recording.signalList.every(s => s.samples === 0))
            text += qsTr(" · no values were recorded: decode its frames with a profile")
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
        // A folder of values alone (a CSV import): nothing to check
        if (!recording.hasFrames)
            return qsTr("Frames check: the recording has no frames")
        if (!check.ran)
            return qsTr("Frames check: didn't run (%1)").arg(check.error)
        const n = recording.findings.length
        return n === 0 ? qsTr("Frames check: nothing found in %1 frames").arg(check.frames)
                       : n === 1 ? qsTr("Frames check: 1 finding")
                                 : qsTr("Frames check: %1 findings").arg(n)
    }

    // Import's file and a CSV's column map
    FileDialog {
        id: importDialog
        title: qsTr("Import a log")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Logs (*.csv *.log *.txt)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.chooseImportFile(importDialog.selectedFile)
    }
    FileDialog {
        id: mapDialog
        title: qsTr("The logger's column map")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Column maps (*.json)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.chooseImportMap(mapDialog.selectedFile)
    }

    // Save report's file: text first, the one a shop or a buyer reads
    FileDialog {
        id: reportDialog
        title: qsTr("Save the health report")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "txt"
        nameFilters: [qsTr("Text (*.txt)"), qsTr("JSON (*.json)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.saveReportTo(reportDialog.selectedFile,
                                      reportDialog.selectedNameFilter.index)
    }

    // The signal rows' filter: Live has its own
    SignalFilter {
        id: signalFilter
    }

    // The Export panel: it takes the chart's place while open
    property bool exportOpen: false
    readonly property bool exportShown: exportOpen && recording.state === RecordingView.Open
                                        && !chartMaximized
    readonly property var plottedNames: recording.signalList
        .filter(s => !s.isText && root.plotted[s.index] === true).map(s => s.name)
    function openExport() {
        exportOpen = true
        exportPanel.begin()
    }
    function closeExport() {
        exportPanel.end()
        exportOpen = false
    }

    // The last Save report's outcome in words, "" before one on this
    // recording
    property string reportNote: ""
    property bool reportFailed: false

    // The whole recording's health report to url, its format by the
    // dialog's filter or the file's name (ADR-0054's appendix "the health
    // report as built"), naming the picked vehicle's baseline; the view's
    // zoom never narrows it. The result comes back in onReportSaved; false
    // when none started
    function saveReportTo(url, filterIndex) {
        const started = recording.saveHealthReport(url, filterIndex, baselineFolder)
        if (started) {
            reportFailed = false
            reportNote = qsTr("Saving the report…")
        }
        return started
    }

    function reportSaved(r) {
        Bridge.logEvent(r.line)
        reportFailed = !r.ok
        reportNote = r.ok ? qsTr("Report saved: %1").arg(r.summary)
                          : qsTr("Report not saved: %1").arg(r.words)
    }

    RecordingView {
        id: recording
        unitSystem: Bridge.unitSystem
        onReportSaved: result => root.reportSaved(result)
        onStateChanged: {
            root.reportNote = ""
            root.reportFailed = false
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
            root.detailView = "values"
            root.framesById = false
            root.framesFilterOpen = false
            framesFilterPanel.clear()
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
        target: Bridge.recordingImport
        function onGuessFinished(result) {
            root.importGuessing = false
            // The panel was closed, or a map file chosen, meanwhile
            if (!root.importOpen || root.importMap.toString() !== ""
                    || root.importKind !== RecordingImport.Csv)
                return
            if (result.ok) {
                guessPreview.load(result)
            } else {
                root.importFailed = true
                root.importNote = qsTr("%1. Column map... picks a map instead.").arg(result.words)
            }
        }
        function onFinished(result) {
            root.importFailed = !result.ok
            if (result.ok) {
                root.importNote = root.importedWords(result)
                root.importOpen = false
                root.refresh()
            } else {
                root.importNote = result.words
                if (root.importKind === RecordingImport.Candump)
                    root.importAsksInterface = true
            }
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

    // The part in view's frames (ADR-0049's appendix "frames read back")
    RecordingFrames { id: viewFrames }
    Timer {
        id: framesRefill
        interval: root.settleMs
        onTriggered: root.refillFrames()
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

    // The page scrolls when its column needs more height than it has: a
    // phone upright, or any screen turned on its side. Up and down only, and
    // never while the chart pans or pinches, so those stay the chart's
    Flickable {
        id: scroller
        objectName: "recordingsScroll"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: Math.max(height, column.implicitHeight + 2 * Theme.pageMargin)
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height && !pan.active && !pinch.active
        ScrollBar.vertical: ScrollBar {
            policy: scroller.contentHeight > scroller.height ? ScrollBar.AlwaysOn
                                                             : ScrollBar.AsNeeded
        }
    }

    ColumnLayout {
        id: column
        parent: scroller.contentItem
        x: Theme.pageMargin
        y: Theme.pageMargin
        width: scroller.width - 2 * Theme.pageMargin
        height: scroller.contentHeight - 2 * Theme.pageMargin
        spacing: 10

        PageHeader {
            title: qsTr("Recordings")
            visible: !root.chartMaximized
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "importButton"
                visible: !root.viewing
                text: qsTr("Import")
                enabled: !Bridge.recordingImport.busy
                onClicked: importDialog.open()
            }
        }

        // ---- Import: the file picked, its kind, a CSV's column map ----
        ColumnLayout {
            objectName: "importPanel"
            visible: root.importOpen && !root.viewing
            Layout.fillWidth: true
            spacing: Theme.spacing

            Label {
                objectName: "importFileName"
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
                color: Theme.text
                text: qsTr("Import %1").arg(root.fileNameOf(root.importFile))
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                Rectangle {
                    implicitWidth: kindSwitch.implicitWidth + 4
                    implicitHeight: kindSwitch.implicitHeight + 4
                    radius: Theme.radius
                    color: Theme.surface
                    Row {
                        id: kindSwitch
                        anchors.centerIn: parent
                        spacing: 2
                        SegmentButton {
                            objectName: "importKind_csv"
                            text: qsTr("CSV")
                            selected: root.importKind === RecordingImport.Csv
                            enabled: !Bridge.recordingImport.busy
                            onClicked: {
                                root.importKind = RecordingImport.Csv
                                if (guessPreview.guess === null && !root.importGuessing)
                                    root.guessImportFile()
                            }
                        }
                        SegmentButton {
                            objectName: "importKind_candump"
                            text: qsTr("candump")
                            selected: root.importKind === RecordingImport.Candump
                            enabled: !Bridge.recordingImport.busy
                            onClicked: {
                                root.importKind = RecordingImport.Candump
                                root.importNote = ""
                            }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    font.pixelSize: Theme.fontCaption
                    text: qsTr("Guessed from its first line")
                }
            }

            // A CSV's columns come through the logger's column map
            RowLayout {
                visible: root.importKind === RecordingImport.Csv
                Layout.fillWidth: true
                spacing: Theme.spacing
                ThemedButton {
                    objectName: "importMapButton"
                    text: qsTr("Column map...")
                    enabled: !Bridge.recordingImport.busy
                    onClicked: mapDialog.open()
                }
                Label {
                    objectName: "importMapName"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    color: root.importMap.toString() !== "" ? Theme.text : Theme.textDim
                    text: root.importMap.toString() !== "" ? root.fileNameOf(root.importMap)
                                                         : qsTr("Guessed from the header")
                }
            }
            // A map kept earlier, or back to the guess
            RowLayout {
                visible: root.importKind === RecordingImport.Csv
                         && (root.savedMaps.length > 0 || root.importMap.toString() !== "")
                Layout.fillWidth: true
                spacing: Theme.spacing
                ComboBox {
                    id: savedMapChoice
                    objectName: "savedMaps"
                    visible: root.savedMaps.length > 0
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    Layout.preferredHeight: Theme.touchTarget
                    enabled: !Bridge.recordingImport.busy
                    model: [qsTr("Saved maps...")].concat(root.savedMaps.map(m => m.name))
                    currentIndex: 0
                    onActivated: index => {
                        if (index > 0)
                            root.chooseImportMap(root.savedMaps[index - 1].url)
                        currentIndex = 0
                    }
                }
                ThemedButton {
                    objectName: "useTheGuess"
                    visible: root.importMap.toString() !== ""
                    compact: true
                    Layout.preferredHeight: Theme.touchTarget
                    text: qsTr("Use the guess")
                    enabled: !Bridge.recordingImport.busy
                    onClicked: root.useTheGuess()
                }
            }
            Label {
                objectName: "importGuessing"
                visible: root.importGuessing
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textDim
                text: qsTr("Reading the file's header...")
            }
            CsvGuessPreview {
                id: guessPreview
                objectName: "guessPreview"
                visible: root.importKind === RecordingImport.Csv && root.importMap.toString() === ""
                Layout.fillWidth: true
                onMapSaved: root.savedMaps = Bridge.recordingImport.savedMaps()
            }

            // A candump log's bus, once core has said the log holds several
            RowLayout {
                visible: root.importKind === RecordingImport.Candump && root.importAsksInterface
                Layout.fillWidth: true
                spacing: Theme.spacing
                Label { text: qsTr("Interface"); color: Theme.textDim; font.pixelSize: Theme.fontCaption }
                TextField {
                    id: importInterface
                    objectName: "importInterface"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0   // the row's width, not its text's
                    placeholderText: qsTr("can0")
                    font.family: Theme.monoFamily
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    enabled: !Bridge.recordingImport.busy
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                ThemedButton {
                    objectName: "importStart"
                    text: qsTr("Import")
                    highlighted: true
                    enabled: !Bridge.recordingImport.busy && root.importReady
                    onClicked: root.startImport()
                }
                ThemedButton {
                    objectName: "importCancel"
                    text: qsTr("Cancel")
                    enabled: !Bridge.recordingImport.busy
                    onClicked: {
                        root.importOpen = false
                        root.importNote = ""
                    }
                }
                Label {
                    objectName: "importBusy"
                    visible: Bridge.recordingImport.busy
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    text: root.importGuessing ? qsTr("Reading...") : qsTr("Importing...")
                }
            }
        }

        // The last import's words: what it made, or why not
        Label {
            objectName: "importNote"
            visible: root.importNote !== "" && !root.viewing
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.importFailed ? Theme.danger : Theme.success
            text: root.importNote
        }

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

        // A baseline whose folder was moved or deleted says so, on the list
        // and in the viewer, with the way to clear it
        RowLayout {
            objectName: "baselineGoneRow"
            visible: root.baselineGone && !root.chartMaximized
            Layout.fillWidth: true
            spacing: Theme.spacing
            Label {
                objectName: "baselineGone"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.warning
                text: qsTr("%1's baseline recording is gone: its folder was moved or deleted.")
                          .arg(root.pickedVehicle !== null ? root.pickedVehicle.label : "")
            }
            ThemedButton {
                objectName: "clearBaseline"
                compact: true
                text: qsTr("Clear it")
                Accessible.name: qsTr("Clear %1's baseline").arg(root.pickedVehicle !== null
                                                                 ? root.pickedVehicle.label : "")
                onClicked: root.clearBaseline()
            }
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
                            // A row without frames (a CSV import's values) has
                            // nothing to replay; a row that doesn't say has
                            text: recordingRow.modelData.frames === false
                                  ? qsTr("values alone: the recording has no frames to replay")
                                  : recordingRow.modelData.source !== ""
                                  ? qsTr("%1 · replays as %2").arg(recordingRow.modelData.source)
                                                              .arg(recordingRow.modelData.profile)
                                  : qsTr("replays as %1").arg(recordingRow.modelData.profile)
                            color: Theme.textDim
                            font.pointSize: 8
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }
                    // The picked vehicle's baseline
                    Label {
                        objectName: "baselineBadge"
                        visible: root.baselineFolder !== ""
                                 && recordingRow.modelData.folder === root.baselineFolder
                        text: qsTr("Baseline")
                        color: Theme.text
                        font.pointSize: 8
                        leftPadding: 6
                        rightPadding: 6
                        topPadding: 2
                        bottomPadding: 2
                        background: Rectangle {
                            radius: Theme.radius
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.info
                        }
                        Accessible.name: qsTr("%1's baseline").arg(root.pickedVehicle !== null
                                                                  ? root.pickedVehicle.label : "")
                    }
                    ThemedButton {
                        objectName: "replayButton"
                        visible: recordingRow.modelData.frames !== false
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
            visible: root.viewing && !root.chartMaximized
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "backToList"
                text: qsTr("All recordings")
                onClicked: root.backToList()
            }
            Label {
                objectName: "openedTitle"
                // On a phone the name takes a line of its own below
                visible: !Theme.compact
                text: root.openedTitle
                color: Theme.text
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            // The part in view replayed as a session: Settings' field
            // filled as the list's Replay fills it, and Connect left to
            // the user, so the name says where it goes; off while a
            // session runs, as the list's is
            ThemedButton {
                objectName: "replayView"
                visible: recording.state === RecordingView.Open && recording.hasFrames
                enabled: !(Bridge.connected || Bridge.connecting)
                text: qsTr("Replay in Settings")
                Accessible.name: qsTr("Put the part in view in Settings, to replay it as a session")
                onClicked: root.replayRequested(root.stretchUri())
            }
        }

        Label {
            objectName: "openedTitleCompact"
            visible: Theme.compact && root.viewing && !root.chartMaximized
            text: root.openedTitle
            color: Theme.text
            // The folder's date is at the end
            elide: Text.ElideMiddle
            Layout.fillWidth: true
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
            visible: recording.state === RecordingView.Open && !root.chartMaximized
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: recording.state === RecordingView.Open ? root.infoText() : ""
        }

        // The whole recording's health as one scrubbed file to share
        RowLayout {
            visible: recording.state === RecordingView.Open && !root.chartMaximized
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "saveReport"
                compact: Theme.compact
                enabled: !recording.reporting
                text: qsTr("Save report…")
                onClicked: reportDialog.open()
            }
            ThemedButton {
                objectName: "exportButton"
                compact: Theme.compact
                text: qsTr("Export...")
                highlighted: root.exportOpen
                onClicked: {
                    if (root.exportOpen)
                        root.closeExport()
                    else
                        root.openExport()
                }
            }
            // The signal area's two views: the values at the cursor, or the
            // frames in view. Hidden for a recording with no frames.csv
            Rectangle {
                visible: recording.state === RecordingView.Open && recording.hasFrames
                         && !root.chartMaximized
                implicitWidth: detailSwitch.implicitWidth + 4
                implicitHeight: detailSwitch.implicitHeight + 4
                radius: Theme.radius
                color: Theme.surface

                Row {
                    id: detailSwitch
                    anchors.centerIn: parent
                    spacing: 2
                    SegmentButton {
                        objectName: "detailView_values"
                        text: qsTr("Values")
                        selected: !root.framesShown
                        onClicked: root.detailView = "values"
                    }
                    SegmentButton {
                        objectName: "detailView_frames"
                        text: qsTr("Frames")
                        selected: root.framesShown
                        onClicked: root.detailView = "frames"
                    }
                }
            }
            Label {
                objectName: "reportNote"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.reportNote
                color: root.reportFailed ? Theme.danger : Theme.textDim
            }
        }

        RecordingExportPanel {
            id: exportPanel
            objectName: "exportPanel"
            visible: root.exportShown
            Layout.fillWidth: true
            view: recording
            plottedNames: root.plottedNames
            onCloseRequested: root.closeExport()
        }

        // Where the values come from: offered once it opened, or failed
        // (a profile that can't decode it, a folder with frames alone)
        ComboBox {
            id: decodeChoice
            objectName: "decodeChoice"
            // Values alone have no frames to decode through a profile
            visible: (recording.state === RecordingView.Open
                      || recording.state === RecordingView.Failed) && !root.chartMaximized
                     && recording.hasFrames
            Layout.fillWidth: true
            textRole: "text"
            valueRole: "value"
            model: visible ? root.decodeChoices() : []
            currentIndex: Math.max(0, indexOfValue(recording.decodedWith))
            Accessible.name: qsTr("Where the values come from")
            onActivated: index => root.decodeWith(valueAt(index))
        }

        // The picked vehicle's baseline: this recording made it, or the
        // baseline overlaid on it, lined up on their engine starts
        Flow {
            id: baselineFlow
            objectName: "baselineRow"
            visible: recording.state === RecordingView.Open && !root.chartMaximized
                     && root.pickedVehicle !== null
            Layout.fillWidth: true
            spacing: Theme.spacing
            ThemedButton {
                objectName: "useAsBaseline"
                visible: !root.isBaseline
                width: Math.min(implicitWidth, baselineFlow.width)
                text: qsTr("Use as %1's baseline").arg(root.pickedVehicle !== null
                                                        ? root.pickedVehicle.label : "")
                onClicked: root.useAsBaseline()
            }
            Label {
                objectName: "isBaseline"
                visible: root.isBaseline
                width: Math.min(implicitWidth, baselineFlow.width)
                height: Theme.controlHeight
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                color: Theme.textDim
                text: qsTr("%1's baseline").arg(root.pickedVehicle !== null
                                                 ? root.pickedVehicle.label : "")
            }
            ThemedButton {
                objectName: "compareBaseline"
                visible: root.baselineRow !== null && !root.isBaseline
                width: Math.min(implicitWidth, baselineFlow.width)
                text: qsTr("Compare with the baseline")
                Accessible.name: qsTr("Compare with %1's baseline, aligned on the engine starts")
                                     .arg(root.pickedVehicle !== null ? root.pickedVehicle.label : "")
                onClicked: root.compareWithBaseline()
            }
        }

        // Another recording to overlay, and how the two line up
        RowLayout {
            visible: recording.state === RecordingView.Open && !root.chartMaximized
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
            visible: root.comparing && recording.state === RecordingView.Open && !root.chartMaximized
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
                        { text: qsTr("Align a fall"), value: "fall" },
                        { text: qsTr("Align engine starts"), value: "engine" }]
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
            visible: root.comparing && recording.state === RecordingView.Open && !root.chartMaximized
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: root.compareStatus
        }

        // ---- The frames of the part in view, read from frames.csv ----
        ColumnLayout {
            objectName: "viewerFrames"
            visible: root.framesShown && !root.chartMaximized
            Layout.fillWidth: true
            spacing: Theme.spacing

            Flow {
                Layout.fillWidth: true
                spacing: Theme.spacing

                Rectangle {
                    implicitWidth: framesTableSwitch.implicitWidth + 4
                    implicitHeight: framesTableSwitch.implicitHeight + 4
                    radius: Theme.radius
                    color: Theme.surface

                    Row {
                        id: framesTableSwitch
                        anchors.centerIn: parent
                        spacing: 2
                        SegmentButton {
                            objectName: "framesTable_frames"
                            text: qsTr("Frames")
                            selected: !root.framesById
                            onClicked: root.framesById = false
                        }
                        SegmentButton {
                            objectName: "framesTable_byId"
                            text: qsTr("By ID")
                            selected: root.framesById
                            onClicked: {
                                root.framesById = true
                                viewFrames.ids.refresh()
                            }
                        }
                    }
                }
                ThemedButton {
                    objectName: "framesFilterToggle"
                    text: viewFrames.frames.filterTerms > 0
                          ? qsTr("Filter (%1)").arg(viewFrames.frames.filterTerms)
                          : qsTr("Filter")
                    checkable: true
                    checked: root.framesFilterOpen
                    onClicked: root.framesFilterOpen = !root.framesFilterOpen
                }
            }

            FrameFilterPanel {
                id: framesFilterPanel
                visible: root.framesFilterOpen
                frames: viewFrames.frames
            }

            // How many rows the part in view has, and when it has more than
            // the table holds, where the table begins
            Label {
                objectName: "framesWords"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                wrapMode: Text.WordWrap
                color: viewFrames.error !== "" ? Theme.danger : Theme.textDim
                font.pixelSize: Theme.fontCaption
                text: viewFrames.error !== "" ? viewFrames.error
                      : viewFrames.loading ? qsTr("Reading the frames…")
                      : viewFrames.note !== "" ? viewFrames.note
                      : viewFrames.frames.filterTerms > 0
                        ? qsTr("Showing %1 of %2 frames").arg(viewFrames.frames.count)
                                                         .arg(viewFrames.frames.heldCount)
                        : viewFrames.rows === 1 ? qsTr("1 frame in view")
                                               : qsTr("%1 frames in view").arg(viewFrames.rows)
            }

            // A fixed height: the page scrolls and the table scrolls inside
            FrameTable {
                objectName: "viewerFrameTable"
                visible: !root.framesById
                Layout.preferredHeight: 280
                live: false
                frames: viewFrames.frames
                emptyText: viewFrames.loading ? qsTr("Reading the frames…")
                         : viewFrames.frames.heldCount > 0 ? qsTr("No frame the filter shows")
                         : qsTr("No frame in view")
                // A tap puts the chart's cursor at the frame's time, whether
                // or not a signal has a sample there
                onRowClicked: (index, modifiers, timestampUs) => root.setCursor(timestampUs / 1000)
            }
            FrameIdList {
                objectName: "viewerFrameIds"
                visible: root.framesById
                Layout.preferredHeight: 280
                ids: viewFrames.ids
                refreshing: root.framesById && root.framesShown
                emptyText: viewFrames.frames.heldCount > 0 ? qsTr("No frame the filter shows")
                                                           : qsTr("No frame in view")
                onIdTapped: (id, extended) => {
                    framesFilterPanel.showId(id, extended)
                    root.framesFilterOpen = true
                    root.framesById = false
                }
            }
        }

        SignalFilterBar {
            objectName: "recordingFilter"
            visible: recording.state === RecordingView.Open && !root.chartMaximized && !root.framesShown
            Layout.fillWidth: true
            filter: signalFilter
            codes: recording.signalList.map(s => s.group)
        }

        // The recording's signals: a tick plots one, and beside each its
        // value at the cursor
        ListView {
            id: signalRows
            objectName: "signalRows"
            visible: recording.state === RecordingView.Open && !root.chartMaximized && !root.framesShown
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
                              : signalRow.at.label ? signalRow.at.label
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
                              : at.label ? at.label
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
        // While the filter narrows the rows, how many it shows and how many
        // the chart draws: the chart keeps every ticked signal (ADR-0050)
        Label {
            objectName: "filterCount"
            visible: recording.state === RecordingView.Open && signalFilter.active && !root.chartMaximized && !root.framesShown
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: (Theme.compact ? qsTr("%1 of %2 shown · %3 plotted")
                                 : qsTr("Showing %1 of %2 signals · plotting %3"))
                  .arg(root.shownSignals.length).arg(recording.signalList.length)
                  .arg(recording.signalList.filter(s => root.plotted[s.index] === true).length)
        }

        // Compact on a phone: at their desktop widths the buttons, Frame
        // test among them, outgrow its width
        RowLayout {
            visible: recording.state === RecordingView.Open
            Layout.fillWidth: true
            spacing: 6
            ThemedButton {
                objectName: "zoomAll"
                compact: Theme.dense
                text: qsTr("All")
                Layout.maximumWidth: 72
                onClicked: root.showAll()
            }
            ThemedButton {
                objectName: "zoomOut"
                compact: Theme.dense
                text: "−"
                Layout.maximumWidth: 48
                Accessible.name: qsTr("Zoom out")
                onClicked: root.zoomAt((root.viewT0 + root.viewT1) / 2, 2)
            }
            ThemedButton {
                objectName: "zoomIn"
                compact: Theme.dense
                text: "+"
                Layout.maximumWidth: 48
                Accessible.name: qsTr("Zoom in")
                onClicked: root.zoomAt((root.viewT0 + root.viewT1) / 2, 0.5)
            }
            ThemedButton {
                objectName: "frameTest"
                compact: Theme.dense
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
                text: !root.hasCursor ? (Theme.compact ? qsTr("Tap for values")
                                                       : qsTr("Tap the chart for values"))
                                      : qsTr("At %1").arg(root.timeText(root.cursorMs, true))
            }
            ThemedButton {
                objectName: "maximizeChart"
                text: root.chartMaximized ? qsTr("Signals") : qsTr("Chart")
                compact: Theme.dense
                Layout.minimumWidth: 52
                Layout.maximumWidth: 84
                Accessible.name: root.chartMaximized ? qsTr("Show the signals again")
                                                     : qsTr("Give the chart the page")
                onClicked: root.toggleMaximized()
            }
        }

        Rectangle {
            id: chartFrame
            objectName: "chartFrame"
            visible: recording.state === RecordingView.Open && !root.exportShown
            Layout.fillWidth: true
            // Tall enough to read on a phone; the page scrolls to it. The
            // splitter below sets it once dragged (not while maximized)
            readonly property bool sized: viewerSettings.chartHeight > 0 && !root.chartMaximized
            Layout.minimumHeight: sized ? Math.max(200, viewerSettings.chartHeight) : 200
            Layout.preferredHeight: sized ? Math.max(200, viewerSettings.chartHeight) : -1
            Layout.fillHeight: !sized
            color: Theme.surface
            radius: 4
            border.color: Theme.textDim
            border.width: 1

            // Maximized, the cursor's values in a strip beside the chart (below
            // it on a tall frame), as the signal rows show them otherwise; only
            // while the cursor is in view, so until a tap the chart has the page.
            // Its button folds it to a tab and opens it again
            readonly property bool cursorInView: root.hasCursor && root.cursorMs >= root.viewT0
                                                 && root.cursorMs <= root.viewT1
            readonly property bool stripShown: root.chartMaximized && cursorInView
            // Taller than wide (a phone upright) the values go below the
            // chart, a band of cells, since height is what's to spare;
            // wider than tall, beside it in a column
            readonly property bool stripBelow: height > width
            readonly property var stripModel: stripShown
                ? recording.signalList.filter(s => root.plotted[s.index] === true) : []
            readonly property int stripColumns:
                Math.max(1, Math.floor((width - stripFold.width - 16) / 140))
            readonly property real stripWidth: !stripShown || stripBelow ? 0
                : viewerSettings.stripFolded ? stripFold.width + 8
                : Math.max(stripFold.width + 8, Math.min(170, Math.round(width * 0.25)))
            readonly property real stripHeight: !stripShown || !stripBelow ? 0
                : viewerSettings.stripFolded ? stripFold.height + 8
                : Math.max(stripFold.height + 8,
                           Math.min(Math.ceil(stripModel.length / stripColumns) * 40 + 8,
                                    Math.round(height * 0.4)))

            StripChart {
                id: chart
                objectName: "chart"
                x: 4
                y: 4
                shownW: parent.width - 8 - chartFrame.stripWidth
                shownH: parent.height - 8 - chartFrame.stripHeight
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
                    // A dot on each trace at the sample its value comes from
                    for (const tr of l.traces) {
                        if (!isFinite(tr.labelMs))
                            continue
                        const x = xOf(tr.labelMs)
                        if (x < 0 || x > l.plotW)
                            continue
                        ctx.fillStyle = Theme.seriesColor(tr.row)
                        ctx.beginPath()
                        ctx.arc(x, tr.endY, root.cursorDotR, 0, 2 * Math.PI)
                        ctx.fill()
                    }
                }
            }

            // Drag pans, the wheel and a pinch zoom about the pointer, a
            // tap places the cursor. A drag is sideways only, so the page
            // scrolls up and down; one that starts while a pinch is on or
            // just after is the pinch's last finger, and moves nothing
            DragHandler {
                id: pan
                objectName: "chartPan"
                target: null
                yAxis.enabled: false
                property real startT0: 0
                property bool fromPinch: false
                onActiveChanged: {
                    if (!active)
                        return
                    startT0 = root.viewT0
                    fromPinch = pinch.active || Date.now() - pinch.endedAt < 300
                }
                onActiveTranslationChanged: {
                    if (!active || fromPinch) return
                    const span = root.viewT1 - root.viewT0
                    const t0 = startT0 - activeTranslation.x / Math.max(1, root.plotWidth() - 1) * span
                    root.setView(t0, t0 + span)
                }
            }
            PinchHandler {
                id: pinch
                objectName: "chartPinch"
                target: null
                property real lastScale: 1
                property double endedAt: 0
                onActiveChanged: {
                    lastScale = 1
                    if (!active)
                        endedAt = Date.now()
                }
                onActiveScaleChanged: {
                    if (!active || activeScale <= 0) return
                    root.zoomAt(root.timeAt(centroid.position.x), lastScale / activeScale)
                    lastScale = activeScale
                }
            }
            // A notch (120) in shows 0.8 of the span, out 1.25; a
            // high-resolution wheel's or a touchpad's parts of a notch zoom
            // by their share of it, and a sideways scroll doesn't zoom
            WheelHandler {
                target: null
                onWheel: event => {
                    const dy = event.angleDelta.y
                    if (dy !== 0)
                        root.zoomAt(root.timeAt(event.x), Math.pow(0.8, dy / 120))
                }
            }
            DoubleTapHandler {
                onTapped: (eventPoint, button) => root.setCursor(root.snapped(root.timeAt(eventPoint.position.x)))
                // A double-click or double-tap maximizes it, or gives the
                // signals back; the two taps of a thumb may land a little apart
                onDoubleTap: root.toggleMaximized()
            }

            ThemedButton {
                id: stripFold
                objectName: "stripFold"
                visible: chartFrame.stripShown
                compact: true
                // Placed, not anchored: an anchor switched off keeps the
                // size it gave, so the strip's two shapes set x and y
                x: parent.width - 4 - width
                y: chartFrame.stripBelow ? parent.height - 4 - height : 4
                text: chartFrame.stripBelow ? (viewerSettings.stripFolded ? "^" : "v")
                                            : (viewerSettings.stripFolded ? "<" : ">")
                Accessible.name: viewerSettings.stripFolded ? qsTr("Show the values at the cursor")
                                                            : qsTr("Hide the values at the cursor")
                onClicked: viewerSettings.stripFolded = !viewerSettings.stripFolded
            }

            GridView {
                objectName: "cursorStrip"
                visible: chartFrame.stripShown && !viewerSettings.stripFolded
                x: chartFrame.stripBelow ? 4 : parent.width - chartFrame.stripWidth
                y: chartFrame.stripBelow ? parent.height - chartFrame.stripHeight + 4
                                         : stripFold.y + stripFold.height + 4
                width: chartFrame.stripBelow ? stripFold.x - 8 : chartFrame.stripWidth - 4
                height: chartFrame.stripBelow ? chartFrame.stripHeight - 8 : parent.height - 4 - y
                cellWidth: chartFrame.stripBelow ? Math.floor(width / chartFrame.stripColumns) : width
                cellHeight: 40
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: visible ? chartFrame.stripModel : []
                delegate: Item {
                    id: stripRow
                    required property var modelData
                    readonly property var at: root.readout[modelData.index]
                    width: GridView.view.cellWidth
                    height: 40
                    Rectangle {
                        x: 0
                        y: 6
                        width: 8
                        height: 8
                        radius: 2
                        color: Theme.seriesColor(stripRow.modelData.index)
                    }
                    Label {
                        objectName: "stripName"
                        x: 12
                        width: parent.width - 12
                        text: stripRow.modelData.name
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "stripValue"
                        x: 12
                        y: 16
                        width: parent.width - 12
                        text: !stripRow.at || stripRow.at.atMs < 0 ? qsTr("—")
                              : stripRow.modelData.isText ? stripRow.at.text
                              : stripRow.at.label ? stripRow.at.label
                              : root.valueText(Number(stripRow.at.text), stripRow.modelData.decimals,
                                               stripRow.at.unit)
                        color: Theme.text
                        font.family: Theme.monoFamily
                        elide: Text.ElideRight
                    }
                }
            }
        }

        // With the chart gone for the Export panel, the rest stays at the top
        Item {
            visible: root.exportShown
            Layout.fillHeight: true
        }

        // The splitter: drag it to make the chart taller or shorter, kept
        // between runs; a double-tap gives the chart the page's own height
        Item {
            objectName: "chartSplitter"
            visible: recording.state === RecordingView.Open && !root.chartMaximized
                     && !root.exportShown
            Layout.fillWidth: true
            implicitHeight: 12
            Rectangle {
                anchors.centerIn: parent
                width: 40
                height: 4
                radius: 2
                color: chartDrag.active || chartSplitHover.hovered ? Theme.textDim : Theme.outline
            }
            HoverHandler { id: chartSplitHover; cursorShape: Qt.SizeVerCursor }
            DragHandler {
                id: chartDrag
                target: null
                xAxis.enabled: false
                property real startHeight: 0
                onActiveChanged: if (active) startHeight = chartFrame.height
                onActiveTranslationChanged: {
                    if (active)
                        viewerSettings.chartHeight = Math.max(200, startHeight + activeTranslation.y)
                }
            }
            DoubleTapHandler { onDoubleTap: viewerSettings.chartHeight = -1 }
        }

        Label {
            objectName: "frameTestResult"
            visible: Theme.developerMode && root.frameTestResult !== "" && !root.chartMaximized
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
            visible: recording.state === RecordingView.Open && !root.chartMaximized
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: recording.state === RecordingView.Open ? root.checkText() : ""
        }
        ListView {
            id: findingList
            objectName: "findingList"
            visible: recording.state === RecordingView.Open && count > 0 && !root.chartMaximized
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
