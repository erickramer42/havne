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
// in the viewer, Replay connects it as a session in one tap, and the
// viewer's Replay the part in view (?from=&to=). The
// viewer: the recording's signals (ticked ones plotted, the cursor's
// values beside each), the chart over the part in view (drag to pan, the
// wheel or a pinch to zoom, a tap to place the cursor, a long press or
// Shift+click the second, whose span gets its statistics), markers and the
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
    // Cancel was pressed on an import under way, until it ends; and the
    // last import ended so, which is neither made nor failed
    property bool importStopping: false
    property bool importCanceled: false
    property bool importFailed: false
    // [{ title, uri, folder, source, profile }] (Bridge.recentRecordings)
    property var recordings: []
    property string openedTitle: ""
    property string openedUri: ""
    readonly property bool viewing: recording.state !== RecordingView.Closed
    // A replay can start: no session running, starting or ending, and no
    // relay holding the adapter (ADR-0047), as Settings' Connect judges it
    readonly property bool sessionIdle: !Bridge.connected && !Bridge.connecting
                                        && !Bridge.disconnecting && !Relay.running
    // Replay's words: what to do first, or plain while a session starts or
    // ends (connected is false once a disconnect begins)
    readonly property string replayWhy: Relay.running ? qsTr("Stop the relay to replay")
                                      : Bridge.connected ? qsTr("Disconnect to replay") : ""
    readonly property string replayText: replayWhy !== "" ? replayWhy : qsTr("Replay")
    // A Replay button's spoken name: its own, then the reason it is off
    function replayName(name) {
        return replayWhy !== "" ? qsTr("%1: %2").arg(name).arg(replayWhy) : name
    }

    // The chart maximized: it takes the page but for the zoom row, and the
    // cursor's values move to a strip beside it. Kept apart for a short
    // layout (a phone on its side, where it starts maximized) and a tall
    // one, as Live keeps them. Only while a recording is open
    property bool maximizedTall: false
    property bool maximizedShort: true
    readonly property bool chartMaximized: recording.state === RecordingView.Open
                                           && (Theme.shortLayout ? maximizedShort : maximizedTall)
    // The zoom row's room for the views' switch while maximized: a phone on
    // its side or the desktop; under it the buttons would push the page wide
    readonly property bool switchInZoomRow: column.width >= 480
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
    // The arrows step a cursor a sample of the snap's signal, B says which
    // cursor they move, Home and End go to the recording's ends
    readonly property bool cursorKeys: root.visible && recording.state === RecordingView.Open
                                       && !Theme.mobile
    Shortcut {
        sequence: "Left"
        enabled: root.cursorKeys
        onActivated: root.stepCursor(false)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.cursorKeys
        onActivated: root.stepCursor(true)
    }
    Shortcut {
        sequence: "Home"
        enabled: root.cursorKeys
        onActivated: root.cursorToEnd(false)
    }
    Shortcut {
        sequence: "End"
        enabled: root.cursorKeys
        onActivated: root.cursorToEnd(true)
    }
    Shortcut {
        sequence: "B"
        enabled: root.cursorKeys
        onActivated: root.arrowsMoveB = root.hasCursorB && !root.arrowsMoveB
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
        // The panes' heights once a seam is dragged, as JSON: a share per
        // pane, kept by the count of panes ({ "3": [0.5, 0.25, 0.25] })
        property string paneShares: "{}"
        // The second cursor's hint has been shown
        property bool spanHintSeen: false
    }
    property alias chartHeight: viewerSettings.chartHeight
    property alias stripFolded: viewerSettings.stripFolded
    property alias spanHintSeen: viewerSettings.spanHintSeen
    // The chart's height until the splitter sets one: on a phone 40 % of
    // the page, no less than three panes and the time axis, its handles' height
    readonly property real phoneChartFloor: 3 * ChartLayout.paneMinH + 2 * ChartLayout.paneGap
                                            + ChartLayout.top + ChartLayout.axisHCursor
                                            + ChartLayout.bottomPad + 8
    readonly property real defaultChartHeight: phoneShape ? Math.max(phoneChartFloor, scroller.height * 0.4)
                                                          : Math.max(200, scroller.height * 0.45)
    // Pinned, the page never scrolls: the chart gives way down to 200 so
    // the view's list keeps this much under the splitter
    readonly property real pinnedListMin: 2 * rowHeight

    // The chart's panes (ADR-0049's viewer, panes by unit): at most 3 on a
    // phone's page, 4 maximized or on the desktop. A pane's range is the
    // user's once zoomed or panned (paneRanges, by pane key, dropped at an
    // open, a close and a change of units); the heights' shares are kept
    readonly property int paneLimit: phoneShape && !chartMaximized ? 3 : ChartLayout.paneMax
    property var paneRanges: ({})
    readonly property var paneShares: {
        try {
            const v = JSON.parse(viewerSettings.paneShares)
            return v !== null && typeof v === "object" && !Array.isArray(v) ? v : {}
        } catch (e) {
            return {}
        }
    }
    function setPaneRange(key, range) {
        const next = Object.assign({}, paneRanges)
        if (range)
            next[key] = { vMin: range.vMin, vMax: range.vMax }
        else
            delete next[key]
        paneRanges = next
        chart.requestPaint()
    }
    function setPaneShares(count, shares) {
        const next = Object.assign({}, paneShares)
        if (shares)
            next[count] = shares
        else
            delete next[count]
        viewerSettings.paneShares = JSON.stringify(next)
        chart.requestPaint()
    }
    // The drawn panes' places, assigned only when they change, so the
    // seams' and axes' handlers aren't made again at every paint
    property var paneBoxes: []
    function notePanes(l) {
        const boxes = !l ? [] : l.panes.map((p, i) => ({ index: i, key: p.key, axis: p.axis,
                                                          top: p.top, h: p.h, left: l.left,
                                                          plotW: l.plotW }))
        if (JSON.stringify(boxes) !== JSON.stringify(paneBoxes))
            paneBoxes = boxes
    }
    // What a double tap at x, y on the chart means: a pane's axis (a touch
    // wide on a phone) returns its range to automatic, a seam's band evens
    // the heights, anywhere else maximizes
    function axisStripWidth(l) {
        return phoneShape ? Math.max(l.left, Theme.touchTarget) : l.left
    }
    function doubleTapAt(x, y) {
        const l = chart.drawn
        if (l && l.panes.length > 1) {
            for (const sy of l.seams) {
                if (Math.abs(y - sy) <= ChartLayout.paneGap / 2 + 8 && x >= l.left) {
                    setPaneShares(l.panes.length, null)
                    return "seam"
                }
            }
        }
        if (l && l.left > 0 && x < axisStripWidth(l)) {
            const i = ChartLayout.paneAt(l, y)
            if (i >= 0 && l.panes[i].axis) {
                setPaneRange(l.panes[i].key, null)
                return "axis"
            }
        }
        toggleMaximized()
        return "maximize"
    }

    // The part of the axis in view, in ms since the recording started
    // (before it too, with a compared recording that starts earlier), and
    // the cursor's time (NaN for none)
    property real viewT0: 0
    property real viewT1: 1000
    // What the signal area shows: the values at the cursor, or the frames
    // of the part in view (a recording with a frames.csv). The values each
    // time a recording opens
    property string detailView: "values"
    // True from a decode choice until the recording opens again
    property bool decodeReopen: false
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
    // The second cursor (NaN for none). Each keeps its own place, so one can
    // be anchored while the other walks past it; what reads out of the two
    // goes by time, the earlier as A
    property real cursorBMs: NaN
    readonly property bool hasCursorB: !isNaN(cursorBMs)
    readonly property bool hasSpan: hasCursor && hasCursorB && cursorMs !== cursorBMs
    readonly property real spanFromMs: Math.min(cursorMs, cursorBMs)
    readonly property real spanToMs: Math.max(cursorMs, cursorBMs)
    // Which cursor the keys move
    property bool arrowsMoveB: false
    // Signal index -> { text, unit } at the second cursor
    property var readoutB: ({})
    // The span's ends' values, by time
    readonly property var readoutFrom: hasSpan && cursorBMs < cursorMs ? readoutB : readout
    readonly property var readoutTo: hasSpan && cursorBMs < cursorMs ? readout : readoutB
    // Signal index -> RecordingView.stats' map over the span
    property var spanStats: ({})
    property int statsAsk: 0
    // Which cursor the step under way moves
    property bool stepsB: false
    // The extremes' dots as last drawn
    property var spanDots: []
    // The box a Shift+drag or a long-press drag draws, NaN between drags
    property real boxFromMs: NaN
    property real boxToMs: NaN
    // The plotted numbers: the statistics' rows and the extremes' dots
    readonly property var spanRows: hasSpan
        ? recording.signalList.filter(s => !s.isText && root.plotted[s.index] === true) : []
    // The first cursor's hint for the second, shown once
    property bool spanHintUp: false
    readonly property bool spanLineUp: recording.state === RecordingView.Open && !exportShown
                                       && (hasSpan || (spanHintUp && hasCursor))
    Timer {
        objectName: "spanHintTimer"
        interval: 6000
        running: root.spanHintUp
        onTriggered: root.spanHintUp = false
    }
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
    // A phone's targets are Theme.touchTarget; the desktop keeps its sizes
    readonly property bool phoneShape: Theme.mobile || Theme.compact
    // The buttons, boxes and fields below take it, as under Settings' pages
    readonly property real touchMin: phoneShape ? Theme.touchTarget : 0
    readonly property real rowHeight: phoneShape ? Theme.touchTarget : Theme.dense ? 36 : 40
    readonly property real segmentHeight: phoneShape ? Theme.touchTarget : 30

    // A phone held upright (the phone viewer's structure): the title row, the
    // chart and its splitter stay put, and under them one list scrolls, the
    // shown view's controls in its head. Not maximized, on its side, on the
    // desktop or while a panel (Export) has the page
    readonly property bool pinnedViewer: phoneShape && !Theme.shortLayout && !chartMaximized
                                         && recording.state === RecordingView.Open && !exportShown
    // On that form About is a view of the switch
    readonly property bool aboutShown: pinnedViewer && detailView === "about"
    // Each view's list head, noted as its list makes it
    property Item valuesHead: null
    property Item valuesFoot: null
    property Item framesHead: null
    property Item idsHead: null
    property Item aboutHead: null
    readonly property Item currentHead: !pinnedViewer ? null
                                      : aboutShown ? aboutHead
                                      : framesShown ? (framesById ? idsHead : framesHead)
                                      : valuesHead
    function noteHead(role, head) {
        if (role === "values") valuesHead = head
        else if (role === "frames") framesHead = head
        else if (role === "ids") idsHead = head
        else aboutHead = head
    }
    // A view's list head on the pinned form: the zoom row, the switch and
    // the view's own controls move into its slots, so they scroll away
    // with the list; elsewhere it is empty and takes no height
    component ViewHead: ColumnLayout {
        id: head
        // values, frames, ids or about
        required property string role
        readonly property alias zoomSlot: zoomSlotItem
        readonly property alias switchSlot: switchSlotItem
        readonly property alias toolsSlot: toolsSlotItem
        // The list it heads. A ListView keeps its contentY when its header's
        // height changes, so a head that grows at the list's start would
        // slide under the list's top: the list goes back to its start then,
        // unless it was scrolled on. While a change waits for that, a contentY
        // within the head's height of the origin is the change's, not a scroll
        property ListView list: null
        property bool listAtStart: true
        property bool settling: false
        visible: root.pinnedViewer
        height: visible ? implicitHeight + 10 : 0
        spacing: 10
        function settle() {
            head.settling = true
            Qt.callLater(head.toStart)
        }
        function toStart() {
            head.settling = false
            if (head.visible && head.list !== null && head.list.visible && head.listAtStart)
                head.list.positionViewAtBeginning()
        }
        onHeightChanged: head.settle()
        Connections {
            target: head.list
            function onContentYChanged() {
                head.listAtStart = head.list.contentY
                                   <= head.list.originY + 1 + (head.settling ? head.height : 0)
            }
            function onHeightChanged() { head.settle() }
            function onOriginYChanged() { head.settle() }
            function onVisibleChanged() { head.settle() }
        }
        ColumnLayout {
            id: zoomSlotItem
            Layout.fillWidth: true
            spacing: 0
        }
        RowLayout {
            id: switchSlotItem
            Layout.fillWidth: true
        }
        ColumnLayout {
            id: toolsSlotItem
            Layout.fillWidth: true
            spacing: 0
        }
        Component.onCompleted: root.noteHead(role, head)
    }
    Component {
        id: framesHeadComponent
        ViewHead { role: "frames"; list: viewerFrameTable.list }
    }
    Component {
        id: idsHeadComponent
        ViewHead { role: "ids"; list: viewerFrameIds.list }
    }
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
    // display units, after being alignHysteresis beyond it), "engine"
    // (each engine's first start: RPM reaching 500 after being below 100)
    // or "pull" (the start of a pull chosen in each: alignPullHere of
    // pullsHere on alignPullThere of pullsThere)
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
    // Each recording's pulls in time order (RecordingView.stretches; null
    // until its answer), and the place of the one chosen in each
    property var pullsHere: null
    property var pullsThere: null
    // An ask that failed (the file changed since the open): not a
    // recording with no pull
    property bool pullsHereFailed: false
    property bool pullsThereFailed: false
    property int pullsHereAsk: 0
    property int pullsThereAsk: 0
    property int alignPullHere: 0
    property int alignPullThere: 0
    onAlignPullHereChanged: align()
    onAlignPullThereChanged: align()
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
        for (const s of frameTestSignals(recording.signalList))
            four[s.index] = true
        frameTest = { begun: Date.now(), frames: [0, 0], plotted: plotted, signals: Object.keys(four).length,
                      panes: 0 }
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
        const panes = chart.drawn ? chart.drawn.panes.length : 0
        frameTest = null
        plotted = t.plotted
        showAll()
        // Frames shown in each half, panning then zooming
        const half = seconds / 2
        frameTestResult = qsTr("Frame test, %1 plotted in %8 at %2 px: %3 frames/s shown panning, "
                               + "%4 zooming; %5 charts drawn/s, %6 ms a chart on average, %7 ms at most")
            .arg(t.signals).arg(Math.round(chart.shownW))
            .arg((t.frames[0] / half).toFixed(1)).arg((t.frames[1] / half).toFixed(1))
            .arg((chart.paints / seconds).toFixed(1))
            .arg((chart.paintMs / Math.max(1, chart.paints)).toFixed(1)).arg(chart.paintMaxMs).arg(panes === 1 ? qsTr("1 pane") : qsTr("%1 panes").arg(panes))
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
        if (!importGuessing) {
            importFailed = true
            importNote = qsTr("The file couldn't be read. Column map... picks a map instead.")
        }
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
        const started = importUsesGuess
            ? Bridge.recordingImport.startWithMap(importFile, guessPreview.mapText())
            : Bridge.recordingImport.start(importFile, importKind, importMap,
                                           importKind === RecordingImport.Candump
                                           ? importInterface.text : "")
        if (!started) {
            importFailed = true
            importNote = qsTr("Not imported: the import couldn't start")
        }
    }

    // What an import made, in words
    function importedWords(result) {
        if (importKind === RecordingImport.Candump)
            return qsTr("Imported %1 frames over %2 s")
                       .arg(result.frames).arg((result.durationUs / 1e6).toFixed(1))
        const words = result.columnsIgnored > 0
                      ? qsTr("Imported %1 values; columns ignored: %2")
                            .arg(result.values).arg(result.columnsIgnored)
                      : qsTr("Imported %1 values").arg(result.values)
        if (!(result.rowsNoTime > 0))
            return words
        // A row with no time has nowhere to go: core leaves it out, counted.
        // havne-cli's words; after a count of columns a comma would read as its
        const left = result.rowsNoTime === 1
                     ? qsTr("1 row with no time left out")
                     : qsTr("%1 rows with no time left out").arg(result.rowsNoTime)
        return words + (result.columnsIgnored > 0 ? "; " : ", ") + left
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
        decodeReopen = false
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
    function snapSignal() {
        return recording.signalList.find(s => !s.isText && root.plotted[s.index] === true)
    }
    function snapped(t) {
        const first = snapSignal()
        if (!first)
            return t
        const at = recording.nearestSampleMs(first.index, t)
        return at >= 0 ? at : t
    }
    function valuesAtTime(t) {
        const values = {}
        if (t >= 0) {
            for (const v of recording.valuesAt(recording.signalList.map(s => s.index), t))
                values[v.index] = v
        }
        return values
    }
    function setCursor(t) {
        cursorMs = Math.min(Math.max(axisStart(), t), axisEnd())
        readout = valuesAtTime(cursorMs)
        compareReadout = compareValuesAt(cursorMs)
        if (!hasCursorB && !spanHintSeen) {
            spanHintSeen = true
            spanHintUp = true
        }
        askSpanStats()
        chart.requestPaint()
    }
    function setCursorB(t) {
        cursorBMs = Math.min(Math.max(axisStart(), t), axisEnd())
        readoutB = valuesAtTime(cursorBMs)
        spanHintUp = false
        askSpanStats()
        chart.requestPaint()
    }
    function clearCursorB() {
        cursorBMs = NaN
        readoutB = ({})
        spanStats = ({})
        statsAsk = 0
        arrowsMoveB = false
        chart.requestPaint()
    }
    // The second gesture at time t: with no first cursor yet it places that
    function placeSecond(t) {
        if (hasCursor)
            setCursorB(t)
        else
            setCursor(t)
    }
    function askSpanStats() {
        if (!hasSpan) {
            statsAsk = 0
            spanStats = ({})
            return
        }
        statsAsk = recording.stats(recording.signalList.map(s => s.index), spanFromMs, spanToMs)
        if (statsAsk === 0)
            spanStats = ({})
    }
    function zoomToSpan() {
        const pad = (spanToMs - spanFromMs) * 0.05
        setView(spanFromMs - pad, spanToMs + pad)
    }
    // Keeps a cursor a key moved in the view
    function bringIntoView(t) {
        if (t < viewT0 || t > viewT1)
            centerOn(t)
    }
    function moveKeyed(t, isB) {
        if (isB)
            setCursorB(t)
        else
            setCursor(t)
        bringIntoView(t)
    }
    function stepCursor(forward) {
        const s = snapSignal()
        const from = arrowsMoveB ? cursorBMs : cursorMs
        if (!s || isNaN(from))
            return
        stepsB = arrowsMoveB
        recording.step(s.index, from, forward)
    }
    function cursorToEnd(last) {
        moveKeyed(snapped(last ? durationMs() : 0), arrowsMoveB)
    }
    // A cursor's handle let go: on a sample, and one dropped on the other
    // leaves a single cursor
    function dropHandle(isB) {
        const t = snapped(isB ? cursorBMs : cursorMs)
        if (isB)
            setCursorB(t)
        else
            setCursor(t)
        if (hasCursorB && Math.abs(xOfTime(cursorMs) - xOfTime(cursorBMs)) <= handleDropPx)
            clearCursorB()
    }
    readonly property real handleDropPx: 4
    // A time's x in the chart's frame
    function xOfTime(t) {
        return chart.x + plotLeft() + (t - viewT0) / (viewT1 - viewT0) * Math.max(1, plotWidth() - 1)
    }
    function finishBox() {
        const a = snapped(Math.min(boxFromMs, boxToMs))
        const b = snapped(Math.max(boxFromMs, boxToMs))
        boxFromMs = NaN
        boxToMs = NaN
        setCursor(a)
        if (b !== a)
            setCursorB(b)
        else if (hasCursorB)
            clearCursorB()
    }
    // A change, signed, in a signal's decimals
    function changeText(v, decimals, unit) {
        return v > 0 ? "+" + valueText(v, decimals, unit) : valueText(v, decimals, unit)
    }
    // A statistic of a row, or a dash with none
    function statText(v, s) {
        return isFinite(v) ? valueText(v, s.decimals, s.unit) : qsTr("—")
    }
    function statChange(s) {
        return s && isFinite(s.change) ? changeText(s.change, s.decimals, s.unit) : qsTr("—")
    }
    function statRate(s) {
        return s && isFinite(s.rate)
               ? changeText(s.rate, s.decimals + 1, s.unit !== "" ? s.unit + "/s" : "/s") : qsTr("—")
    }
    // A value at one of the span's ends, as the rows show it
    function atText(at, s) {
        return !at || at.atMs < 0 ? qsTr("—")
               : s.isText ? at.text
               : at.label ? at.label
               : valueText(Number(at.text), s.decimals, at.unit)
    }
    // The second line a row and a strip's cell show with a span
    function spanCaption(s) {
        const stat = spanStats[s.index]
        return s.isText ? qsTr("B %1").arg(atText(readoutTo[s.index], s))
                        : qsTr("B %1 · Δ %2").arg(atText(readoutTo[s.index], s)).arg(statChange(stat))
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
    // The plot's left edge in the chart: the panes' axes are left of it
    function plotLeft() {
        return chart.drawn ? chart.drawn.left : 0
    }
    function timeAt(x) {
        return viewT0 + (x - chart.x - plotLeft()) / Math.max(1, plotWidth() - 1) * (viewT1 - viewT0)
    }

    // The chart's series: each plotted number's columns over the view, its
    // label the cursor's value while that's in view, then the compared
    // recording's of the same names, dashed. Two pixels a column, or a
    // second when that would read more than rawRowsPerFrame rows
    function series(widthPx) {
        if (recording.state !== RecordingView.Open)
            return []
        // A plotted signal with no samples in this recording has nothing to
        // draw here, so its columns aren't read; the compared recording's
        // of the same name still draw. With nothing drawn the chart says so
        const sigs = recording.signalList
        const plotted = sigs.filter(s => !s.isText && root.plotted[s.index]).map(s => s.index)
        if (plotted.length === 0)
            return []
        const indexes = plotted.filter(i => sigs[i].samples > 0)
        const spanS = (viewT1 - viewT0) / 1000
        let n = Math.max(1, Math.floor((chart.drawn ? chart.drawn.plotW : widthPx * 0.8) / 2))
        if (spanS * (rowsPerSecond + (compareAligned ? compareRowsPerSecond : 0)) > rawRowsPerFrame)
            n = Math.max(1, Math.min(n, Math.floor(spanS)))
        // The labels follow the readout's A, the earlier of the two cursors
        // (the compared recording's readout is the first cursor's alone)
        const inView = cursorMs >= viewT0 && cursorMs <= viewT1
        const fromMs = hasSpan ? spanFromMs : cursorMs
        const fromInView = fromMs >= viewT0 && fromMs <= viewT1
        const labelOf = (at, shown) => shown && at && at.text !== "" ? Number(at.text) : NaN
        // Where that value's sample lies on the axis: a dot marks it
        const labelMsOf = (at, offset, shown) => shown && at && at.text !== "" && at.atMs >= 0
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
        const out = indexes.length === 0 ? [] : recording.columns(indexes, g0, g1, gn).map(c => {
            const s = sigs[c.index]
            return { columns: { lows: c.lows, highs: c.highs, t0: g0, t1: g1 }, row: c.index,
                     scaleKey: c.index, decimals: s.decimals, unit: s.unit,
                     labelValue: labelOf(root.readoutFrom[c.index], fromInView),
                     labelMs: labelMsOf(root.readoutFrom[c.index], 0, fromInView) }
        })
        if (!compareAligned)
            return out
        const theirs = compare.signalList
        const pairs = plotted.map(i => ({ mine: i, theirs: root.compareByName[sigs[i].name] }))
                             .filter(p => p.theirs !== undefined && !theirs[p.theirs].isText
                                          && theirs[p.theirs].samples > 0)
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
                       unit: s.unit, labelValue: labelOf(root.compareReadout[mine], inView),
                       labelMs: labelMsOf(root.compareReadout[mine], compareOffsetMs, inView) })
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
    // Asks each recording for its pulls, once: the answers align again
    function askPulls() {
        if (pullsHere === null && pullsHereAsk === 0)
            pullsHereAsk = recording.stretches(true, false)
        if (pullsThere === null && pullsThereAsk === 0)
            pullsThereAsk = compare.stretches(true, false)
    }
    // A recording's pulls as the choices' rows: its number, start and length
    function pullRows(pulls, here) {
        return (pulls || []).map((p, i) =>
            (here ? qsTr("Pull %1 here · %2 · %3") : qsTr("Pull %1 there · %2 · %3"))
                .arg(i + 1).arg(timeText(p.fromMs, true)).arg(lengthText(p.toMs - p.fromMs)))
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
        } else if (alignOn === "pull") {
            if (pullsHere === null || pullsThere === null) {
                askPulls()
                compareStatus = qsTr("Dashed: %1. Finding each recording's pulls…").arg(compareTitle)
                return
            }
            if (pullsHereFailed || pullsThereFailed) {
                compareStatus = (pullsHereFailed && pullsThereFailed
                                 ? qsTr("%1 isn't drawn: neither recording's pulls could be read")
                                 : pullsHereFailed ? qsTr("%1 isn't drawn: this recording's pulls couldn't be read")
                                 : qsTr("%1 isn't drawn: its pulls couldn't be read")).arg(compareTitle)
                return
            }
            what = qsTr("pull %1 here and pull %2 there").arg(alignPullHere + 1).arg(alignPullThere + 1)
            here = alignPullHere < pullsHere.length ? pullsHere[alignPullHere].fromMs : -1
            there = alignPullThere < pullsThere.length ? pullsThere[alignPullThere].fromMs : -1
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
                : alignOn === "pull"
                ? (here < 0 && there < 0
                   ? qsTr("%1 isn't drawn: neither recording has a pull (full throttle for a second while it accelerates)")
                   : here < 0 ? qsTr("%1 isn't drawn: this recording has no pull (full throttle for a second while it accelerates)")
                   : qsTr("%1 isn't drawn: it has no pull (full throttle for a second while it accelerates)"))
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
        return kind === 7 ? Theme.textDim : kind === 1 || kind === 4 || kind === 5 ? Theme.danger : Theme.warning
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
        decodeReopen = true
        if (value === "")
            recording.open(recording.folder)
        else
            recording.openFrames(recording.folder, value)
    }

    // A row's second line: the address it was recorded from and the
    // profile it replays under, the profile only where the address
    // doesn't name it already. A row without frames (a CSV import's
    // values) has nothing to replay; a row that doesn't say has
    function rowDetail(row) {
        if (row.frames === false)
            return qsTr("values alone: the recording has no frames to replay")
        if (row.source === "")
            return qsTr("replays as %1").arg(row.profile)
        // The address's own profile, or its scheme's default (mock:// is mock)
        const m = /[?&]profile=([^&]*)/.exec(row.source)
        const own = m !== null ? decodeURIComponent(m[1])
                  : row.source.toLowerCase().startsWith("mock://") ? "mock" : ""
        if (own === row.profile)
            return row.source
        return qsTr("%1 · replays as %2").arg(row.source).arg(row.profile)
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
    // The plotted signals, when none of them draws: no values in this
    // recording, nor in the compared one's signal of the same name. The
    // chart then has nothing to draw and says so
    readonly property var plottedNoValues: {
        const plotted = recording.signalList.filter(s => !s.isText && root.plotted[s.index] === true)
        return plotted.length > 0 && plotted.every(s => !root.draws(s)) ? plotted.map(s => s.name) : []
    }
    function draws(s) {
        if (s.samples > 0)
            return true
        if (!compareAligned)
            return false
        const theirs = compare.signalList[root.compareByName[s.name]]
        return theirs !== undefined && !theirs.isText && theirs.samples > 0
    }
    function noValuesText(names) {
        const list = names.length <= 2 ? names.join(qsTr(" and "))
                   : names.slice(0, -1).join(", ") + qsTr(" and ") + names[names.length - 1]
        return names.length === 1 ? qsTr("%1 has no values in this recording").arg(list)
                                  : qsTr("%1 have no values in this recording").arg(list)
    }
    // The panel opens under the title row: the page goes to its top first
    function openExport() {
        exportOpen = true
        exportPanel.begin()
        scroller.contentY = 0
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
        onStatsReady: (request, stats) => {
            if (request !== root.statsAsk)
                return
            const byIndex = {}
            for (const s of stats)
                byIndex[s.index] = s
            root.spanStats = byIndex
            chart.repaintMarks()
        }
        onStepReady: (request, atMs) => {
            if (atMs >= 0)
                root.moveKeyed(atMs, root.stepsB)
        }
        onStretchesReady: (request, found, ok) => {
            if (request !== root.pullsHereAsk)
                return
            root.pullsHereAsk = 0
            root.alignPullHere = 0
            root.pullsHereFailed = !ok
            root.pullsHere = found
            root.align()
        }
        onStateChanged: {
            root.reportNote = ""
            root.reportFailed = false
            root.pullsHere = null
            root.pullsHereFailed = false
            root.pullsHereAsk = 0
            root.alignPullHere = 0
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
            root.spanHintUp = false
            root.clearCursorB()
            // A decode chosen in the pinned form's About reopens into About
            root.detailView = root.decodeReopen && root.detailView === "about" ? "about" : "values"
            root.decodeReopen = false
            root.framesById = false
            root.framesFilterOpen = false
            framesFilterPanel.clear()
            root.showAll()
        }
        onSignalListChanged: {
            // An open, a close or new units: a range set before means nothing
            root.paneRanges = ({})
            if (root.hasCursor)
                root.setCursor(root.cursorMs)
            if (root.hasCursorB)
                root.setCursorB(root.cursorBMs)
            chart.requestPaint()
        }
    }

    RecordingView {
        id: compare
        unitSystem: Bridge.unitSystem
        onStretchesReady: (request, found, ok) => {
            if (request !== root.pullsThereAsk)
                return
            root.pullsThereAsk = 0
            root.alignPullThere = 0
            root.pullsThereFailed = !ok
            root.pullsThere = found
            root.align()
        }
        onStateChanged: {
            root.pullsThere = null
            root.pullsThereFailed = false
            root.pullsThereAsk = 0
            root.alignPullThere = 0
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
            root.importStopping = false
            root.importCanceled = result.canceled === true
            root.importFailed = !result.ok && !root.importCanceled
            if (result.ok) {
                root.importNote = root.importedWords(result)
                root.importOpen = false
                root.refresh()
            } else if (root.importCanceled) {
                // The panel stays, its choices kept, for another try
                root.importNote = qsTr("Import canceled: nothing was imported")
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
        contentHeight: root.pinnedViewer ? height
                                         : Math.max(height, column.implicitHeight + 2 * Theme.pageMargin)
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        interactive: !root.pinnedViewer && contentHeight > height && !pan.active && !pinch.active
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
            objectName: "recordingsHeader"
            title: qsTr("Recordings")
            // On a phone the tab names the page, and the name is the viewer's title
            visible: !root.chartMaximized && !(root.viewing && Theme.compact)
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
                            tapHeight: root.segmentHeight
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
                            tapHeight: root.segmentHeight
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
                    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                             implicitContentHeight + topPadding + bottomPadding,
                                             implicitIndicatorHeight + topPadding + bottomPadding, root.touchMin)
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
                ThemedTextField {
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
                    // An import under way is stopped; a guess is short and
                    // runs to its end
                    enabled: !root.importGuessing && !root.importStopping
                    onClicked: {
                        if (Bridge.recordingImport.busy) {
                            root.importStopping = true
                            Bridge.recordingImport.cancel()
                            return
                        }
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
                    text: root.importGuessing ? qsTr("Reading...")
                          : root.importStopping ? qsTr("Stopping...") : qsTr("Importing...")
                }
            }
        }

        // The last import's words: what it made, or why not
        Label {
            objectName: "importNote"
            visible: root.importNote !== "" && !root.viewing
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.importFailed ? Theme.danger
                   : root.importCanceled ? Theme.textDim : Theme.success
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
                            text: root.rowDetail(recordingRow.modelData)
                            color: Theme.textDim
                            font.pixelSize: Theme.fontCaption
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
                        font.pixelSize: Theme.fontCaption
                        leftPadding: 6
                        rightPadding: 6
                        topPadding: 2
                        bottomPadding: 2
                        background: Rectangle {
                            radius: Theme.radius
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.textDim
                        }
                        Accessible.name: qsTr("%1's baseline").arg(root.pickedVehicle !== null
                                                                  ? root.pickedVehicle.label : "")
                    }
                    ThemedButton {
                        objectName: "replayButton"
                        visible: recordingRow.modelData.frames !== false
                        // One tap connects; during a session it says what to do first
                        text: root.replayText
                        // Darker than its row, so it reads as a button there
                        baseColor: Theme.surface
                        enabled: root.sessionIdle
                        Layout.maximumWidth: enabled ? 96 : 160
                        Accessible.name: root.replayName(qsTr("Replay %1").arg(recordingRow.modelData.title))
                        onClicked: root.replayRequested(recordingRow.modelData.uri)
                    }
                }
            }
        }

        // ---- The viewer: the chart and its signals first, then what
        // applies to the whole recording under "About this recording" ----
        RowLayout {
            objectName: "viewerTitleRow"
            visible: root.viewing && !root.chartMaximized
            Layout.fillWidth: true
            spacing: Theme.spacing
            // On a phone a quiet back arrow at the left edge, where a back
            // control is looked for
            ThemedButton {
                objectName: "backToList"
                flat: Theme.compact
                text: Theme.compact ? "←" : qsTr("All recordings")   // a left arrow
                Accessible.name: qsTr("All recordings")
                Layout.preferredWidth: Theme.compact ? Theme.touchTarget : -1
                onClicked: root.backToList()
            }
            Label {
                objectName: "openedTitle"
                text: root.openedTitle
                color: Theme.text
                font.pixelSize: Theme.fontEmphasis
                // The folder's date is at the end
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            // Replay and Save report...: here on the desktop, in About on a
            // phone, side by side
            RowLayout {
                id: titleActions
                visible: !Theme.compact
                spacing: Theme.spacing
                RowLayout {
                    parent: Theme.compact ? aboutActions : titleActions
                    visible: recording.state === RecordingView.Open
                    Layout.fillWidth: Theme.compact
                    spacing: Theme.spacing
                    // The part in view replayed as a session in one tap, as
                    // the list's Replay does; off while a session runs, as
                    // the list's is
                    ThemedButton {
                        objectName: "replayView"
                        visible: recording.hasFrames
                        compact: Theme.compact
                        Layout.fillWidth: Theme.compact
                        Layout.preferredWidth: Theme.compact ? 1 : -1
                        enabled: root.sessionIdle
                        text: root.replayText
                        Accessible.name: root.replayName(qsTr("Replay the part in view as a session"))
                        onClicked: root.replayRequested(root.stretchUri())
                    }
                    ThemedButton {
                        objectName: "saveReport"
                        compact: Theme.compact
                        Layout.fillWidth: Theme.compact
                        Layout.preferredWidth: Theme.compact ? 1 : -1
                        enabled: !recording.reporting
                        text: qsTr("Save report…")
                        onClicked: reportDialog.open()
                    }
                }
            }
            ThemedButton {
                objectName: "exportButton"
                visible: recording.state === RecordingView.Open
                compact: Theme.compact
                text: qsTr("Export…")
                // Selected while its panel is open: Save as... is the primary there
                checkable: true
                checked: root.exportOpen
                onClicked: {
                    if (root.exportOpen)
                        root.closeExport()
                    else
                        root.openExport()
                }
            }
        }

        ColumnLayout {
            id: topNote
            visible: !Theme.compact && root.reportNote !== ""
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

        RecordingExportPanel {
            id: exportPanel
            objectName: "exportPanel"
            visible: root.exportShown
            Layout.fillWidth: true
            view: recording
            plottedNames: root.plottedNames
            onCloseRequested: root.closeExport()
        }

        Rectangle {
            id: chartFrame
            objectName: "chartFrame"
            // Maximized with Frames chosen, the table has the page instead
            visible: recording.state === RecordingView.Open && !root.exportShown
                     && !(root.chartMaximized && root.framesShown)
            Layout.fillWidth: true
            // A share of the page, so the rows show under it; the splitter
            // below sets it once dragged. Maximized, it has the page
            readonly property real restingHeight: viewerSettings.chartHeight > 0
                                                  ? Math.max(200, viewerSettings.chartHeight)
                                                  : root.defaultChartHeight
            Layout.minimumHeight: root.chartMaximized || root.pinnedViewer ? 200 : restingHeight
            Layout.preferredHeight: root.chartMaximized ? -1 : restingHeight
            // Pinned, it fills up to its height, so a short page shrinks it
            Layout.maximumHeight: root.pinnedViewer ? restingHeight : Number.POSITIVE_INFINITY
            Layout.fillHeight: root.chartMaximized || root.pinnedViewer
            color: Theme.surface
            radius: 4
            border.color: Theme.hairline
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
                           Math.min(Math.ceil(stripModel.length / stripColumns) * stripCellH + 8,
                                    Math.round(height * 0.4)))
            // A cell's height: a third line with a span
            readonly property real stripCellH: root.hasSpan ? 56 : 40

            StripChart {
                id: chart
                objectName: "chart"
                x: 4
                y: 4
                shownW: parent.width - 8 - chartFrame.stripWidth
                shownH: parent.height - 8 - chartFrame.stripHeight
                emptyText: root.plottedNoValues.length > 0 ? root.noValuesText(root.plottedNoValues)
                                                           : qsTr("No signals plotted; tick a row to plot it")
                fetch: function(widthPx) { return root.series(widthPx) }
                layoutOpts: ({
                    gapMs: Theme.staleMs, minSpanUnits: 20, format: root.valueText,
                    span: { t0: root.viewT0, t1: root.viewT1 }, absolute: true,
                    caption: root.captionOf,
                    axisH: root.phoneShape && root.hasCursor ? ChartLayout.axisHCursor : ChartLayout.axisH,
                    panes: { max: root.paneLimit, minH: ChartLayout.paneMinH,
                             shares: root.paneShares, ranges: root.paneRanges,
                             mixedLabel: qsTr("mixed") } })
                onDrawnChanged: root.notePanes(drawn)
                // Findings as bars along the plot's foot, markers as dashed
                // lines with their labels, the cursors as solid lines over
                // their span's tint, the extremes' dots on the traces
                decorate: function(ctx, l) {
                    const span = root.viewT1 - root.viewT0
                    const xOf = t => l.left + (t - root.viewT0) / span * (l.plotW - 1)
                    const plotEnd = l.left + l.plotW
                    for (const f of recording.findings) {
                        const x0 = xOf(f.atMs), x1 = xOf(f.atMs + f.lengthMs)
                        if (x1 < l.left || x0 > plotEnd) continue
                        ctx.fillStyle = root.findingColor(f.kind)
                        const left = Math.max(l.left, x0)
                        ctx.fillRect(left, l.top + l.plotH - 4, Math.max(2, Math.min(plotEnd, x1) - left), 4)
                    }
                    ctx.strokeStyle = Theme.textDim
                    ctx.fillStyle = Theme.textDim
                    ctx.lineWidth = 1
                    ctx.setLineDash([4, 3])
                    ctx.textAlign = "left"
                    ctx.textBaseline = "top"
                    for (const m of recording.markers) {
                        const x = xOf(m.timeMs)
                        if (x < l.left || x > plotEnd) continue
                        ctx.beginPath()
                        ctx.moveTo(x, l.top)
                        ctx.lineTo(x, l.top + l.plotH)
                        ctx.stroke()
                        ctx.fillText(m.label, x + 3, l.top + 1)
                    }
                    ctx.setLineDash([])
                    const tint = (from, to, alpha) => {
                        const x0 = Math.max(l.left, xOf(from))
                        const x1 = Math.min(plotEnd, xOf(to))
                        if (x1 <= x0)
                            return
                        const c = Qt.color(Theme.text)
                        ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, alpha)
                        ctx.fillRect(x0, l.top, x1 - x0, l.plotH)
                    }
                    if (root.hasSpan)
                        tint(root.spanFromMs, root.spanToMs, 0.06)
                    if (!isNaN(root.boxFromMs))
                        tint(Math.min(root.boxFromMs, root.boxToMs),
                             Math.max(root.boxFromMs, root.boxToMs), 0.14)
                    ctx.strokeStyle = Theme.text
                    for (const t of [root.cursorMs, root.cursorBMs]) {
                        if (!(t >= root.viewT0 && t <= root.viewT1))
                            continue
                        ctx.beginPath()
                        ctx.moveTo(xOf(t), l.top)
                        ctx.lineTo(xOf(t), l.top + l.plotH)
                        ctx.stroke()
                    }
                    // The span's lowest hollow, its highest filled
                    root.spanDots = root.hasSpan
                        ? ChartLayout.extremeDots(l, root.spanStats, root.viewT0, root.viewT1) : []
                    for (const d of root.spanDots) {
                        ctx.beginPath()
                        ctx.arc(d.x, d.y, root.cursorDotR, 0, 2 * Math.PI)
                        if (d.filled) {
                            ctx.fillStyle = Theme.seriesColor(d.row)
                            ctx.fill()
                        } else {
                            ctx.strokeStyle = Theme.seriesColor(d.row)
                            ctx.stroke()
                        }
                    }
                    // A dot on each trace at the sample its value comes from
                    for (const tr of l.traces) {
                        if (!isFinite(tr.labelMs))
                            continue
                        const x = xOf(tr.labelMs)
                        if (x < l.left || x > plotEnd)
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
            // just after is the pinch's last finger, and moves nothing.
            // Under Shift or after a long press it draws the span's box
            DragHandler {
                id: pan
                objectName: "chartPan"
                target: null
                yAxis.enabled: false
                property real startT0: 0
                property bool fromPinch: false
                property bool boxing: false
                onActiveChanged: {
                    if (!active) {
                        if (boxing)
                            root.finishBox()
                        boxing = false
                        return
                    }
                    startT0 = root.viewT0
                    fromPinch = pinch.active || Date.now() - pinch.endedAt < 300
                    boxing = !fromPinch && (chartTap.held || (centroid.modifiers & Qt.ShiftModifier) !== 0)
                    if (boxing) {
                        root.boxFromMs = root.timeAt(centroid.pressPosition.x)
                        root.boxToMs = root.boxFromMs
                    }
                }
                onActiveTranslationChanged: {
                    if (!active || fromPinch) return
                    if (boxing) {
                        root.boxToMs = Math.min(root.viewT1, Math.max(root.viewT0, root.timeAt(centroid.position.x)))
                        chart.repaintMarks()
                        return
                    }
                    const span = root.viewT1 - root.viewT0
                    const t0 = startT0 - activeTranslation.x / Math.max(1, root.plotWidth() - 1) * span
                    root.setView(t0, t0 + span)
                }
            }
            // A pinch zooms the time about its middle; one whose fingers
            // lie more up and down than across as it starts, over a pane
            // with an axis, zooms that pane's range about its middle instead
            PinchHandler {
                id: pinch
                objectName: "chartPinch"
                target: null
                property real lastScale: 1
                property double endedAt: 0
                // The pane a vertical pinch zooms, -1 for a time zoom
                property int pane: -1
                property var startPane: null
                property real startY: 0
                onActiveChanged: {
                    lastScale = 1
                    pane = -1
                    if (!active) {
                        endedAt = Date.now()
                        return
                    }
                    const l = chart.drawn
                    const a = fingerA.point.position, b = fingerB.point.position
                    if (!l || !fingerA.active || !fingerB.active
                            || Math.abs(b.y - a.y) <= Math.abs(b.x - a.x))
                        return
                    startY = centroid.position.y - chart.y
                    const i = ChartLayout.paneAt(l, startY)
                    if (i < 0 || !l.panes[i].axis)
                        return
                    pane = i
                    startPane = Object.assign({}, l.panes[i])
                }
                onActiveScaleChanged: {
                    if (!active || activeScale <= 0) return
                    if (pane >= 0) {
                        root.setPaneRange(startPane.key,
                                          ChartLayout.scaledRange(startPane, startY, 1 / activeScale))
                        return
                    }
                    root.zoomAt(root.timeAt(centroid.position.x), lastScale / activeScale)
                    lastScale = activeScale
                }
            }
            // The pinch's two fingers, for which way it runs as it starts
            PointHandler {
                id: fingerA
                acceptedDevices: PointerDevice.TouchScreen
            }
            PointHandler {
                id: fingerB
                acceptedDevices: PointerDevice.TouchScreen
            }
            // A notch (120) in shows 0.8 of the span, out 1.25; a
            // high-resolution wheel's or a touchpad's parts of a notch zoom
            // by their share of it, and a sideways scroll doesn't zoom.
            // With Ctrl, over a pane with an axis, it zooms the pane's range
            // about the pointer instead
            WheelHandler {
                target: null
                onWheel: event => {
                    const dy = event.angleDelta.y
                    if (dy === 0)
                        return
                    const factor = Math.pow(0.8, dy / 120)
                    if (event.modifiers & Qt.ControlModifier) {
                        const l = chart.drawn
                        const i = l ? ChartLayout.paneAt(l, event.y - chart.y) : -1
                        if (i >= 0 && l.panes[i].axis)
                            root.setPaneRange(l.panes[i].key,
                                              ChartLayout.scaledRange(l.panes[i], event.y - chart.y, factor))
                        return
                    }
                    root.zoomAt(root.timeAt(event.x), factor)
                }
            }
            DoubleTapHandler {
                id: chartTap
                objectName: "chartTap"
                property point lastAt: Qt.point(0, 0)
                // A long press fired in this press, so its drag draws a box
                property bool held: false
                onPressedChanged: if (pressed) held = false
                onLongPressed: {
                    const at = chartTap.point.position
                    if (!root.phoneShape || at.x - chart.x < root.plotLeft())
                        return
                    held = true
                    root.placeSecond(root.snapped(root.timeAt(at.x)))
                }
                onTapped: (eventPoint, button) => {
                    lastAt = eventPoint.position
                    // A tap on the axes' column places no cursor
                    if (eventPoint.position.x - chart.x < root.plotLeft())
                        return
                    const t = root.snapped(root.timeAt(eventPoint.position.x))
                    if ((chartTap.point.modifiers & Qt.ShiftModifier) !== 0)
                        root.placeSecond(t)
                    else
                        root.setCursor(t)
                }
                // A double-click or double-tap maximizes it, or gives the
                // signals back (on an axis or a seam, root.doubleTapAt);
                // the two taps of a thumb may land a little apart
                onDoubleTap: root.doubleTapAt(lastAt.x - chart.x, lastAt.y - chart.y)
            }

            // On the desktop a drag on a pane's axis pans its range (a
            // double click there, the handler above, makes it automatic again)
            Repeater {
                model: Theme.mobile ? 0 : root.paneBoxes.length
                delegate: Item {
                    id: paneAxis
                    required property int index
                    readonly property var box: root.paneBoxes[index]
                    objectName: "paneAxis_" + index
                    visible: box !== undefined && box.axis
                    x: chart.x
                    y: chart.y + (box ? box.top : 0)
                    width: box ? box.left : 0
                    height: box ? box.h : 0
                    HoverHandler { cursorShape: Qt.SizeVerCursor }
                    DragHandler {
                        target: null
                        xAxis.enabled: false
                        property var startRange: null
                        onActiveChanged: {
                            const p = active && chart.drawn ? chart.drawn.panes[paneAxis.index] : null
                            startRange = p ? { vMin: p.vMin, vMax: p.vMax, key: p.key, h: p.h } : null
                        }
                        onActiveTranslationChanged: {
                            if (active && startRange)
                                root.setPaneRange(startRange.key, ChartLayout.pannedRange(
                                    startRange, startRange.h, activeTranslation.y))
                        }
                    }
                }
            }
            // The seams between panes: a drag trades two neighbors' heights,
            // kept by the count of panes; a double tap evens them (above).
            // A count for a model, so a drag's own resize keeps its handler
            Repeater {
                model: Math.max(0, root.paneBoxes.length - 1)
                delegate: Item {
                    id: seam
                    required property int index
                    readonly property var box: root.paneBoxes[index]
                    objectName: "paneSeam_" + index
                    x: chart.x + (box ? box.left : 0)
                    y: chart.y + (box ? box.top + box.h + ChartLayout.paneGap / 2 : 0) - height / 2
                    width: box ? box.plotW : 0
                    height: 24
                    Rectangle {
                        anchors.centerIn: parent
                        width: 40
                        height: 4
                        radius: 2
                        color: seamDrag.active || seamHover.hovered ? Theme.textDim : Theme.outline
                    }
                    HoverHandler { id: seamHover; cursorShape: Qt.SizeVerCursor }
                    DragHandler {
                        id: seamDrag
                        objectName: "seamDrag"
                        target: null
                        xAxis.enabled: false
                        property var startHeights: []
                        onActiveChanged: if (active) startHeights = root.paneBoxes.map(b => b.h)
                        onActiveTranslationChanged: {
                            if (active && startHeights.length > seam.index + 1)
                                root.setPaneShares(startHeights.length, ChartLayout.seamShares(
                                    startHeights, seam.index, activeTranslation.y, ChartLayout.paneMinH))
                        }
                    }
                }
            }

            // A cursor's handle at the axis's foot, its time on it; a drag
            // moves its cursor. It reaches up over the plot to a touch's
            // height, and takes the press from the chart's own gestures
            component CursorHandle: Item {
                id: handle
                required property bool isB
                readonly property real at: isB ? root.cursorBMs : root.cursorMs
                readonly property real minWidth: root.phoneShape ? Theme.touchTarget : 24
                readonly property real tabH: chart.drawn ? chart.drawn.axisH : ChartLayout.axisH
                visible: chart.drawn !== null && at >= root.viewT0 && at <= root.viewT1
                width: Math.max(minWidth, handleText.implicitWidth + 8)
                height: root.phoneShape ? Theme.touchTarget : 24
                x: Math.min(Math.max(0, root.xOfTime(at) - width / 2), chartFrame.width - width)
                y: chart.y + (chart.drawn ? chart.drawn.top + chart.drawn.plotH + tabH : 0) - height
                Accessible.role: Accessible.Slider
                Accessible.name: qsTr("Cursor at %1").arg(handleText.text)
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: handle.tabH
                    radius: 2
                    readonly property bool moved: handle.isB === root.arrowsMoveB
                    color: moved ? Theme.text : Theme.elevated
                    border.width: 1
                    border.color: Theme.text
                    Label {
                        id: handleText
                        objectName: "handleTime"
                        anchors.centerIn: parent
                        text: root.timeText(handle.at, true)
                        color: parent.moved ? Theme.bg : Theme.text
                        font.pixelSize: ChartLayout.labelPx
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
                    }
                }
                MouseArea {
                    objectName: "handleDrag"
                    anchors.fill: parent
                    cursorShape: Qt.SizeHorCursor
                    preventStealing: true
                    property real pressX: 0
                    property bool dragging: false
                    function frameX(x) {
                        return mapToItem(chartFrame, x, 0).x
                    }
                    onPressed: mouse => {
                        pressX = frameX(mouse.x)
                        dragging = false
                    }
                    onPositionChanged: mouse => {
                        const x = frameX(mouse.x)
                        if (!dragging && Math.abs(x - pressX) < Application.styleHints.startDragDistance)
                            return
                        dragging = true
                        if (handle.isB)
                            root.setCursorB(root.timeAt(x))
                        else
                            root.setCursor(root.timeAt(x))
                    }
                    onReleased: if (dragging) root.dropHandle(handle.isB)
                }
            }
            CursorHandle { id: handleA; objectName: "cursorHandleA"; isB: false }
            CursorHandle { id: handleB; objectName: "cursorHandleB"; isB: true }

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
                cellHeight: chartFrame.stripCellH
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: visible ? chartFrame.stripModel : []
                delegate: Item {
                    id: stripRow
                    required property var modelData
                    readonly property var at: root.readoutFrom[modelData.index]
                    width: GridView.view.cellWidth
                    height: chartFrame.stripCellH
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
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "stripSpan"
                        visible: root.hasSpan
                        x: 12
                        y: 34
                        width: parent.width - 12
                        text: visible ? root.spanCaption(stripRow.modelData) : ""
                        color: Theme.textDim
                        font.pixelSize: Theme.fontCaption
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
                        elide: Text.ElideRight
                    }
                }
            }
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

        // Maximized on a narrow page, the views' switch has a row of its own
        RowLayout {
            id: switchRowSlot
            visible: root.chartMaximized && recording.hasFrames && !root.switchInZoomRow
        }

        // The span line: where the two cursors are and how far apart, or
        // once, the first cursor's hint for the second. Under the zoom row
        // on a phone, at its right on the desktop
        RowLayout {
            id: spanLine
            objectName: "spanLine"
            parent: root.phoneShape ? spanRowPhone : spanRowDesktop
            visible: root.spanLineUp
            Layout.fillWidth: true
            spacing: 6
            Label {
                objectName: "spanText"
                Layout.fillWidth: true
                horizontalAlignment: root.phoneShape ? Text.AlignLeft : Text.AlignRight
                verticalAlignment: Text.AlignVCenter
                wrapMode: root.phoneShape ? Text.WordWrap : Text.NoWrap
                maximumLineCount: root.phoneShape ? 2 : 1
                elide: Text.ElideRight
                font.pixelSize: root.phoneShape ? Theme.fontCaption : Theme.fontBody
                color: Theme.textDim
                text: root.hasSpan ? qsTr("A %1 to B %2 · %3").arg(root.timeText(root.spanFromMs, true))
                                       .arg(root.timeText(root.spanToMs, true))
                                       .arg(root.timeText(root.spanToMs - root.spanFromMs, true))
                      : root.phoneShape ? qsTr("Hold to place the second cursor")
                      : qsTr("At %1. Shift+click to place the second cursor")
                            .arg(root.timeText(root.cursorMs, true))
            }
            ThemedButton {
                objectName: "zoomToSpan"
                visible: root.hasSpan
                compact: true
                text: root.phoneShape ? qsTr("Zoom") : qsTr("Zoom to span")
                Accessible.name: qsTr("Zoom to the span")
                onClicked: root.zoomToSpan()
            }
            ThemedButton {
                objectName: "clearSpan"
                visible: root.hasSpan
                flat: true
                text: qsTr("Clear")
                Accessible.name: qsTr("Remove the second cursor")
                onClicked: root.clearCursorB()
            }
        }

        // The zoom row's place on the page; on the pinned form it leads the
        // shown view's list
        ColumnLayout {
            id: zoomHome
            // A visible layout counts in its column's spacing even when empty
            visible: zoomBlock.parent === zoomHome && recording.state === RecordingView.Open
            Layout.fillWidth: true
            // A layout fills by default: this one keeps its content's height
            Layout.fillHeight: false
            ColumnLayout {
                id: zoomBlock
                parent: root.currentHead ? root.currentHead.zoomSlot : zoomHome
                Layout.fillWidth: true
                spacing: 10
                // Zoom, and where the cursor is
                // Compact on a phone: at their desktop widths the buttons, Frame
                // test among them, outgrow its width
                RowLayout {
                    visible: recording.state === RecordingView.Open
                    Layout.fillWidth: true
                    spacing: 6
                    // Maximized, the views' switch leads the row, so either view
                    // leads back to the other
                    RowLayout {
                        id: zoomSwitchSlot
                        visible: root.chartMaximized && recording.hasFrames && root.switchInZoomRow
                    }
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
                    // On a phone the hint wraps onto two caption lines where the
                    // row leaves it little width, instead of eliding to "Tap for …"
                    Label {
                        objectName: "cursorTime"
                        visible: root.phoneShape || !root.spanLineUp
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Theme.compact ? Text.WordWrap : Text.NoWrap
                        maximumLineCount: Theme.compact ? 2 : 1
                        elide: Text.ElideRight
                        font.pixelSize: Theme.compact ? Theme.fontCaption : Theme.fontBody
                        color: Theme.textDim
                        text: !root.hasCursor ? (Theme.compact ? qsTr("Tap for values")
                                                               : qsTr("Click the chart for values"))
                                              : qsTr("At %1").arg(root.timeText(root.cursorMs, true))
                    }
                    RowLayout {
                        id: spanRowDesktop
                        visible: !root.phoneShape && root.spanLineUp
                        Layout.fillWidth: true
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

                // The span line, under the zoom row on a phone
                RowLayout {
                    id: spanRowPhone
                    visible: root.phoneShape && root.spanLineUp
                    Layout.fillWidth: true
                }
                ColumnLayout {
                    id: spanStatsPinned
                    visible: spanStatsBlock.parent === spanStatsPinned && spanStatsBlock.shown
                    Layout.fillWidth: true
                    Layout.fillHeight: false
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
            }
        }

        // The signal area's two views, the values at the cursor or the frames
        // in view (hidden for a recording with no frames.csv), then the
        // filter: one row on the desktop, a row each on a phone
        GridLayout {
            id: detailGrid
            objectName: "detailRow"
            visible: recording.state === RecordingView.Open && !root.chartMaximized && !root.pinnedViewer
            Layout.fillWidth: true
            columns: root.phoneShape ? 1 : 2
            columnSpacing: Theme.spacing
            rowSpacing: 10
            RowLayout {
                id: detailSwitchSlot
                visible: recording.hasFrames
                // Maximized, it leads the zoom row where the row has room
                Rectangle {
                    parent: root.currentHead ? root.currentHead.switchSlot
                          : !root.chartMaximized ? detailSwitchSlot
                          : root.switchInZoomRow ? zoomSwitchSlot : switchRowSlot
                    implicitWidth: detailSwitch.implicitWidth + 4
                    implicitHeight: detailSwitch.implicitHeight + 4
                    radius: Theme.radius
                    color: Theme.surface

                    Row {
                        id: detailSwitch
                        anchors.centerIn: parent
                        spacing: 2
                        SegmentButton {
                            tapHeight: root.segmentHeight
                            objectName: "detailView_values"
                            text: qsTr("Values")
                            selected: !root.framesShown && !root.aboutShown
                            onClicked: root.detailView = "values"
                        }
                        SegmentButton {
                            tapHeight: root.segmentHeight
                            objectName: "detailView_frames"
                            visible: recording.hasFrames
                            text: qsTr("Frames")
                            selected: root.framesShown
                            onClicked: root.detailView = "frames"
                        }
                        // On the pinned form About is a view of its own
                        SegmentButton {
                            tapHeight: root.segmentHeight
                            objectName: "detailView_about"
                            visible: root.pinnedViewer
                            text: qsTr("About")
                            selected: root.aboutShown
                            onClicked: root.detailView = "about"
                        }
                    }
                }
            }
            SignalFilterBar {
                objectName: "recordingFilter"
                parent: root.currentHead && root.currentHead === root.valuesHead
                        ? root.valuesHead.toolsSlot : detailGrid
                visible: !root.framesShown
                Layout.fillWidth: true
                filter: signalFilter
                codes: recording.signalList.map(s => s.group)
            }
        }

        // ---- The frames of the part in view, read from frames.csv ----
        ColumnLayout {
            objectName: "viewerFrames"
            visible: root.framesShown
            Layout.fillWidth: true
            Layout.fillHeight: root.chartMaximized || root.pinnedViewer
            spacing: Theme.spacing

            // The table's tools: here, or in the table's head on the pinned form
            ColumnLayout {
                id: framesToolsHome
                visible: framesTools.parent === framesToolsHome
                Layout.fillWidth: true
                // A layout fills by default: this one keeps its content's height
                Layout.fillHeight: false
                ColumnLayout {
                    id: framesTools
                    parent: root.currentHead && root.framesShown ? root.currentHead.toolsSlot
                                                                 : framesToolsHome
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
                                    tapHeight: root.segmentHeight
                                    objectName: "framesTable_frames"
                                    text: qsTr("List")
                                    selected: !root.framesById
                                    onClicked: root.framesById = false
                                }
                                SegmentButton {
                                    tapHeight: root.segmentHeight
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
                }
            }

            // A fixed height, the page scrolling and the table inside it;
            // maximized, what the page has left
            FrameTable {
                id: viewerFrameTable
                objectName: "viewerFrameTable"
                visible: !root.framesById
                Layout.preferredHeight: root.chartMaximized || root.pinnedViewer ? 0 : 280
                Layout.minimumHeight: root.pinnedViewer ? root.pinnedListMin : 0
                Layout.fillHeight: root.chartMaximized || root.pinnedViewer
                head: framesHeadComponent
                headScrolls: root.pinnedViewer
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
                id: viewerFrameIds
                objectName: "viewerFrameIds"
                visible: root.framesById
                Layout.preferredHeight: root.chartMaximized || root.pinnedViewer ? 0 : 280
                Layout.minimumHeight: root.pinnedViewer ? root.pinnedListMin : 0
                Layout.fillHeight: root.chartMaximized || root.pinnedViewer
                head: idsHeadComponent
                headScrolls: root.pinnedViewer
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

        // The recording's signals: a tick plots one, and beside each its
        // value at the cursor
        ListView {
            id: signalRows
            objectName: "signalRows"
            visible: recording.state === RecordingView.Open && !root.chartMaximized && !root.framesShown
                     && !root.aboutShown
            Layout.fillWidth: true
            // On the pinned form the rest of the page, its head included
            Layout.preferredHeight: root.pinnedViewer ? 0 : count * root.rowHeight
            Layout.minimumHeight: root.pinnedViewer ? root.pinnedListMin : 0
            Layout.maximumHeight: root.pinnedViewer ? Number.POSITIVE_INFINITY
                                                    : Math.max(root.rowHeight, column.height * 0.3)
            Layout.fillHeight: root.pinnedViewer
            clip: true
            currentIndex: -1
            highlightFollowsCurrentItem: false
            boundsBehavior: Flickable.StopAtBounds
            model: root.shownSignals
            header: Component {
                ViewHead {
                    role: "values"
                    width: signalRows.width
                    list: signalRows
                }
            }
            footer: Component {
                ColumnLayout {
                    id: valuesFootItem
                    width: signalRows.width
                    Item { implicitHeight: 6; visible: filterCount.parent === valuesFootItem && filterCount.shown }
                    Component.onCompleted: root.valuesFoot = valuesFootItem
                }
            }
            ScrollBar.vertical: ScrollBar {
                policy: root.pinnedViewer && signalRows.contentHeight > signalRows.height
                        ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            }

            delegate: Rectangle {
                id: signalRow
                required property var modelData
                required property int index
                readonly property int signalIndex: modelData.index
                readonly property var at: root.readoutFrom[signalIndex]
                width: signalRows.width
                height: root.rowHeight
                color: index % 2 === 0 ? Theme.surface : Theme.bg

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 12
                    ThemedCheckBox {
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
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: signalRow.modelData.name
                                color: Theme.text
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                objectName: "cursorValue"
                                text: root.atText(signalRow.at, signalRow.modelData)
                                Layout.maximumWidth: Math.round(signalRow.width * (root.compareAligned ? 0.3 : 0.5))
                                elide: Text.ElideRight
                                color: Theme.text
                                font.family: Theme.valueFamily
                                font.features: Theme.valueFeatures
                            }
                        }
                        // On a phone B and the change have a line of their own
                        Label {
                            objectName: "cursorSpanLine"
                            visible: root.phoneShape && root.hasSpan
                            text: visible ? root.spanCaption(signalRow.modelData) : ""
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideLeft
                            color: Theme.textDim
                            font.pixelSize: Theme.fontCaption
                            font.family: Theme.valueFamily
                            font.features: Theme.valueFeatures
                        }
                    }
                    // On the desktop, two more columns
                    Label {
                        objectName: "cursorBValue"
                        visible: !root.phoneShape && root.hasSpan
                        text: visible ? root.atText(root.readoutTo[signalRow.signalIndex], signalRow.modelData) : ""
                        Layout.preferredWidth: Math.round(signalRow.width * 0.18)
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        color: Theme.text
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
                    }
                    Label {
                        objectName: "cursorDelta"
                        visible: !root.phoneShape && root.hasSpan
                        text: visible && !signalRow.modelData.isText
                              ? root.statChange(root.spanStats[signalRow.signalIndex]) : ""
                        Layout.preferredWidth: Math.round(signalRow.width * 0.18)
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        color: Theme.textDim
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
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
                        font.family: Theme.valueFamily
                        font.features: Theme.valueFeatures
                    }
                }
            }
        }

        // While the filter narrows the rows, how many it shows and how many
        // the chart draws: the chart keeps every ticked signal (ADR-0050).
        // Under the rows, or their list's footer on the pinned form
        ColumnLayout {
            id: filterCountHome
            visible: filterCount.parent === filterCountHome && filterCount.shown
            Layout.fillWidth: true
            Layout.fillHeight: false
            Label {
                id: filterCount
                objectName: "filterCount"
                readonly property bool shown: recording.state === RecordingView.Open && signalFilter.active
                                              && !root.chartMaximized && !root.framesShown && !root.aboutShown
                parent: root.pinnedViewer && root.valuesFoot !== null ? root.valuesFoot : filterCountHome
                visible: shown
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textDim
                text: (Theme.compact ? qsTr("%1 of %2 shown · %3 plotted")
                                     : qsTr("Showing %1 of %2 signals · plotting %3"))
                      .arg(root.shownSignals.length).arg(recording.signalList.length)
                      .arg(recording.signalList.filter(s => root.plotted[s.index] === true).length)
            }
        }

        // The statistics' place on the page; on the pinned form they follow
        // the span line in the list's head
        ColumnLayout {
            id: spanStatsHome
            visible: spanStatsBlock.parent === spanStatsHome && spanStatsBlock.shown
            Layout.fillWidth: true
            Layout.fillHeight: false
            // Each plotted signal over the span: a table on the desktop, a
            // grid of cards on a phone, whose rate the span line's length gives
            ColumnLayout {
                id: spanStatsBlock
                objectName: "spanStats"
                readonly property bool shown: root.spanRows.length > 0 && !root.chartMaximized
                                              && !root.framesShown && !root.aboutShown
                parent: root.pinnedViewer ? spanStatsPinned : spanStatsHome
                visible: shown
                Layout.fillWidth: true
                spacing: 0
                component StatCell: Label {
                    // Shares, not widths: a width read from the page would
                    // make the column lay itself out again as it sizes
                    Layout.fillWidth: true
                    Layout.preferredWidth: 10
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                    color: Theme.text
                    font.family: Theme.valueFamily
                    font.features: Theme.valueFeatures
                }
                RowLayout {
                    visible: !root.phoneShape
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 12
                    Label { text: qsTr("Signal"); color: Theme.textDim; Layout.fillWidth: true; Layout.preferredWidth: 30 }
                    StatCell { text: qsTr("Change"); color: Theme.textDim }
                    StatCell { text: qsTr("Low"); color: Theme.textDim }
                    StatCell { text: qsTr("High"); color: Theme.textDim }
                    StatCell { text: qsTr("Mean"); color: Theme.textDim }
                    StatCell { text: qsTr("Rate"); color: Theme.textDim }
                }
                Repeater {
                    model: root.phoneShape ? [] : root.spanRows
                    delegate: RowLayout {
                        id: statRow
                        required property var modelData
                        readonly property var stat: root.spanStats[modelData.index]
                        objectName: "statRow_" + modelData.index
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
                        Layout.rightMargin: 12
                        Label {
                            text: statRow.modelData.name
                            color: Theme.text
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            Layout.preferredWidth: 30
                        }
                        StatCell { objectName: "statChange"; text: root.statChange(statRow.stat) }
                        StatCell {
                            objectName: "statLow"
                            text: statRow.stat ? root.statText(statRow.stat.lowest, statRow.stat) : qsTr("—")
                        }
                        StatCell {
                            objectName: "statHigh"
                            text: statRow.stat ? root.statText(statRow.stat.highest, statRow.stat) : qsTr("—")
                        }
                        StatCell {
                            objectName: "statMean"
                            text: statRow.stat ? root.statText(statRow.stat.mean, statRow.stat) : qsTr("—")
                        }
                        StatCell { objectName: "statRate"; text: root.statRate(statRow.stat) }
                    }
                }
                GridLayout {
                    visible: root.phoneShape
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 4
                    rowSpacing: 4
                    Repeater {
                        model: root.phoneShape ? root.spanRows : []
                        delegate: Rectangle {
                            id: statCard
                            required property var modelData
                            readonly property var stat: root.spanStats[modelData.index]
                            objectName: "statCard_" + modelData.index
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: Theme.touchTarget
                            color: Theme.surface
                            radius: 4
                            border.color: Theme.hairline
                            border.width: 1
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: 0
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: statCard.modelData.name
                                        color: Theme.textDim
                                        elide: Text.ElideRight
                                        font.pixelSize: Theme.fontCaption
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        objectName: "statChange"
                                        text: root.statChange(statCard.stat)
                                        color: Theme.text
                                        font.pixelSize: Theme.fontCaption
                                        font.family: Theme.valueFamily
                                        font.features: Theme.valueFeatures
                                    }
                                }
                                Label {
                                    objectName: "statLine"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    color: Theme.textDim
                                    font.pixelSize: Theme.fontCaption
                                    font.family: Theme.valueFamily
                                    font.features: Theme.valueFeatures
                                    text: !statCard.stat ? qsTr("—")
                                          : [statCard.stat.lowest, statCard.stat.highest, statCard.stat.mean]
                                            .map(v => isFinite(v) ? Number(v).toLocaleString(Qt.locale(), 'f', statCard.stat.decimals)
                                                                  : qsTr("—")).join(" · ")
                                }
                            }
                        }
                    }
                }
            }
        }

        // What applies to the recording as a whole: under the rows, or the
        // About view's head on the pinned form
        ColumnLayout {
            id: aboutHome
            // Shown through a reopen, so the decode choice keeps its model
            visible: aboutBlock.parent === aboutHome && !root.pinnedViewer && !root.chartMaximized
                     && root.viewing
            Layout.fillWidth: true
            // A layout fills by default: this one keeps its content's height
            Layout.fillHeight: false
            ColumnLayout {
                id: aboutBlock
                parent: root.currentHead && root.aboutShown ? root.currentHead.toolsSlot : aboutHome
                Layout.fillWidth: true
                spacing: 10
                // ---- About this recording: what applies to it as a whole ----
                Heading {
                    objectName: "aboutHeading"
                    visible: recording.state === RecordingView.Open && !root.chartMaximized && !root.pinnedViewer
                    text: qsTr("About this recording")
                }
                Label {
                    objectName: "recordingInfo"
                    visible: recording.state === RecordingView.Open && !root.chartMaximized
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    text: recording.state === RecordingView.Open ? root.infoText() : ""
                }
                RowLayout {
                    id: aboutActions
                    visible: Theme.compact && recording.state === RecordingView.Open && !root.chartMaximized
                    Layout.fillWidth: true
                }
                ColumnLayout {
                    id: aboutNote
                    visible: Theme.compact && root.reportNote !== "" && !root.chartMaximized
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "reportNote"
                    // Under Save report...: the title row's on the desktop, About's on a phone
                    parent: Theme.compact ? aboutNote : topNote
                    visible: root.reportNote !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.reportNote
                    color: root.reportFailed ? Theme.danger : Theme.textDim
                }

                // Where the values come from: offered once it opened, or failed
                // (a profile that can't decode it, a folder with frames alone)
                ComboBox {
                    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                             implicitContentHeight + topPadding + bottomPadding,
                                             implicitIndicatorHeight + topPadding + bottomPadding, root.touchMin)
                    id: decodeChoice
                    objectName: "decodeChoice"
                    // Values alone have no frames to decode through a profile
                    readonly property bool shown: (recording.state === RecordingView.Open
                                                   || recording.state === RecordingView.Failed)
                                                  && !root.chartMaximized && recording.hasFrames
                    visible: shown
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    // Its own rule, not its place's: About's head hides and moves it
                    model: shown ? root.decodeChoices() : []
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
                        height: root.phoneShape ? Theme.touchTarget : Theme.controlHeight
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
                        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                                 implicitContentHeight + topPadding + bottomPadding,
                                                 implicitIndicatorHeight + topPadding + bottomPadding, root.touchMin)
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
                        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                                 implicitContentHeight + topPadding + bottomPadding,
                                                 implicitIndicatorHeight + topPadding + bottomPadding, root.touchMin)
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
                                { text: qsTr("Align engine starts"), value: "engine" },
                                { text: qsTr("Align a pull"), value: "pull" }]
                        onActivated: root.setAlignOn(currentValue)
                    }
                    ComboBox {
                        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                                 implicitContentHeight + topPadding + bottomPadding,
                                                 implicitIndicatorHeight + topPadding + bottomPadding, root.touchMin)
                        objectName: "alignPullHere"
                        visible: root.alignOn === "pull"
                        width: 220
                        enabled: count > 0
                        model: root.pullRows(root.pullsHere, true)
                        currentIndex: root.alignPullHere
                        Accessible.name: qsTr("This recording's pull to align on")
                        // A pick ends the binding: put it back, so the box
                        // follows the page's choice
                        onActivated: index => {
                            root.alignPullHere = index
                            currentIndex = Qt.binding(() => root.alignPullHere)
                        }
                    }
                    ComboBox {
                        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                                 implicitContentHeight + topPadding + bottomPadding,
                                                 implicitIndicatorHeight + topPadding + bottomPadding, root.touchMin)
                        objectName: "alignPullThere"
                        visible: root.alignOn === "pull"
                        width: 220
                        enabled: count > 0
                        model: root.pullRows(root.pullsThere, false)
                        currentIndex: root.alignPullThere
                        Accessible.name: qsTr("The compared recording's pull to align on")
                        onActivated: index => {
                            root.alignPullThere = index
                            currentIndex = Qt.binding(() => root.alignPullThere)
                        }
                    }
                    ComboBox {
                        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                                 implicitContentHeight + topPadding + bottomPadding,
                                                 implicitIndicatorHeight + topPadding + bottomPadding, root.touchMin)
                        objectName: "alignMarker"
                        visible: root.alignOn === "marker"
                        width: 150
                        model: recording.state === RecordingView.Open ? root.markerLabels() : []
                        onActivated: root.alignLabel = currentText
                    }
                    ComboBox {
                        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                                                 implicitContentHeight + topPadding + bottomPadding,
                                                 implicitIndicatorHeight + topPadding + bottomPadding, root.touchMin)
                        objectName: "alignSignal"
                        visible: root.alignOn === "rise" || root.alignOn === "fall"
                        width: 150
                        model: recording.state === RecordingView.Open ? root.numberNames() : []
                        onActivated: root.alignSignal = currentText
                    }
                    ThemedTextField {
                        objectName: "alignLevel"
                        visible: root.alignOn === "rise" || root.alignOn === "fall"
                        width: 100
                        placeholderText: qsTr("Level")
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        onEditingFinished: root.alignLevel = root.numberOf(text)
                    }
                    ThemedTextField {
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
            }
        }
        ListView {
            id: findingList
            objectName: "findingList"
            visible: recording.state === RecordingView.Open && !root.chartMaximized
                     && (root.pinnedViewer ? root.aboutShown : count > 0)
            Layout.fillWidth: true
            Layout.preferredHeight: root.pinnedViewer ? 0 : Math.min(count, 3) * Math.max(32, root.touchMin)
            Layout.minimumHeight: root.pinnedViewer ? root.pinnedListMin : 0
            Layout.fillHeight: root.pinnedViewer
            clip: true
            currentIndex: -1
            highlightFollowsCurrentItem: false
            boundsBehavior: Flickable.StopAtBounds
            model: recording.findings
            header: Component {
                ViewHead {
                    role: "about"
                    width: findingList.width
                    list: findingList
                }
            }
            ScrollBar.vertical: ScrollBar {
                policy: root.pinnedViewer && findingList.contentHeight > findingList.height
                        ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            }

            delegate: Rectangle {
                id: findingRow
                required property var modelData
                width: findingList.width
                height: Math.max(32, root.touchMin)
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

        // The rest stays at the top
        Item {
            visible: root.viewing && !root.chartMaximized && !root.pinnedViewer
            Layout.fillHeight: true
        }
    }
}
