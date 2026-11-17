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
    property alias spanStatsHome: spanStatsHome
    property alias recording: recording
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
    property alias viewFrames: viewFrames
    property alias aboutActions: aboutBlock.aboutActions
    property alias alignChoice: aboutBlock.alignChoice
    property alias aboutHome: aboutHome
    property alias compareSlot: compareSlot
    property alias topNote: topNote
    property alias guessPreview: importPanel.guessPreview
    property alias importInterface: importPanel.importInterface
    property alias importLabel: importPanel.importLabel
    property alias mapDialog: mapDialog
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
    onBaselineFolderChanged: checkBaseline()

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
    // A focused box (a ComboBox, which keeps Home, End and its letters for its
    // own choice) keeps those keys: told by its `popup`, since a style's box
    // isn't a ComboBox to `instanceof`. Left and Right it doesn't use, and a
    // press in the plot takes the focus back from it (chartTap)
    readonly property bool boxFocused: root.Window.activeFocusItem !== null
                                       && root.Window.activeFocusItem.popup !== undefined
    // F on a keyboard; a text field takes its own letters first
    Shortcut {
        sequence: "F"
        enabled: root.visible && recording.state === RecordingView.Open && !Theme.mobile
                 && !root.boxFocused
        onActivated: root.toggleMaximized()
    }
    // The arrows step a cursor a sample of the snap's signal, B says which
    // cursor they move, Home and End go to the recording's ends
    readonly property bool cursorKeys: root.visible && recording.state === RecordingView.Open
                                       && !Theme.mobile
    readonly property bool editKeys: cursorKeys && !root.boxFocused
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
        enabled: root.editKeys
        onActivated: root.cursorToEnd(false)
    }
    Shortcut {
        sequence: "End"
        enabled: root.editKeys
        onActivated: root.cursorToEnd(true)
    }
    Shortcut {
        sequence: "B"
        enabled: root.editKeys
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
    function viewNameOf(folder) {
        const parts = String(folder).replace(/[\\/]+$/, "").split(/[\\/]/)
        return parts[parts.length - 1]
    }
    readonly property string viewName: recording.state === RecordingView.Open ? viewNameOf(recording.folder) : ""
    readonly property var viewEntry: viewName !== "" ? viewsBook[viewName] : undefined
    // Signal name -> pane key, and pane count -> heights' shares
    readonly property var viewAssign: viewEntry !== undefined ? viewEntry.assign : ({})
    readonly property var paneShares: viewEntry !== undefined ? viewEntry.shares : ({})
    function writeView(patch) {
        if (viewName === "")
            return
        viewerSettings.views = JSON.stringify(ChartLayout.viewsWith(viewsBook, viewName, patch, Date.now()))
        chart.requestPaint()
    }
    // The assignment as the layout reads it: signal index -> pane key, the
    // names the recording lacks passed by
    function assignRows() {
        const out = {}
        for (const s of recording.signalList) {
            const key = viewAssign[s.name]
            if (key !== undefined)
                out[s.index] = key
        }
        return out
    }
    // A signal's pane, "" for the one its unit makes
    function setPaneOf(name, key) {
        const next = Object.assign({}, viewAssign)
        if (key === "")
            delete next[name]
        else
            next[name] = key
        writeView({ assign: next })
    }
    // The panes a signal can join: each ordinary pane now drawn, named by the
    // signals plotted in it
    function paneChoices() {
        const l = chart.drawn
        const out = []
        if (!l)
            return out
        const byKey = {}
        for (const tr of l.traces) {
            if (tr.pinned || tr.pane === undefined)
                continue
            const key = l.panes[tr.pane].key
            if (key === "mixed" || key === "difference")
                continue
            const s = recording.signalList.find(x => x.index === tr.row)
            if (s === undefined)
                continue
            if (byKey[key] === undefined) {
                byKey[key] = []
                out.push({ key: key, names: byKey[key] })
            }
            if (byKey[key].indexOf(s.name) < 0)
                byKey[key].push(s.name)
        }
        return out.map(c => ({ key: c.key, label: c.names.join(", ") }))
    }
    // A new pane's key: the lowest "own:n" no pane or assignment holds
    function newPaneKey() {
        const used = Object.keys(viewAssign).map(n => viewAssign[n])
        for (const c of paneChoices())
            used.push(c.key)
        let n = 1
        while (used.indexOf("own:" + n) >= 0)
            ++n
        return "own:" + n
    }
    // The pane a plotted signal is drawn in: its label, "" when not drawn
    function paneLabelOf(index) {
        const l = chart.drawn
        if (!l)
            return ""
        for (const tr of l.traces) {
            if (tr.row === index && !tr.pinned && tr.pane !== undefined)
                return l.panes[tr.pane].label
        }
        return ""
    }
    // The menu's entries for one signal: [{ text, key, checked }], key "" for
    // the unit's own pane, "new" for a pane of its own
    property string paneMenuName: ""
    property var paneMenuItems: []
    function openPaneMenu(index, anchor) {
        const s = recording.signalList.find(x => x.index === index)
        if (s === undefined || s.isText || plotted[index] !== true)
            return
        paneMenuName = s.name
        const here = viewAssign[s.name]
        const items = [{ text: qsTr("By unit"), key: "", checked: here === undefined }]
        for (const c of paneChoices())
            items.push({ text: qsTr("With %1").arg(c.label), key: c.key, checked: here === c.key })
        items.push({ text: qsTr("New pane"), key: "new", checked: false })
        paneMenuItems = items
        paneMenu.popup(anchor)
    }
    function choosePane(key) {
        setPaneOf(paneMenuName, key === "new" ? newPaneKey() : key)
    }
    // The signal whose value label lies at (x, y) in the chart, -1 for none
    function labelRowAt(x, y) {
        const l = chart.drawn
        if (!l)
            return -1
        for (const tr of l.traces) {
            if (tr.pinned || tr.labelY === undefined || !tr.labeled)
                continue
            if (x >= l.labelX - 8 && Math.abs(y - tr.labelY) <= ChartLayout.lineH)
                return tr.row
        }
        return -1
    }
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
        writeView({ shares: next })
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
    // Signal index -> its trace's color slot, by plotted order: a signal
    // takes the lowest free slot when plotted and keeps it while it stays
    // plotted, so unplotting another recolors nothing
    property var slots: ({})
    onPlottedChanged: reconcileSlots()
    function reconcileSlots() {
        const next = {}
        const taken = {}
        for (const k of Object.keys(slots)) {
            if (plotted[k] === true) {
                next[k] = slots[k]
                taken[slots[k]] = true
            }
        }
        for (const k of Object.keys(plotted).map(Number).sort((a, b) => a - b)) {
            if (plotted[k] !== true || next[k] !== undefined)
                continue
            let slot = 0
            while (taken[slot])
                ++slot
            next[k] = slot
            taken[slot] = true
        }
        slots = next
    }
    // Only a plotted signal is drawn, and each plotted one has a slot, so a
    // trace never falls back to its row; the fallback is for any other caller
    function slotOf(index) {
        const slot = slots[index]
        return slot === undefined ? index : slot
    }
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
    function noteHead(role, head) {
        if (role === "values") valuesHead = head
        else if (role === "frames") framesHead = head
        else if (role === "ids") idsHead = head
        else aboutHead = head
    }
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
    // The Compare controls shown above the chart (the title row's Compare...
    // opens them; a compare started from anywhere does too)
    property bool compareOpen: false
    property bool compareAligned: false
    property real compareOffsetMs: 0
    property string compareStatus: ""
    property var compareByName: ({})
    property real compareRowsPerSecond: 0
    // The comparison's difference traces (ADR-0056's appendix "a difference
    // in the viewer"): for each plotted number the compared recording holds
    // under the same name in the same unit, the compared one's mean less
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
    readonly property var stretchRules: readRules(viewerSettings.stretchRules)
    property bool stretchesOpen: false
    property string rulesProblem: ""
    // The levels the last accepted ask went out with (the stored ones, or
    // the defaults when those were refused) and the unit system they were
    // read in: Compare's ask takes the same, and a change of units asks again
    property var askedRules: ({})
    property int stretchUnits: 0
    function readRules(text) {
        try {
            const v = JSON.parse(text)
            return v !== null && typeof v === "object" && !Array.isArray(v) ? v : ({})
        } catch (e) {
            return ({})
        }
    }
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

    // A file picked to import: its kind guessed, the rest asked for. name is
    // the picker's for it, else the URL's last part
    function chooseImportFile(url, name) {
        importFile = url
        importFileName = name || fileNameOf(url)
        importKind = Bridge.recordingImport.guessKind(url)
        importMap = ""
        importAsksInterface = false
        importInterface.text = ""
        importLabel.text = ""
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
    function chooseImportMap(url, name) {
        importMap = url
        importMapFileName = name || fileNameOf(url)
        importNote = ""
        importFailed = false
        guessPreview.clear()
    }

    // The picker couldn't give a file or a map
    function importPickFailed(words) {
        importNote = words
        importFailed = true
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
        // An import's words belong to the import, not to the recording opened next
        importNote = ""
        importFailed = false
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
        importNote = ""
        closeExport()
        stretchesOpen = false
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
    // A Conditions row: the view centered on its entry and the cursor at its
    // start, as a finding's row; a phone's panel gives the chart back
    function showCondition(i) {
        const s = conditionList[i]
        if (s === undefined)
            return
        conditionPick = i
        centerOn((s.fromMs + s.toMs) / 2)
        setCursor(s.fromMs)
        if (Theme.compact)
            conditionsOpen = false
    }

    // The cursors' time tags on the axis row, for the layout to keep its
    // captions out from under them: the time and the tag's width
    function cursorTags() {
        const out = []
        for (const ms of [cursorMs, cursorBMs]) {
            if (isFinite(ms) && ms >= viewT0 && ms <= viewT1)
                out.push({ ms: ms, width: Math.max(phoneShape ? Theme.touchTarget : 24,
                                                   timeTextWidth(ms) + 8) })
        }
        return out
    }
    FontMetrics { id: tagMetrics; font.pixelSize: ChartLayout.labelPx; font.family: Theme.valueFamily }
    function timeTextWidth(ms) {
        return tagMetrics.advanceWidth(timeText(ms, true))
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
        refreshCompareReadout()
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
        refreshCompareReadout()
        spanHintUp = false
        askSpanStats()
        chart.requestPaint()
    }
    // The compared recording's values at the cursor the rows read: the first,
    // or the earlier of two
    function refreshCompareReadout() {
        compareReadout = compareValuesAt(hasSpan ? spanFromMs : cursorMs)
    }
    function clearCursorB() {
        cursorBMs = NaN
        readoutB = ({})
        refreshCompareReadout()
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
    // The nearest sample can lie past the view's end by half a sample when the
    // end is off the grid (a wheel zoom): then the one in force at the end,
    // so the handle never drops out of view
    function snappedInView(t) {
        const s = snapped(t)
        if (s < viewT0)
            return viewT0
        const first = snapSignal()
        if (s <= viewT1 || !first)
            return Math.min(s, viewT1)
        const at = recording.valuesAt([first.index], viewT1)
        return at.length > 0 && at[0].atMs >= 0 ? at[0].atMs : viewT1
    }
    function dropHandle(isB) {
        const t = snappedInView(isB ? cursorBMs : cursorMs)
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

    // What a difference is taken between: a plotted number here and the
    // compared recording's signal of the same name in the same unit (the
    // unit's case aside), each with samples
    function differencePairs() {
        if (!compareAligned || recording.state !== RecordingView.Open)
            return []
        const sigs = recording.signalList
        const theirs = compare.signalList
        return sigs.filter(s => !s.isText && root.plotted[s.index] && s.samples > 0)
                   .map(s => ({ mine: s.index, theirs: root.compareByName[s.name] }))
                   .filter(p => p.theirs !== undefined && !theirs[p.theirs].isText
                                && theirs[p.theirs].samples > 0
                                && theirs[p.theirs].unit.toLowerCase() === sigs[p.mine].unit.toLowerCase())
    }
    // What an answer holds for: the pairs, the offset and the units
    function differencePairsKey(pairs) {
        return pairs.map(p => p.mine + ":" + p.theirs).join(",") + "|" + compareOffsetMs
               + "|" + Bridge.unitSystem
    }
    // Asks both recordings for the wanted grid's means, unless it is
    // answered or asked already
    function askDifference() {
        const w = differenceWanted
        if (!differenceShown || !compareAligned || w === null || w.pairs.length === 0)
            return
        if ((difference !== null && difference.key === w.key)
                || (differenceAsk !== null && differenceAsk.key === w.key))
            return
        const refused = differenceRefused !== null && differenceRefused.key === w.key
                        ? differenceRefused.count : 0
        if (refused >= 2)
            return
        // The grid's ends at whole microseconds, so each column's two spans
        // lie exactly the offset apart and hold the same samples
        const t0 = Math.round(w.g0 * 1000) / 1000
        const t1 = Math.round(w.g1 * 1000) / 1000
        // A view that refuses (0) answers nothing: the other isn't asked
        const mine = recording.columnMeans(w.pairs.map(p => p.mine), t0, t1, w.gn)
        const theirs = mine > 0 ? compare.columnMeans(w.pairs.map(p => p.theirs), t0 - compareOffsetMs,
                                                      t1 - compareOffsetMs, w.gn) : 0
        if (mine > 0 && theirs > 0) {
            differenceRefused = null
            differenceAsk = { key: w.key, pairsKey: w.pairsKey, pairs: w.pairs, t0: t0, t1: t1,
                              mine: mine, theirs: theirs, mineMeans: null, theirMeans: null }
        } else {
            differenceAsk = null
            differenceRefused = { key: w.key, count: refused + 1 }
        }
    }
    // One recording's means for the ask under way; with both, the difference
    function differenceAnswered(here, request, means) {
        const a = differenceAsk
        if (a === null || request !== (here ? a.mine : a.theirs))
            return
        if (here)
            a.mineMeans = means
        else
            a.theirMeans = means
        if (a.mineMeans === null || a.theirMeans === null)
            return
        differenceAsk = null
        // A read that failed answers with no rows: no difference to draw
        const rows = {}
        if (a.mineMeans.length === a.pairs.length && a.theirMeans.length === a.pairs.length) {
            for (let k = 0; k < a.pairs.length; ++k) {
                const m = a.mineMeans[k].means
                const t = a.theirMeans[k].means
                const d = []
                for (let c = 0; c < m.length; ++c)
                    d.push(t[c] - m[c])
                rows[a.pairs[k].mine] = d
            }
        }
        difference = { key: a.key, pairsKey: a.pairsKey, t0: a.t0, t1: a.t1, rows: rows }
        chart.requestPaint()
    }
    function setDifferenceShown(on) {
        differenceShown = on
        chart.requestPaint()
    }
    // Another recording on either side: an answer for the one before
    // holds for nothing
    function forgetDifference() {
        difference = null
        differenceWanted = null
        differenceAsk = null
        differenceRefused = null
    }

    // The rows a second of a recording's signals at those indexes
    function rowsEachSecond(list, indexes, lengthMs) {
        let rows = 0
        for (const i of indexes)
            rows += list[i].samples
        return rows / Math.max(1, lengthMs / 1000)
    }

    // The chart's series: each plotted number's columns over the view, its
    // label the cursor's value while that's in view, then the compared
    // recording's of the same names, dashed. Two pixels a column, or a
    // second when that would draw more than rawRowsPerFrame rows or hold
    // more than rawRowsCached
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
        const theirs = compareAligned ? compare.signalList : []
        const pairs = !compareAligned ? []
            : plotted.map(i => ({ mine: i, theirs: root.compareByName[sigs[i].name] }))
                     .filter(p => p.theirs !== undefined && !theirs[p.theirs].isText
                                  && theirs[p.theirs].samples > 0)
        const spanS = (viewT1 - viewT0) / 1000
        let n = Math.max(1, Math.floor((chart.drawn ? chart.drawn.plotW : widthPx * 0.8) / 2))
        const drawn = rowsEachSecond(sigs, indexes, durationMs())
                      + rowsEachSecond(theirs, pairs.map(p => p.theirs), compare.durationMs)
        const held = Math.max(rowsPerSecond, compareAligned ? compareRowsPerSecond : 0)
        if (spanS * drawn > rawRowsPerFrame || spanS * held > rawRowsCached)
            n = Math.max(1, Math.min(n, Math.floor(spanS)))
        // The labels follow the readout's A, the earlier of the two cursors,
        // the compared recording's too
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
                     slot: root.slotOf(c.index),
                     scaleKey: c.index, decimals: s.decimals, unit: s.unit,
                     labelValue: labelOf(root.readoutFrom[c.index], fromInView),
                     labelMs: labelMsOf(root.readoutFrom[c.index], 0, fromInView) }
        })
        if (pairs.length === 0)
            return out
        const cols = compare.columns(pairs.map(p => p.theirs), g0 - compareOffsetMs,
                                     g1 - compareOffsetMs, gn)
        for (let k = 0; k < cols.length; ++k) {
            const mine = pairs[k].mine
            const s = theirs[pairs[k].theirs]
            out.push({ columns: { lows: cols[k].lows, highs: cols[k].highs, t0: g0, t1: g1 },
                       row: mine, slot: root.slotOf(mine),
                       scaleKey: mine, dash: Theme.seriesCompareDash(root.slotOf(mine)), decimals: s.decimals,
                       unit: s.unit, labelValue: labelOf(root.compareReadout[mine], fromInView),
                       labelMs: labelMsOf(root.compareReadout[mine], compareOffsetMs, fromInView) })
        }
        if (!differenceShown)
            return out
        // Each difference in a pane of its own quantity, under the signals'.
        // The last answer draws where its own columns lie while a newer
        // grid's is read, which waits for the view to rest
        const apart = differencePairs()
        const pairsKey = differencePairsKey(apart)
        const key = [g0, g1, gn, pairsKey].join("|")
        if (difference !== null && difference.pairsKey === pairsKey) {
            for (const p of apart) {
                const d = difference.rows[p.mine]
                if (d === undefined)
                    continue
                const s = sigs[p.mine]
                const here = labelOf(root.readoutFrom[p.mine], fromInView)
                const there = labelOf(root.compareReadout[p.mine], fromInView)
                out.push({ columns: { lows: d, highs: d, t0: difference.t0, t1: difference.t1 },
                           row: p.mine, slot: root.slotOf(p.mine), decimals: s.decimals, unit: s.unit,
                           pane: "difference " + ChartLayout.paneKey(s.unit), pinned: true,
                           paneLabel: qsTr("Δ %1").arg(s.unit), labelValue: there - here })
            }
        }
        if (differenceWanted === null || differenceWanted.key !== key)
            differenceWanted = { key: key, pairsKey: pairsKey, pairs: apart, g0: g0, g1: g1, gn: gn }
        if (!chart.moving)
            Qt.callLater(root.askDifference)
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
        compareOpen = true
        compareTitle = row.title
        compareAligned = false
        compareStatus = qsTr("Opening %1…").arg(row.title)
        compare.open(row.folder)
    }
    function stopComparing() {
        compareOpen = false
        compare.close()
        compareTitle = ""
        compareStatus = ""
        compareAligned = false
        compareReadout = ({})
        differenceShown = false
        forgetDifference()
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
        if (pullsHere === null && stretchAsk === 0)
            askStretches()
        if (pullsThere === null && pullsThereAsk === 0)
            pullsThereAsk = compare.stretches(true, false, forcedRules())
    }
    // One ask for both kinds: only the newest ask per view is answered, so
    // the Jump row, the bands and the alignment share it
    function askStretches() {
        if (recording.state !== RecordingView.Open)
            return
        rulesProblem = ""
        stretchUnits = Bridge.unitSystem
        let used = stretchRules
        let request = recording.stretches(true, true, used)
        if (request === 0 && recording.stretchesProblem !== "") {
            // A stored level that is none today: the defaults, with the
            // words under the panel's fields, never a refused ask
            rulesProblem = recording.stretchesProblem
            used = ({})
            request = recording.stretches(true, true, used)
        }
        stretchAsk = request
        if (request === 0) {
            stretchFailed = true
            pullsHereFailed = true
            pullsHere = []
        } else {
            askedRules = used
        }
    }
    // The levels in force for the compared recording's ask too, so a pull is
    // counted by one rule in both
    function forcedRules() {
        return askedRules
    }
    // Asks again with the levels in force, here and there: a change of unit
    // system reads the same typed numbers in the other unit
    function reaskStretches() {
        stretchList = null
        stretchFailed = false
        pullsHere = null
        pullsHereFailed = false
        jumpIndex = -1
        askStretches()
        if (compare.state === RecordingView.Open) {
            pullsThere = null
            alignPullThere = 0
            pullsThereAsk = compare.stretches(true, false, askedRules)
        }
    }
    // Asks again with the panel's levels. A refusal leaves the earlier answer
    // standing and its words under the fields
    function applyStretchRules(rules) {
        const request = recording.stretches(true, true, rules)
        if (request === 0) {
            rulesProblem = recording.stretchesProblem
            if (rulesProblem === "")
                stretchFailed = true
            return
        }
        rulesProblem = ""
        askedRules = rules
        stretchUnits = Bridge.unitSystem
        viewerSettings.stretchRules = JSON.stringify(rules)
        stretchAsk = request
        stretchList = null
        stretchFailed = false
        pullsHere = null
        jumpIndex = -1
        if (compare.state === RecordingView.Open) {
            pullsThere = null
            alignPullThere = 0
            pullsThereAsk = compare.stretches(true, false, rules)
        }
        // On a phone the panel stood where the chart is: back to it
        if (Theme.compact)
            stretchesOpen = false
    }
    function stretchesOf(kind) {
        return stretchList === null ? [] : stretchList.filter(s => s.kind === kind)
    }
    // Where a jump goes on from: the cursor, else the view's start
    function jumpBase() {
        return hasCursor ? cursorMs : viewT0
    }
    // The place in a kind's list of the first stretch after the base, or
    // of the last before it; -1 for none
    function nextStretch(kind) {
        const at = jumpBase()
        return stretchesOf(kind).findIndex(s => s.fromMs > at + 0.5)
    }
    function previousStretch(kind) {
        const at = jumpBase()
        const list = stretchesOf(kind)
        for (let i = list.length - 1; i >= 0; --i) {
            if (list[i].fromMs < at - 0.5)
                return i
        }
        return -1
    }
    // The stretch in view with a margin, the cursor at its start
    function jump(kind, direction) {
        const i = direction > 0 ? nextStretch(kind) : previousStretch(kind)
        if (i < 0)
            return
        const s = stretchesOf(kind)[i]
        const margin = Math.max(minSpanMs, (s.toMs - s.fromMs) / 4)
        setView(s.fromMs - margin, s.toMs + margin)
        setCursor(s.fromMs)
        jumpKind = kind
        jumpIndex = i
    }
    // What the row says for no pull: a recording with neither a throttle nor
    // an engine-load signal has nothing a pull is judged by, whatever the
    // levels, and the row says so; a phone's row must stay one line, so it
    // keeps the shorter words
    readonly property string noPullText: recording.pullsLackSignals
        ? (Theme.compact ? qsTr("No pull (no throttle or load)")
                         : qsTr("No pull found (no throttle or load signal)"))
        : qsTr("No pull found")
    // The Jump row's line: where the last jump went while the cursor is
    // still at its start, else how many of each were found
    readonly property string jumpStatus: {
        if (stretchFailed)
            return qsTr("Its stretches couldn't be read")
        if (stretchList === null)
            return qsTr("Looking for stretches…")
        const list = jumpIndex >= 0 ? stretchesOf(jumpKind) : []
        const at = jumpIndex >= 0 ? list[jumpIndex] : undefined
        if (at !== undefined && hasCursor && Math.abs(cursorMs - at.fromMs) < 0.5)
            return qsTr("%1 %2 of %3 · %4").arg(jumpKind === "pull" ? qsTr("Pull") : qsTr("Cruise"))
                       .arg(jumpIndex + 1).arg(list.length).arg(timeText(at.fromMs, true))
        const pulls = stretchesOf("pull").length
        const cruises = stretchesOf("cruise").length
        return (pulls === 0 ? noPullText
                            : pulls === 1 ? qsTr("1 pull") : qsTr("%1 pulls").arg(pulls)) + " · "
               + (cruises === 0 ? qsTr("No cruise found")
                                : cruises === 1 ? qsTr("1 cruise") : qsTr("%1 cruises").arg(cruises))
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
        // Tenths are rounded as a whole first: 119.96 s is 2:00.0, never 1:60.0
        const s = tenths ? Math.round(ms / 100) / 10 : ms / 1000
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

    // A row's length, whether it is a log of chosen signals or a whole
    // session, its signal count and its marks
    function rowFacts(row) {
        const parts = []
        if (row.durationMs >= 0)
            parts.push(lengthText(row.durationMs))
        if (row.log)
            parts.push(row.signals === 1 ? qsTr("log, 1 signal") : qsTr("log, %1 signals").arg(row.signals))
        else if (row.frames !== false)
            parts.push(qsTr("session"))
        else if (row.signals > 0)
            parts.push(row.signals === 1 ? qsTr("1 signal") : qsTr("%1 signals").arg(row.signals))
        if (row.marks > 0)
            parts.push(row.marks === 1 ? qsTr("1 mark") : qsTr("%1 marks").arg(row.marks))
        return parts.join(" · ")
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
    DocumentOpenDialog {
        id: importDialog
        objectName: "importDialog"
        title: qsTr("Import a log")
        nameFilters: [qsTr("Logs (*.csv *.log *.txt)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.chooseImportFile(importDialog.selectedFile, importDialog.selectedName)
        onFailed: words => root.importPickFailed(words)
    }
    DocumentOpenDialog {
        id: mapDialog
        objectName: "mapDialog"
        title: qsTr("The logger's column map")
        nameFilters: [qsTr("Column maps (*.json)"), qsTr("All files (*)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.chooseImportMap(mapDialog.selectedFile, mapDialog.selectedName)
        onFailed: words => root.importPickFailed(words)
    }

    // Save report's file: text first, the one a shop or a buyer reads
    DocumentSaveDialog {
        id: reportDialog
        objectName: "reportDialog"
        title: qsTr("Save the health report")
        defaultSuffix: "txt"
        nameFilters: [qsTr("Text (*.txt)"), qsTr("JSON (*.json)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        currentFile: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
                     + "/health-report.txt"
        onAccepted: root.saveReportTo(reportDialog.selectedFile,
                                      reportDialog.selectedNameFilter.index)
        onFailed: words => { root.reportFailed = true; root.reportNote = qsTr("Report not saved: %1").arg(words) }
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
        } else if (!recording.reporting) {
            // Not a report already under way: the note says this one didn't start
            reportFailed = true
            reportNote = qsTr("Report not saved: the report couldn't start")
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
        objectName: "recordingView"
        unitSystem: Bridge.unitSystem
        onReportSaved: result => root.reportSaved(result)
        onColumnMeansReady: (request, means) => root.differenceAnswered(true, request, means)
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
            if (request !== root.stretchAsk)
                return
            root.stretchAsk = 0
            root.stretchFailed = !ok
            root.stretchList = found
            root.jumpIndex = -1
            root.alignPullHere = 0
            root.pullsHereFailed = !ok
            root.pullsHere = found.filter(s => s.kind === "pull")
            if (root.comparing)
                root.align()
        }
        onStateChanged: {
            root.forgetDifference()
            root.reportNote = ""
            root.reportFailed = false
            root.pullsHere = null
            root.pullsHereFailed = false
            root.stretchAsk = 0
            root.stretchList = null
            root.stretchFailed = false
            root.jumpIndex = -1
            root.stretchesOpen = false
            root.conditionsOpen = false
            root.conditionPick = -1
            root.alignPullHere = 0
            if (recording.state !== RecordingView.Open)
                return
            const defaults = {}
            for (const s of recording.signalList)
                defaults[s.index] = s.plotted
            root.slots = ({})
            root.plotted = defaults
            // A kept arrangement is used again: stamped, so the oldest goes first
            viewerSettings.views = JSON.stringify(ChartLayout.viewsTouched(
                root.viewsBook, root.viewNameOf(recording.folder), Date.now()))
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
            root.askStretches()
        }
        onSignalListChanged: {
            // An open, a close or new units: a range set before means nothing
            root.paneRanges = ({})
            if (root.hasCursor)
                root.setCursor(root.cursorMs)
            if (root.hasCursorB)
                root.setCursorB(root.cursorBMs)
            if (recording.state === RecordingView.Open && root.stretchUnits !== Bridge.unitSystem)
                root.reaskStretches()
            chart.requestPaint()
        }
    }

    RecordingView {
        id: compare
        objectName: "compareView"
        unitSystem: Bridge.unitSystem
        onColumnMeansReady: (request, means) => root.differenceAnswered(false, request, means)
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
            root.forgetDifference()
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
                root.importOpen = false
                root.openFolder(result.folder)
                root.importNote = root.importedWords(result)
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
                onClicked: root.clearBaseline()
            }
        }

        RecordingList {
            id: recordingList
            host: root
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
                    // Selected while its controls are open above the chart
                    ThemedButton {
                        objectName: "compareButton"
                        compact: Theme.compact
                        Layout.fillWidth: Theme.compact
                        Layout.preferredWidth: Theme.compact ? 1 : -1
                        checkable: true
                        checked: root.compareOpen
                        text: qsTr("Compare…")
                        onClicked: root.compareOpen = !root.compareOpen
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
            onApplyRequested: rules => root.applyStretchRules(rules)
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
            timeText: ms => root.timeText(ms, true)
            lengthText: ms => root.lengthText(ms)
            picked: root.conditionPick
            onShowRequested: i => root.showCondition(i)
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
            canPreviousPull: root.previousStretch("pull") >= 0
            canNextPull: root.nextStretch("pull") >= 0
            canPreviousCruise: root.previousStretch("cruise") >= 0
            canNextCruise: root.nextStretch("cruise") >= 0
            bandsShown: root.bandsShown
            stretchesOpen: root.stretchesOpen
            conditionsShown: root.conditionList.length > 0 || recording.info.conditionEventsLost > 0
            conditionsOpen: root.conditionsOpen
            onJumpRequested: (kind, direction) => root.jump(kind, direction)
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
                    onTriggered: root.choosePane(modelData.key)
                }
                onObjectAdded: (index, object) => paneMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => paneMenu.removeItem(object)
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
                margin: root.phoneShape ? Math.max(0, (Theme.touchTarget - parent.height) / 2) : 0
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
            RecordingZoomRow {
                id: zoomBlock
                host: root
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
                SegmentRow {
                    parent: root.currentHead ? root.currentHead.switchSlot
                          : !root.chartMaximized ? detailSwitchSlot
                          : root.switchInZoomRow ? zoomSwitchSlot : switchRowSlot

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
