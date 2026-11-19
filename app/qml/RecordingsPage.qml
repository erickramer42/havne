// File/layer: app/qml/RecordingsPage.qml | Qt6 Quick, UI layer
// Testing: BRIDGE: RecordingViewTest (test_bridge); QML: QmlTest.recordings,
//          QmlTest.phone, QmlTest.chartlayout (test_qml); Manual smoke

import QtCore
import QtQuick
import QtQuick.Controls
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

    // The parts' ids the page reads, and the page's ids the parts read
    property alias refreshPart: refreshPart
    property alias comparePart: comparePart
    property alias decodePart: decodePart
    property alias rowInfoPart: rowInfoPart
    property alias formatPart: formatPart
    property alias textsPart: textsPart
    property alias seriesPart: seriesPart
    property alias settle: recordingWatchers.settle
    property alias framesRefill: recordingWatchers.framesRefill
    property alias cursorsPart: cursorsPart
    property alias signalFilter: signalFilter
    property alias openPart: openPart
    property alias paneMenu: paneMenu
    property alias panesPart: panesPart
    property alias scroller: scroller
    property alias exportPanel: exportPanel
    property alias compare: recordingViews.compare
    property alias reportPart: reportPart
    property alias frameTestPart: frameTestPart
    property alias spanStatsHome: spanStatsHome
    property alias recording: recordingViews.recording
    property alias chartFrame: chartFrame
    property alias switchRowSlot: switchRowSlot
    property alias spanRowDesktop: zoomBlock.spanRowDesktop
    property alias spanRowPhone: zoomBlock.spanRowPhone
    property alias spanStatsPinned: zoomBlock.spanStatsPinned
    property alias zoomSwitchSlot: zoomBlock.zoomSwitchSlot
    property alias spanStatsBlock: spanStatsBlock
    property alias zoomHome: zoomHome
    property alias column: column
    property alias filterCount: filterCount
    property alias framesFilterPanel: viewerFrames.framesFilterPanel
    property alias viewerFrameIds: viewerFrames.viewerFrameIds
    property alias viewerFrameTable: viewerFrames.viewerFrameTable
    property alias framesHeadComponent: framesHeadComponent
    property alias idsHeadComponent: idsHeadComponent
    property alias viewFrames: recordingWatchers.viewFrames
    property alias aboutActions: aboutBlock.aboutActions
    property alias alignChoice: aboutBlock.alignChoice
    property alias aboutHome: aboutHome
    property alias compareSlot: compareSlot
    property alias topNote: topNote
    property alias guessPreview: importPanel.guessPreview
    property alias importInterface: importPanel.importInterface
    property alias importLabel: importPanel.importLabel
    property alias importDialog: recordingDialogs.importDialog
    property alias mapDialog: recordingDialogs.mapDialog
    property alias reportDialog: recordingDialogs.reportDialog
    property alias chart: chartFrame.chart
    property alias pan: chartFrame.pan
    property alias pinch: chartFrame.pinch
    property alias conditionsPanel: conditionsPanel
    property alias viewerSettings: viewerSettings

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
    onBaselineFolderChanged: openPart.checkBaseline()

    readonly property int recordingsListed: 20

    // Import: another logger's file made a recording. The file picked, its
    // kind (RecordingImport.Kind: guessed from its first line, switched
    // here), a CSV's column map, and the last import's words: what it
    // made, or why not
    property url importFile: ""
    // The picked file's and map's names as shown (a provider's URL ends in
    // an id, not the name)
    property string importFileName: ""
    property int importKind: RecordingImport.Csv
    property url importMap: ""
    property string importMapFileName: ""
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
    // Cancel pressed on an import under way, until it ends
    property bool importStopping: false
    // The last import was canceled: neither made nor failed
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
    // A focused box (a ComboBox, which keeps Home, End and its letters for its
    // own choice) keeps those keys: told by its `popup`, since a style's box
    // isn't a ComboBox to `instanceof`. Left and Right it doesn't use, and a
    // press in the plot takes the focus back from it (chartTap)
    readonly property bool boxFocused: root.Window.activeFocusItem !== null
                                       && root.Window.activeFocusItem.popup !== undefined
    // The arrows step a cursor a sample of the snap's signal, B says which
    // cursor they move, Home and End go to the recording's ends
    readonly property bool cursorKeys: root.visible && recording.state === RecordingView.Open
                                       && !Theme.mobile
    readonly property bool editKeys: cursorKeys && !root.boxFocused
    RecordingKeys { id: recordingKeys; host: root }
    // The splitter below the chart: its height once dragged, -1 for the
    // page's own (the default), kept between runs
    Settings {
        id: viewerSettings
        category: "recordings"
        property real chartHeight: -1
        // The maximized chart's strip of the cursor's values folded to a
        // tab, so the plot has the width
        property bool stripFolded: false
        // Each recording's pane arrangement (ChartLayout.viewsParse): the
        // panes its signals sit in and the panes' heights once a seam is
        // dragged, by the recording's folder name
        property string views: "{}"
        // The second cursor's hint has been shown
        property bool spanHintSeen: false
        // The stretches' levels the user set, as JSON by key, in display
        // units: {} for core's defaults
        property string stretchRules: "{}"
    }
    property alias chartHeight: viewerSettings.chartHeight
    property alias stripFolded: viewerSettings.stripFolded
    property alias spanHintSeen: viewerSettings.spanHintSeen
    // The chart's height until the splitter sets one: on a phone 40 % of
    // the page, no less than three panes and the time axis, its handles' height
    readonly property real phoneChartFloor: 3 * ChartLayout.paneMinH + 2 * ChartLayout.paneGap
                                            + ChartLayout.top + ChartLayout.axisHCursor
                                            + ChartLayout.bottomPad + 8
    // A phone's chart gives up the 8 px the Jump row's top margin takes, so the
    // first row of data stays on the first screen
    readonly property real defaultChartHeight: phoneShape ? Math.max(phoneChartFloor, scroller.height * 0.4 - 8)
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
    // The pane arrangement kept with the open recording, by its folder's
    // name; another recording opens with the panes its units make
    readonly property var viewsBook: ChartLayout.viewsParse(viewerSettings.views)
    readonly property string viewName: recording.state === RecordingView.Open ? panesPart.viewNameOf(recording.folder) : ""
    readonly property var viewEntry: viewName !== "" ? viewsBook[viewName] : undefined
    // Pane id (paneIdOf; a name an earlier version kept) -> pane key, and
    // pane count -> heights' shares
    readonly property var viewAssign: viewEntry !== undefined ? viewEntry.assign : ({})
    readonly property var paneShares: viewEntry !== undefined ? viewEntry.shares : ({})
    // A format 4 recording keeps a pane by key; an older one by name, which
    // its values and its frames opens share
    readonly property bool panesByKey: recording.info.format >= 4
    // The menu's entries for one signal: [{ text, key, checked }], key "" for
    // the unit's own pane, "new" for a pane of its own
    property var paneMenuSignal: ({ key: "", name: "" })
    property var paneMenuItems: []
    // Said under the chart while the arrangement has more panes than fit
    readonly property string paneFoldNote: {
        const l = chart.drawn
        if (!l)
            return ""
        const folded = {}
        for (const tr of l.traces) {
            const k = tr.pane !== undefined ? l.panes[tr.pane].key : ""
            if (k === "mixed" || k === "difference")
                folded[String(tr.paneKey)] = true
        }
        const n = Object.keys(folded).length
        return n === 0 ? "" : qsTr("%n pane(s) are folded into the last one: maximize the chart or move a signal", "", n)
    }
    // The drawn panes' places, assigned only when they change, so the
    // seams' and axes' handlers aren't made again at every paint
    property var paneBoxes: []
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
    onViewT0Changed: cursorsPart.refillFramesSoon()
    onViewT1Changed: cursorsPart.refillFramesSoon()
    onFramesShownChanged: {
        if (framesShown)
            cursorsPart.refillFrames()
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
    // Signal index -> its trace's color slot, by plotted order: a signal
    // takes the lowest free slot when plotted and keeps it while it stays
    // plotted, so unplotting another recolors nothing
    property var slots: ({})
    onPlottedChanged: panesPart.reconcileSlots()
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
    property ViewHead valuesHead: null
    property Item valuesFoot: null
    property ViewHead framesHead: null
    property ViewHead idsHead: null
    property ViewHead aboutHead: null
    readonly property ViewHead currentHead: !pinnedViewer ? null
                                      : aboutShown ? aboutHead
                                      : framesShown ? (framesById ? idsHead : framesHead)
                                      : valuesHead
    Component {
        id: framesHeadComponent
        ViewHead { role: "frames"; list: viewerFrameTable.list; host: root }
    }
    Component {
        id: idsHeadComponent
        ViewHead { role: "ids"; list: viewerFrameIds.list; host: root }
    }
    // A column narrower than a second reads the recording's raw rows
    // (ADR-0049 section 5); a second or wider, the reader's per-second
    // summaries. A frame's work is the rows of the signals it draws (the
    // reader passes by another signal's row at a twentieth of the cost):
    // past this many of those in view, both recordings' together, columns
    // stay a second wide (ADR-0049's appendix "the viewer")
    readonly property int rawRowsPerFrame: 20000
    // The reader keeps 8 MiB of parsed rows, every signal's. A view that
    // holds more reads its seconds from the file again each frame, so past
    // this many rows of either recording in view columns stay a second
    // wide too. A test lowers it
    property int rawRowsCached: 150000
    // The recording's rows a second, every signal's together
    property real rowsPerSecond: 0

    // A second recording overlaid (ADR-0049 step 5): each signal's pair
    // there (comparePairs) drawn dashed beside it, sharing its scale,
    // on the first's axis at compareOffsetMs (its own time plus the
    // offset). alignOn says how the two line up: "start", "utc", "marker"
    // (alignLabel), "rise" or "fall" (the signal alignSignal, its idOf,
    // and its pair there reaching alignLevel, in display units, after
    // being alignHysteresis beyond it), "engine"
    // (each engine's first start: RPM reaching 500 after being below 100)
    // or "pull" (the start of a pull chosen in each: alignPullHere of
    // pullsHere on alignPullThere of pullsThere)
    readonly property bool comparing: compare.state !== RecordingView.Closed
    property string compareTitle: ""
    // The Compare controls shown above the chart (the title row's Compare...
    // opens them; a compare started from anywhere does too)
    property bool compareOpen: false
    property bool compareAligned: false
    property real compareOffsetMs: 0
    property string compareStatus: ""
    // Each signal's pair there by index (ComparePairs.made), at either list's change
    property var comparePairs: []
    property real compareRowsPerSecond: 0
    // The comparison's difference traces (ADR-0056's appendix "a difference
    // in the viewer"): for each plotted number whose pair there is in the
    // same unit, the compared one's mean less
    // this one's in each column, a column the same stretch of the axis in
    // both (RecordingView.columnMeans)
    property bool differenceShown: false
    // The last answer, drawn while its pairs and offset still hold: { key,
    // pairsKey, t0, t1, rows: { <this recording's index>: [a number a
    // column, NaN where either is empty] } }; null with none
    property var difference: null
    // The grid the chart last drew, and the ask under way for it: each
    // view's request number, then its answer
    property var differenceWanted: null
    property var differenceAsk: null
    // The grid a view refused to be asked for and how often: { key, count }.
    // Asked once more at a later paint, then left until the grid changes
    property var differenceRefused: null
    // The first's signal index -> the compared one's value at the cursor
    property var compareReadout: ({})
    property string alignOn: "start"
    property string alignLabel: ""
    property string alignSignal: ""
    property string alignSignalName: ""
    property real alignLevel: NaN
    property real alignHysteresis: 0
    // Each recording's pulls in time order (RecordingView.stretches; null
    // until its answer), and the place of the one chosen in each
    property var pullsHere: null
    property var pullsThere: null
    // This recording's stretches, pulls and cruises in time order, from one
    // ask per open (RecordingView.stretches; null until answered): the Jump
    // row's list, the bands' and the alignment's pulls here. A failed ask is
    // stretchFailed, never an empty list
    property var stretchList: null
    property bool stretchFailed: false
    property int stretchAsk: 0
    property bool bandsShown: true
    // The Stretches panel's levels: what is in force, the panel above the
    // chart (in its place on a phone), and the words of the last refusal
    readonly property var stretchRules: refreshPart.readRules(viewerSettings.stretchRules)
    property bool stretchesOpen: false
    property string rulesProblem: ""
    // The levels the last accepted ask went out with (the stored ones, or
    // the defaults when those were refused) and the unit system they were
    // read in: Compare's ask takes the same, and a change of units asks again
    property var askedRules: ({})
    property int stretchUnits: 0
    // The stretch last jumped to: its kind and its place in that kind's
    // list, -1 for none
    property string jumpKind: ""
    property int jumpIndex: -1
    onStretchListChanged: chart.requestPaint()
    onBandsShownChanged: chart.requestPaint()
    onJumpIndexChanged: chart.requestPaint()
    // The Conditions panel (ADR-0063 section 10): open in the Stretches
    // panel's place, one of the two at a time, and the entry of
    // conditionList last shown from it, -1 for none
    property bool conditionsOpen: false
    property int conditionPick: -1
    onConditionPickChanged: chart.requestPaint()
    // An ask that failed (the file changed since the open): not a
    // recording with no pull
    property bool pullsHereFailed: false
    property bool pullsThereFailed: false
    property int pullsThereAsk: 0
    property int alignPullHere: 0
    property int alignPullThere: 0
    onAlignPullHereChanged: comparePart.align()
    onAlignPullThereChanged: comparePart.align()
    onAlignOnChanged: comparePart.align()
    onAlignLabelChanged: comparePart.align()
    onAlignSignalChanged: comparePart.align()
    onAlignLevelChanged: comparePart.align()
    onAlignHysteresisChanged: comparePart.align()

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

    RecordingFrameTest { id: frameTestPart; host: root }

    RecordingReport { id: reportPart; host: root }

    RecordingPanes { id: panesPart; host: root }

    RecordingOpen { id: openPart; host: root }

    RecordingCursors { id: cursorsPart; host: root }

    RecordingSeries { id: seriesPart; host: root }

    RecordingTexts { id: textsPart; host: root }

    RecordingFormat { id: formatPart; host: root }

    RecordingRowInfo { id: rowInfoPart; host: root }

    RecordingDecode { id: decodePart; host: root }

    RecordingCompare { id: comparePart; host: root }

    readonly property bool importReady: importKind === RecordingImport.Candump
                                        || importMap.toString() !== ""
                                        || (importUsesGuess && guessPreview.ready)

    // The view, kept inside the axis and no narrower than minSpanMs.
    // A change within settleMs of the last is the view moving (a drag, a
    // pinch, the wheel): the chart draws it without antialiasing until the
    // view rests
    readonly property int settleMs: 150
    readonly property real handleDropPx: 4
    // What the row says for no pull: a recording with neither a throttle nor
    // an engine-load signal has nothing a pull is judged by, whatever the
    // levels, and the row says so. A phone's row must stay one line at any
    // font, so it keeps the plain words and the reason is the label's spoken
    // name (jumpStatusSpoken)
    readonly property string noPullWhy: recording.pullsLackSignals
        ? qsTr("No pull found (no throttle or load signal)") : qsTr("No pull found")
    readonly property string noPullText: Theme.compact ? qsTr("No pull found") : noPullWhy
    // The Jump row's line: where the last jump went while the cursor is
    // still at its start, else how many of each were found
    readonly property string jumpStatus: {
        if (stretchFailed)
            return qsTr("Its stretches couldn't be read")
        if (stretchList === null)
            return qsTr("Looking for stretches…")
        const list = jumpIndex >= 0 ? comparePart.stretchesOf(jumpKind) : []
        const at = jumpIndex >= 0 ? list[jumpIndex] : undefined
        if (at !== undefined && hasCursor && Math.abs(cursorMs - at.fromMs) < 0.5)
            return qsTr("%1 %2 of %3 · %4").arg(jumpKind === "pull" ? qsTr("Pull") : qsTr("Cruise"))
                       .arg(jumpIndex + 1).arg(list.length).arg(formatPart.timeText(at.fromMs, true))
        const pulls = comparePart.stretchesOf("pull").length
        const cruises = comparePart.stretchesOf("cruise").length
        return (pulls === 0 ? noPullText
                            : pulls === 1 ? qsTr("1 pull") : qsTr("%1 pulls").arg(pulls)) + " · "
               + (cruises === 0 ? qsTr("No cruise found")
                                : cruises === 1 ? qsTr("1 cruise") : qsTr("%1 cruises").arg(cruises))
    }
    // What a screen reader says for the status: the reason in full
    readonly property string jumpStatusSpoken: jumpStatus.replace(noPullText, noPullWhy)
    // RecordingView.findings and markers copied once at each change, with
    // the fields the chart's paint reads: the bridge map rule
    property var chartFindings: recording.findings.map(f => ({ kind: f.kind, atMs: f.atMs,
                                                                lengthMs: f.lengthMs }))
    property var chartMarkers: recording.markers.map(m => ({ timeMs: m.timeMs, label: m.label }))

    // The recording's conditions drawn none, warning, then danger, so the
    // louder level is on top where two overlap
    // RecordingView.conditionSpans copied once into plain objects at each
    // open: a paint that read the C++ maps would make a wrapper for each
    // span and each stale part, the garbage behind Qt 6.12's sweep crash.
    // Nothing else reads conditionSpans
    readonly property var conditionList: recording.conditionSpans.map(s => ({
        key: s.key, name: s.name, level: s.level, on: s.on, fromMs: s.fromMs, toMs: s.toMs,
        heldStart: s.heldStart, openEnd: s.openEnd,
        stale: Array.from(s.stale, p => ({ fromMs: p.fromMs, toMs: p.toMs })) }))
    readonly property var conditionsByLevel: conditionList.slice()
        .sort((a, b) => (a.on ? a.level : -1) - (b.on ? b.level : -1))
    // The conditions' two 4 px rows, the lower's top at y: an alert's ON
    // spans on it in their level's role, everything else 6 px above in
    // textDim, so a span inside an alert's keeps pixels of its own. A
    // stretch with no new data is dashed 3 px on, 3 off from its own start,
    // so the dashes stay put as the view pans. spans: conditionsByLevel
    function drawConditions(ctx, y, left, right, xOf, spans) {
        const dashed = (top, origin, x0, x1) => {
            for (let x = origin + Math.max(0, Math.floor((x0 - origin) / 6)) * 6; x < x1; x += 6) {
                const a = Math.max(x, x0), b = Math.min(x + 3, x1)
                if (b > a)
                    ctx.fillRect(a, top, b - a, 4)
            }
        }
        for (const s of spans) {
            const from = xOf(s.fromMs), to = Math.max(xOf(s.toMs), from + 2)
            const x0 = Math.max(left, from), x1 = Math.min(right, to)
            if (x1 <= x0)
                continue
            const top = s.on && s.level > 0 ? y : y - 6
            ctx.fillStyle = conditionsPanel.colorOf(s)
            if (!s.on) {
                dashed(top, from, x0, x1)
                continue
            }
            let at = x0
            for (const p of s.stale) {
                const p0 = Math.max(at, xOf(p.fromMs)), p1 = Math.min(x1, xOf(p.toMs))
                if (p1 <= p0)
                    continue
                ctx.fillRect(at, top, p0 - at, 4)
                dashed(top, xOf(p.fromMs), p0, p1)
                at = p1
            }
            if (x1 > at)
                ctx.fillRect(at, top, x1 - at, 4)
        }
    }

    RecordingDialogs { id: recordingDialogs; host: root }

    // The signal rows' filter: Live has its own
    SignalFilter {
        id: signalFilter
    }

    // The Export panel: it takes the chart's place while open
    property bool exportOpen: false
    readonly property bool exportShown: exportOpen && recording.state === RecordingView.Open
                                        && !chartMaximized
    // The export's columns name each signal by its id: a key or a name one
    // signal has, as core takes them
    readonly property var plottedNames: recording.signalList
        .filter(s => !s.isText && root.plotted[s.index] === true).map(s => panesPart.idOf(s))
    // The plotted signals, when none of them draws: no values in this
    // recording, nor in the compared one's signal of the same name. The
    // chart then has nothing to draw and says so
    readonly property var plottedNoValues: {
        const plotted = recording.signalList.filter(s => !s.isText && root.plotted[s.index] === true)
        return plotted.length > 0 && plotted.every(s => !reportPart.draws(s)) ? plotted.map(s => s.name) : []
    }
    // The last Save report's outcome in words, "" before one on this
    // recording
    property string reportNote: ""
    property bool reportFailed: false

    RecordingRefresh { id: refreshPart; host: root }

    RecordingViews { id: recordingViews; host: root }

    RecordingEvents { id: recordingEvents; host: root }

    onVisibleChanged: if (visible && !viewing) refreshPart.refresh()
    Component.onCompleted: refreshPart.refresh()

    RecordingWatchers { id: recordingWatchers; host: root }

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

        RecordingImportPanel {
            id: importPanel
            host: root
        }

        // The last import's words: what it made, or why not
        Label {
            objectName: "importNote"
            visible: root.importNote !== ""
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
                  : qsTr("No recordings yet. Start log, in the top bar while connected, makes one; Settings, Recording keeps every session; Import brings in another logger's file.")
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
                onClicked: openPart.clearBaseline()
            }
        }

        RecordingList {
            id: recordingList
            host: root
        }

        // ---- The viewer: the chart and its signals first, then what
        // applies to the whole recording under "About this recording" ----
        RecordingViewerTitle { id: recordingViewerTitle; host: root }

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
                onClicked: openPart.backToList()
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
            onCloseRequested: reportPart.closeExport()
        }

        // Compare's controls, over the chart: the recording to overlay, how
        // the two line up, and what came of it
        ColumnLayout {
            id: compareSlot
            objectName: "compareSlot"
            visible: root.compareOpen && recording.state === RecordingView.Open && !root.exportShown
            Layout.fillWidth: true
            spacing: Theme.spacing
        }

        // The levels that make a stretch: over the chart, so the bands stay
        // in sight while they are tuned; on a phone in the chart's place
        StretchesPanel {
            id: stretchesPanel
            objectName: "stretchesPanel"
            visible: root.stretchesOpen && recording.state === RecordingView.Open && !root.exportShown
            Layout.fillWidth: true
            Layout.fillHeight: Theme.compact
            view: recording
            rules: root.stretchRules
            problem: root.rulesProblem
            bandsShown: root.bandsShown
            onBandsToggled: root.bandsShown = !root.bandsShown
            onApplyRequested: rules => comparePart.applyStretchRules(rules)
            onCloseRequested: root.stretchesOpen = false
        }
        // The recording's conditions, each row centering the view on its
        // entry: in the same place as Stretches, one of the two at a time
        ConditionsPanel {
            id: conditionsPanel
            objectName: "conditionsPanel"
            visible: root.conditionsOpen && recording.state === RecordingView.Open && !root.exportShown
            Layout.fillWidth: true
            Layout.fillHeight: Theme.compact
            spans: root.conditionList
            info: recording.info
            timeText: ms => formatPart.timeText(ms, true)
            lengthText: ms => formatPart.lengthText(ms)
            picked: root.conditionPick
            onShowRequested: i => cursorsPart.showCondition(i)
            onCloseRequested: root.conditionsOpen = false
        }

        // The Jump row over the chart on the desktop; on a phone under it,
        // so a recording still opens onto its chart
        ColumnLayout {
            id: jumpAbove
            visible: chartFrame.visible && !Theme.compact
            Layout.fillWidth: true
            spacing: 0
        }
        RecordingJumpRow {
            id: jumpRow
            objectName: "jumpRow"
            parent: Theme.compact ? jumpBelow : jumpAbove
            visible: chartFrame.visible
            Layout.fillWidth: true
            status: root.jumpStatus
            statusSpoken: root.jumpStatusSpoken
            canPreviousPull: comparePart.previousStretch("pull") >= 0
            canNextPull: comparePart.nextStretch("pull") >= 0
            canPreviousCruise: comparePart.previousStretch("cruise") >= 0
            canNextCruise: comparePart.nextStretch("cruise") >= 0
            bandsShown: root.bandsShown
            stretchesOpen: root.stretchesOpen
            conditionsShown: root.conditionList.length > 0 || recording.info.conditionEventsLost > 0
            conditionsOpen: root.conditionsOpen
            onJumpRequested: (kind, direction) => comparePart.jump(kind, direction)
            onBandsToggled: root.bandsShown = !root.bandsShown
            onStretchesToggled: {
                root.stretchesOpen = !root.stretchesOpen
                if (root.stretchesOpen)
                    root.conditionsOpen = false
            }
            onConditionsToggled: {
                root.conditionsOpen = !root.conditionsOpen
                if (root.conditionsOpen)
                    root.stretchesOpen = false
            }
        }

        RecordingChart {
            id: chartFrame
            host: root
        }

        // The panes a signal can be drawn in (its row's Pane button, or a
        // long press on its label in the chart)
        Menu {
            id: paneMenu
            objectName: "paneMenu"
            Instantiator {
                model: root.paneMenuItems
                delegate: MenuItem {
                    required property var modelData
                    objectName: "paneChoice_" + (modelData.key === "" ? "unit" : modelData.key === "new" ? "new" : modelData.key)
                    text: modelData.text
                    checkable: true
                    checked: modelData.checked
                    onTriggered: panesPart.choosePane(modelData.key)
                }
                onObjectAdded: (index, object) => paneMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => paneMenu.removeItem(object)
            }
        }

        // The splitter: drag it to make the chart taller or shorter, kept
        // between runs; a double-tap gives the chart the page's own height
        RecordingSplitter { id: recordingSplitter; host: root }
        ColumnLayout {
            id: jumpBelow
            visible: chartFrame.visible && Theme.compact
            Layout.fillWidth: true
            // The splitter's grip reaches this far below it (its margin less
            // the column's spacing), so the Jump row's buttons stay out of it
            Layout.topMargin: root.phoneShape ? 8 : 0
            spacing: 0
        }

        // Maximized on a narrow page, the views' switch has a row of its own
        RowLayout {
            id: switchRowSlot
            visible: root.chartMaximized && recording.hasFrames && !root.switchInZoomRow
        }

        // The span line: where the two cursors are and how far apart, or
        // once, the first cursor's hint for the second. Under the zoom row
        // on a phone, at its right on the desktop
        RecordingSpanLine { id: recordingSpanLine; host: root }

        // The zoom row's place on the page; on the pinned form it leads the
        // shown view's list
        ColumnLayout {
            id: zoomHome
            // A visible layout counts in its column's spacing even when empty
            visible: zoomBlock.parent === zoomHome && recording.state === RecordingView.Open
            Layout.fillWidth: true
            // A layout fills by default: this one keeps its content's height
            Layout.fillHeight: false
            RecordingZoomRow {
                id: zoomBlock
                host: root
            }
        }

        // The signal area's two views, the values at the cursor or the frames
        // in view (hidden for a recording with no frames.csv), then the
        // filter: one row on the desktop, a row each on a phone
        RecordingDetailRow { id: recordingDetailRow; host: root }
        RecordingFramesView {
            id: viewerFrames
            host: root
        }

        RecordingSignalRows {
            id: signalRows
            host: root
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
            RecordingSpanStats {
                id: spanStatsBlock
                host: root
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
            RecordingAbout {
                id: aboutBlock
                host: root
            }
        }
        RecordingFindings {
            id: findingList
            host: root
        }

        // The rest stays at the top
        Item {
            visible: root.viewing && !root.chartMaximized && !root.pinnedViewer
            Layout.fillHeight: true
        }
    }
}
