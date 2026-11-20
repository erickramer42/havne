// File/layer: app/qml/DashboardPage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit and its parts (QmlTest.dashedit_*), QmlTest.phone,
//          QmlTest.linkhealth, QmlTest.driveview (test_qml); BENCH, Manual smoke

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// The Dashboard (ADR-0057): one form's pages of gauges, drawn from the
// form's document. DashboardBook holds the document's rules; this page
// reads it, places each page's cells on a grid and binds each cell's
// gauge to the first of its refs the session has. Edit mode (section 5)
// changes the document through DashboardBook's writes and stores each
// change as it's made.
Page {
    id: root
    background: Rectangle { color: Theme.bg }

    readonly property bool compact: width < 600
    readonly property real margin: Theme.pageMargin
    // The tool row's width from which it holds every tool, Reset peaks
    // among them (header.holdsResetPeaks)
    readonly property real toolRowFull: 272
    readonly property real gap: compact ? 10 : 16
    // The gauges are one panel on both forms (ADR-0057's appendix,
    // section 3): the cells meet, the grid draws the surface, the edge and
    // the hairlines between neighbors, and no cell paints its own
    readonly property bool joined: true
    readonly property real cellGap: joined ? 0 : gap
    // The space above the header
    readonly property real topGap: drive.driving ? 0 : phoneForm ? 12 : margin
    // The desktop's page strip shows, and its shown tab meets the panel:
    // not with an edit panel open, which sits a gap under the strip
    readonly property bool stripShown: !phoneForm && (pages.length > 1 || editing)
    readonly property bool stripMeetsPanel: !phoneForm && stripShown && panel === "" && !drive.driving
    // The space above the panel, or above an edit panel over it
    readonly property real gridTop: stripMeetsPanel ? 0 : topGap
    // The panel's distance from the page's sides; none in the drive view
    readonly property real panelMargin: drive.driving ? 0 : margin
    // The edit panel's side column: on the desktop form the open panel stands
    // right of the grid, this wide, in a column that scrolls by itself, while
    // the cells it leaves are at least this wide (the cells narrow, the
    // columns' count stays); with less room the panel stays above the grid
    readonly property real sideColumnWidth: 340
    property real sideCellMinWidth: 140
    // One pixel of the display in the layout's units, and a hairline
    // that many whole pixels wide, so every line draws the same width
    readonly property real pixel: 1 / ((Screen.devicePixelRatio || 1) * Theme.zoom)
    readonly property real hairlineWidth: Math.max(1, Math.round(1 / pixel)) * pixel
    function snap(v) { return Math.round(v / pixel) * pixel }

    // The platform's form, never the window's width (section 4). A desktop's
    // edit mode may show the phone's: phoneEdit
    property string form: Theme.mobile || phoneEdit ? "phone" : "desktop"
    // The desktop edits the phone form's document in a frame of the phone's
    // width (the grid holds its six columns at any phone width); only while
    // editing, and not stored: Done is back to the desktop's
    property bool phoneEdit: false
    readonly property real phoneFrameWidth: 384
    // Never on a phone; where the frame fits the page (the switch stays
    // while the page is the frame itself)
    readonly property bool phoneSwitchShown: !Theme.mobile && editing
                                             && (phoneEdit || width >= phoneFrameWidth + 48)

    SignalHub { id: hub }
    // The hub, for the page's parts
    readonly property SignalHub signalHub: hub
    // The page's own objects, for its parts
    readonly property var layoutStore: store
    readonly property DashHistory historyPart: history
    readonly property DashLayoutFile layoutFilePart: layoutFile
    readonly property DashAddFlow addFlowPart: addFlow
    readonly property DashEdits editsPart: edits
    readonly property DashDrive drivePart: drive
    readonly property Item topBarItem: topBar
    readonly property Item driveStatusItem: driveStatus
    readonly property Item indicatorItem: indicator

    // One document per form, and the page last shown on each
    Settings {
        id: store
        objectName: "dashStore"
        category: "dashboards"
        property string phone: ""
        property string desktop: ""
        property int phonePage: 0
        property int desktopPage: 0
        // A stored text that couldn't be read, kept by the first edit
        property string phoneUnread: ""
        property string desktopUnread: ""
        // The drive view: left on when the last one ended (section 7), and
        // how many times its hint has shown
        property bool phoneDrive: false
        property int driveHints: 0
    }
    // The vehicles' document (Main binds Settings'): the files a layout
    // file names are the picked vehicle's picks'
    property var vehicles: VehicleBook.empty()
    // Settings' URI and profile, whose signals Add offers with no session
    property string offlineUri: ""
    onOfflineUriChanged: if (panel === "signal" || panel === "cell") addFlow.refreshRows()
    // Settings' picked vehicle's sets ([{ profile, name, variant }], its
    // plugin first) and the names of the profiles it knows (profile: name)
    property var pickedSets: []
    property var profileNames: ({})

    // The profile a session on uri reads: its last profile=, else the
    // scheme's default as core picks it (core.cpp, ecuio_session_create)
    function profileOf(uri) {
        const at = uri.indexOf("?")
        let profile = ""
        for (const pair of (at < 0 ? [] : uri.slice(at + 1).split("&"))) {
            if (pair.startsWith("profile="))
                profile = pair.slice("profile=".length)
        }
        if (profile !== "")
            return profile
        const scheme = uri.slice(0, Math.max(0, uri.indexOf("://")))
        return scheme === "mock" ? "mock"
             : scheme === "replay" || scheme === "socketcan" ? "raw" : "obd2"
    }
    function profileName(profile) {
        return profileNames[profile] ?? profile
    }
    // The session's profile, "" with none
    readonly property string sessionProfile: Bridge.connected ? profileOf(Bridge.currentUri) : ""
    // While a session runs, each ref a picked set the session isn't
    // reading has, to that set's name (the first set's that has it); its
    // variant alone when the session reads another variant of that plugin
    readonly property var neededSets: {
        const needed = {}
        if (sessionProfile === "")
            return needed
        const reading = profileName(sessionProfile)
        for (const s of pickedSets) {
            if (s.profile === sessionProfile)
                continue
            const name = s.name === reading && s.variant
                         ? qsTr("variant %1").arg(s.variant) : s.name
            for (const row of Bridge.profileSignals("mock://?profile=" + s.profile)) {
                if (row.ref !== "" && needed[row.ref] === undefined)
                    needed[row.ref] = name
            }
        }
        return needed
    }
    // A cell's note without its signal: the picked set that has it, else
    // the profile the session reads
    function missingNote(refs) {
        for (const ref of refs) {
            if (neededSets[ref] !== undefined)
                return qsTr("needs %1").arg(neededSets[ref])
        }
        return qsTr("not in %1").arg(profileName(sessionProfile))
    }

    // The stored text read: the document, or the built-in layout with
    // the reason when there is none or it can't be used
    readonly property var parsed: DashboardBook.parse(storedText, form)
    property var doc: parsed.doc
    readonly property string problem: parsed.problem
    // The built-in layout is what's shown: nothing stored, or what is stored
    // can't be read. A saved layout is drawn as it was saved
    readonly property bool builtInShown: doc === parsed.doc && problem !== ""
                                         && problem !== "dropped"
    // What a "dropped" read left out, in words
    readonly property string droppedWords: droppedText(parsed.dropped)
    function droppedText(d) {
        if (!d)
            return ""
        const words = []
        if (d.pages > 0)
            words.push(d.pages === 1 ? qsTr("1 page") : qsTr("%1 pages").arg(d.pages))
        if (d.cells > 0)
            words.push(d.cells === 1 ? qsTr("1 gauge") : qsTr("%1 gauges").arg(d.cells))
        return words.join(qsTr(" and "))
    }
    readonly property var pages: doc && Array.isArray(doc.pages) ? doc.pages : []
    // Every page's cells' refs (lookupRefs): Main hands them to
    // Bridge.pickGauges, so the default pick asks what the gauges show
    readonly property var pickRefs: {
        const all = []
        for (const p of pages) {
            for (const c of (Array.isArray(p.cells) ? p.cells : []))
                all.push(lookupRefs(c && Array.isArray(c.refs) ? c.refs : []))
        }
        return all
    }
    property bool problemDismissed: false
    // A different stored text is a different problem, so its line shows again
    readonly property string storedText: form === "phone" ? store.phone : store.desktop
    onStoredTextChanged: problemDismissed = false

    // Edit mode (section 5). Each edit is stored as it's made, so
    // Done leaves edit mode and ends the undo history, and nothing waits
    // to be saved

    property bool editing: false
    property int selected: -1        // the cell edited, on the page shown
    property int selectedDigits: 4   // the selected Digits card's count
    property int selectedCount: 15   // the selected Shift lights card's lights
    property string panel: ""        // "", "cell", "add", "signal"
    property string addKind: ""      // Add's kind; "" when changing a cell's signal
    property bool changingKind: false  // the kind list picks the selected cell's kind
    property bool moreShown: false
    // A page was just added: the Page row's name field takes the focus
    signal pageNameWanted()
    property bool confirmReset: false
    property bool confirmRemovePage: false
    // The preset list's purpose: "" shut, "add" (a new page after the shown
    // one) or "replace" (the shown page's gauges); confirmReplaceKey is the
    // preset a replace waits to be sure of
    property string presetList: ""
    property string confirmReplaceKey: ""
    // Whether the layout file the open dialog picks adds its pages (the
    // preset list's From a file) or replaces every page (Load layout)
    property bool fileAdds: false

    // The gesture (section 4): hold, then release or drag

    property int held: -1            // the cell lifted on the page shown, -1 for none
    property int dropAt: -1          // the place a drag would drop it; -1 until it moves
    // A copied cell and the form it came from (section 5), for the app's run
    property var copied: null        // { form, cell }
    readonly property bool canPaste: copied !== null && copied.form === form
                                     && DashboardEdit.canAddCell(doc, pageIndex)

    DashHistory {
        id: history
        host: root
    }
    onFormChanged: history.clearHistory()

    // A cell's name in a step's words: its label, or its kind's name
    function cellName(c) {
        return c && c.label ? c.label : c ? (kindNames[c.kind] ?? c.kind) : ""
    }

    // The kinds this page draws, in the order Add offers them, and the
    // component that makes each (a cell's and Add's preview); null for
    // a kind this version doesn't draw
    readonly property var drawnKinds: ["analog", "ring", "bar", "segments", "digital",
                                       "seven-segment", "sparkline", "shift-light", "blank"]
    function kindComponent(kind) {
        return kind === "analog" ? analogKind
             : kind === "ring" ? ringKind
             : kind === "bar" ? barKind
             : kind === "digital" ? digitalKind
             : kind === "shift-light" ? shiftKind
             : kind === "segments" ? segmentsKind
             : kind === "seven-segment" ? readoutKind
             : kind === "sparkline" ? sparkKind
             : kind === "blank" ? blankKind
             : null
    }
    // The kinds that draw a peak, so the panel offers Peak hold only there
    readonly property var peakKinds: ["analog", "ring", "bar", "segments"]
    readonly property var kindNames: ({
        "analog": qsTr("Dial"), "ring": qsTr("Ring"), "bar": qsTr("Bar"),
        "digital": qsTr("Number"), "shift-light": qsTr("Shift lights"),
        "segments": qsTr("Cells"), "seven-segment": qsTr("Digits"), "sparkline": qsTr("Sparkline"),
        "blank": qsTr("Blank")
    })
    // A form's spans: the phone's grid has three columns for the desktop's one
    readonly property bool phoneForm: form === "phone"
    readonly property int spanScale: phoneForm ? DashboardBook.phoneSpanScale : 1
    // A new gauge's span: columns, and rows of a quarter column
    readonly property var newSpans: {
        const spans = {
            "analog": [1, 4], "ring": [1, 4], "bar": [2, 2], "digital": [1, 4],
            "shift-light": [12, 1], "segments": [2, 3], "seven-segment": [1, 2], "sparkline": [2, 3],
            "blank": [1, 4]
        }
        for (const k in spans)
            spans[k] = [Math.min(DashboardBook.maxColumns, spans[k][0] * spanScale),
                        spans[k][1] * spanScale]
        return spans
    }

    readonly property string formKey: form === "phone" ? "phone" : "desktop"
    readonly property var shownCells: pages[pageIndex] && Array.isArray(pages[pageIndex].cells)
                                      ? pages[pageIndex].cells : []
    readonly property var selectedCell: selected >= 0 && selected < shownCells.length
                                        ? shownCells[selected] : null
    onSelectedCellChanged: if (selectedCell === null && panel === "cell") panel = ""

    function startEditing() {
        editing = true
    }
    // The desktop's edit mode in the phone form's frame, or back. The history,
    // the selection, the panel and any question belong to the form they were
    // made in, and the page last shown is the new form's own
    function setPhoneEdit(on) {
        if (on === phoneEdit || (on && !phoneSwitchShown))
            return
        commitTyping()
        cancelHold()
        restored = false
        phoneEdit = on
        history.showStoredLayout()
        selected = -1
        panel = ""
        addKind = ""
        confirmReset = false
        confirmRemovePage = false
        presetList = ""
        confirmReplaceKey = ""
        layoutFile.noteLayout("")
        history.clearHistory()
        goTo(form === "phone" ? store.phonePage : store.desktopPage)
        restored = true
    }
    function stopEditing() {
        commitTyping()
        cancelHold()
        setPhoneEdit(false)
        editing = false
        selected = -1
        panel = ""
        moreShown = false
        confirmReset = false
        confirmRemovePage = false
        presetList = ""
        confirmReplaceKey = ""
        layoutFile.noteLayout("")
        history.clearHistory()
    }

    // Takes the focus off a text field, so the field commits what was
    // typed to the cell or page it was typed for before the selection or
    // the page changes (a tap on a cell and a swipe take no focus). Its
    // fields exist only while a panel is open, and otherwise the focus is
    // left where it is. The field's own focus goes: the page is a focus
    // scope, so focusing the page would give it back to the field
    function commitTyping() {
        if (!editing && panel === "")
            return
        const f = Window.activeFocusItem
        if (f !== null && (f instanceof TextInput || f instanceof TextEdit))
            f.focus = false
        // qmllint disable missing-property
        Qt.inputMethod.hide()
        // qmllint enable missing-property
    }

    // Stores next as this form's document, unless it's the same (a
    // refused write gives an equal copy), and makes `step` an undo step
    // (none: a write that can't be undone). The first edit after a stored
    // text that couldn't be used keeps that text in <form>Unread
    function edit(next, step) {
        const now = DashboardBook.serialize(doc)
        const after = DashboardBook.serialize(next)
        if (after === now)
            return false
        if (step)
            history.record(store[formKey] === "" || problem !== "" ? "" : now, after, step)
        const texts = DashboardBook.saveTexts(problem, store[formKey], next)
        if (texts.unread !== "")
            store[formKey + "Unread"] = texts.unread
        store[formKey] = texts.text
        doc = next
        return true
    }

    DashLayoutFile {
        id: layoutFile
        host: root
    }

    function select(i) {
        commitTyping()
        selected = i
        // The cell's panel takes its decimals from the listed rows with no session
        if (!Bridge.connected && (addFlow.profileRows.length === 0 || addFlow.rowsFor !== offlineUri))
            addFlow.refreshRows()
        panel = "cell"
    }

    // Ends a hold with no drop: the page, its document or its grid changed
    // under the finger
    function cancelHold() {
        held = -1
        dropAt = -1
    }
    onDocChanged: cancelHold()

    // Outside edit mode nothing stays selected once the menu is gone
    function closePanel() {
        commitTyping()
        const shown = selected
        panel = ""
        if (!editing)
            selected = -1
        else if (shown >= 0)
            revealRequested(shown)
    }
    // The shown page scrolls to a cell, so what a panel edited is seen again
    signal revealRequested(int cell)

    // A gauge's Alert (ADR-0063 section 9): a new condition from the cell's
    // own levels, refs and unit, which Main opens in Settings' editor as a
    // copy bound to nothing; null for a cell with no level
    signal alertRequested(var condition)
    function alertOf(cell, name) {
        if (cell === null || cell.kind === "blank" || !Array.isArray(cell.refs) || cell.refs.length === 0)
            return null
        const o = cell.options ?? ({})
        // The range the cell draws: its own ends, else its unit's default
        const range = DashboardEdit.defaultRange(cell.unit ?? "")
        return ConditionBook.alertFromLevels(name, cell.refs, cell.unit ?? "", o,
                                             typeof o.from === "number" ? o.from : range.from,
                                             typeof o.to === "number" ? o.to : range.to, [])
    }
    // Why a gauge's Alert didn't open the editor, under the button until
    // another cell is picked
    property string alertNote: ""
    onSelectedChanged: alertNote = ""

    DashDrive {
        id: drive
        host: root
    }
    property alias driveHideMs: drive.driveHideMs
    property alias driveInsetBottom: drive.driveInsetBottom
    property alias driveInsetTop: drive.driveInsetTop
    property alias driveLogSize: drive.driveLogSize
    property alias driveLogTime: drive.driveLogTime
    property alias driveOrientation: drive.driveOrientation
    property alias driveTabBarHeight: drive.driveTabBarHeight
    // Copy and paste (section 5)

    function copySelected() {
        const c = DashboardEdit.copyCell(doc, pageIndex, selected)
        if (c !== null)
            copied = { form: form, cell: c }
    }
    function pasteAfter(i) {
        if (!canPaste)
            return
        commitTyping()
        edit(DashboardEdit.pasteCell(doc, pageIndex, i, copied.cell),
             history.stepOf("", qsTr("pasted %1").arg(cellName(copied.cell)), -1, i + 1))
    }

    DashAddFlow {
        id: addFlow
        host: root
    }
    // Bridge.alertLevels copied once per change, by def id: a bridge map read
    // in each cell's binding makes a JS wrapper per read (the Qt 6.12 sweep)
    property var alertLevels: {
        const levels = Bridge.alertLevels, copy = {}
        for (const id in levels)
            copy[id] = levels[id]
        return copy
    }
    // Bridge.gaugeSignals as plain arrays, read once (it is CONSTANT): each read
    // of a bridge map makes a new JS wrapper, and every cell's lookup reads it
    readonly property var gaugeSets: DashboardEdit.copySets(Bridge.gaugeSignals)
    function counterparts(ref) {
        return DashboardEdit.counterparts(ref, gaugeSets)
    }
    function lookupRefs(refs) {
        return DashboardEdit.lookupRefs(refs, gaugeSets)
    }
    function labelFor(row) {
        return Array.from(String(row.name)).slice(0, DashboardBook.maxLabel).join("")
    }
    function keyOfRef(ref) {
        return DashboardEdit.keyOfRef(ref, gaugeSets)
    }
    DashEdits {
        id: edits
        host: root
    }
    // Pages

    property int pageIndex: 0
    property bool restored: false
    function goTo(i) {
        const n = Math.max(1, pages.length)
        const to = Math.max(0, Math.min(n - 1, Math.round(i)))
        if (to !== pageIndex)
            commitTyping()
        pageIndex = to
    }
    onPagesChanged: goTo(pageIndex)
    // A page change closes whatever panel was open
    onPageIndexChanged: {
        edits.closePresets()
        cancelHold()
        selected = -1
        panel = ""
        addKind = ""
        if (swipe.count > pageIndex && swipe.currentIndex !== pageIndex)
            swipe.setCurrentIndex(pageIndex)
        // A tab with the focus hands it on, so the ring is on the page shown
        const strip = topBar.stripLoader.item as DashPageStrip
        if (strip && strip.holdsFocus())
            strip.focusTab(pageIndex)
        if (!restored) return
        if (form === "phone") store.phonePage = pageIndex
        else store.desktopPage = pageIndex
    }

    // Left and Right on the desktop form, never while a text field has
    // the focus (an editable one keeps them; a read-only one would pass
    // them on, though they move its cursor)
    readonly property bool typing: {
        const f = Window.activeFocusItem
        return f !== null && (f instanceof TextInput || f instanceof TextEdit)
    }
    readonly property bool pageKeys: visible && form === "desktop" && pages.length > 1 && !typing
    Shortcut {
        sequence: "Left"
        enabled: root.pageKeys
        onActivated: root.goTo(root.pageIndex - 1)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.pageKeys
        onActivated: root.goTo(root.pageIndex + 1)
    }
    // Undo and Redo on the desktop form, in edit mode or out of it; a
    // field with the focus keeps its own
    readonly property bool undoKeys: visible && form === "desktop" && !typing && !drive.driving
    Shortcut {
        id: undoKey
        sequence: "Ctrl+Z"
        enabled: root.undoKeys && history.canUndo
        onActivated: history.undo()
    }
    Shortcut {
        id: redoKey
        sequence: "Ctrl+Shift+Z"
        enabled: root.undoKeys && history.canRedo
        onActivated: history.redo()
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: root.undoKeys && history.canRedo
        onActivated: history.redo()
    }

    // The sweep and the peaks, on every page

    signal sweepRequested()
    signal peaksResetRequested()

    // Ignition-style sweep, once per session, the first time the page
    // is on screen while connected
    property bool sweptThisSession: false
    // When the sweep was last requested (ms since the epoch) and how long
    // it plays. A gauge that finishes loading within that time of it
    // plays its own, so a page loaded between frames sweeps too.
    property double sweptAt: 0
    property int sweepMs: 1550
    function sweepIsOn() { return Date.now() - sweptAt < sweepMs }
    function maybeSweep() {
        if (!Bridge.connected) {
            sweptThisSession = false
            sweptAt = 0
            return
        }
        if (!visible || sweptThisSession) return
        sweptThisSession = true
        sweptAt = Date.now()
        sweepRequested()
    }
    function resetPeaks() { peaksResetRequested() }
    onVisibleChanged: {
        maybeSweep()
        // Any page but Dash ends the view for good: a tab, the status's Log,
        // and the history outside edit mode, as Done does: a step from
        // outside edit mode isn't undone by a Ctrl+Z days later. Edit mode
        // stays open while hidden, so its history stays with it
        if (!visible) {
            drive.leaveDrive(true)
            if (!editing)
                history.clearHistory()
        }
    }
    Component.onCompleted: {
        goTo(form === "phone" ? store.phonePage : store.desktopPage)
        restored = true
        maybeSweep()
    }
    Connections {
        target: Bridge
        function onConnectedChanged() {
            if (Bridge.connected) root.resetPeaks()
            root.maybeSweep()
            // The view comes back with a session only with Dash showing
            if (!Bridge.connected)
                drive.leaveDrive(false)
            else if (store.phoneDrive && root.visible && !drive.isReplay(Bridge.currentUri))
                drive.enterDrive()
        }
        // A peak held in the old units would read wrong in the new ones
        function onUnitSystemChanged() { root.resetPeaks() }
    }

    // Units

    // Scale ends and zones are written in the unit the signal decodes
    // to and shown in the current unit system through core (ADR-0022).
    // Reading Bridge.unitSystem here makes each binding follow a change.
    function shown(unit, v) {
        return Bridge.unitSystem === 0 ? v : Bridge.displayValue(unit, v)
    }
    function shownUnit(unit) {
        return Bridge.unitSystem === 0 ? unit : Bridge.displayUnit(unit)
    }
    // A number typed in the unit shown, in the unit stored
    function stored(unit, v) {
        return Bridge.unitSystem === 0 ? v : DashboardEdit.roundStored(Bridge.decodedValue(unit, v))
    }
    // A number for a field: at most four decimals, none trailing
    function fieldText(v, places) {
        return typeof v === "number" && isFinite(v) ? String(Number(v.toFixed(places ?? 4))) : ""
    }
    // A round tick spacing near `step`: 1, 2 or 5 times a power of ten
    function niceStep(step) {
        if (!(step > 0) || !isFinite(step)) return 1
        const p = Math.pow(10, Math.floor(Math.log(step) / Math.LN10))
        const m = step / p
        return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p
    }

    // Edit mode's panels

    DashPages {
        id: swipe
        host: root
    }
    // No session on the phone: the connect list, in the panel's place above
    // the gauges, on every page. Settings owns connecting; ConnectList the
    // chosen row and the last failure's words
    property var connectUris: []
    property string connectVehicleUri: ""
    property var connectAdapters: []
    property var connectProfileWords: ({})
    // A tapped row outlives the list, which the Loader makes again each show
    property string connectTapped: ""
    readonly property bool connectShown: form === "phone" && !Bridge.connected && !editing
    signal connectToUri(string uri)
    signal otherAddressRequested()
    signal logRequested()
    signal connectListShown()
    onConnectUrisChanged: connectTapped = ""
    onConnectVehicleUriChanged: connectTapped = ""
    Component { id: analogKind; AnalogGauge {} }
    Component { id: ringKind; RingGauge {} }
    Component { id: barKind; BarGauge {} }
    Component { id: digitalKind; DigitalGauge {} }
    Component { id: shiftKind; ShiftLightBar {} }
    Component { id: segmentsKind; SegmentGauge {} }
    Component { id: readoutKind; SegmentReadout {} }
    Component { id: sparkKind; SparkGauge {} }
    // A spot held open: an outline to find it by in edit mode, else nothing
    Component {
        id: blankKind
        GaugeBase {
            // Nothing to announce where nothing is drawn
            Accessible.ignored: !root.editing
            Accessible.name: root.kindNames["blank"]
            Accessible.description: ""
            Rectangle {
                objectName: "blankOutline"
                anchors.fill: parent
                visible: root.editing
                color: "transparent"
                border.color: Theme.outline
                border.width: 1
                radius: Theme.radius
            }
        }
    }

    Component { id: toolNight; NightSwitch {} }
    // The Night switch, for the page's parts
    readonly property Component nightSwitch: toolNight

    // The drive view's one line of chrome: the link state and the address,
    // and a log's or recording's progress (section 2). It takes no taps
    DashDriveStatus {
        id: driveStatus
        host: root
    }

    // The alert band in the drive view, where Main's is hidden: under
    // the status line, over the gauges' top, moving nothing
    AlertBand {
        id: driveAlertBand
        objectName: "driveAlertBand"
        visible: drive.driving && shown
        z: 2
        x: Theme.barGap
        y: driveStatus.y + driveStatus.height
        width: root.width - 2 * Theme.barGap
    }

    // The drive view's controls (ADR-0059's appendix "the reveal from
    // the bottom"): a sheet that rises with the tab bar, over the panel's
    // foot, so the top row and the status line stay in view. Upright the
    // session's row over the view's, Exit drive nearest the tabs; on its
    // side one row, the view's controls first. Its ground keeps the gauges
    // from showing through, and a touch anywhere on it restarts the delay
    DashDriveSheet {
        id: driveSheet
        host: root
    }

    // The header: on the desktop the title, the link state and the
    // tools, on the phone the tools alone (the tab bar names the page and
    // the top bar has the link state); why the stored layout isn't shown;
    // last, the desktop's page strip, on the panel's upper edge. The drive
    // view shows none of it: its controls are in the sheet

    DashTopBar {
        id: topBar
        host: root
    }

    // After an undo or a redo, the step's words; outside edit mode, after each
    // step too, with an Undo, since there is no bar there. It floats over the
    // pages' foot, so its coming and going moves nothing
    Rectangle {
        id: undoOverlay
        z: 2
        x: (root.width - width) / 2
        anchors.bottom: swipe.bottom
        width: Math.max(0, root.width - 2 * root.margin)
        height: Theme.touchTarget + 8
        visible: history.undoLine !== "" && !drive.driving
        color: Theme.surface
        border.width: 1
        border.color: Theme.hairline
        radius: Theme.radiusCard
        RowLayout {
            objectName: "dashUndoLine"
            readonly property real touchMin: Theme.touchTarget
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                      leftMargin: 8; rightMargin: 8 }
            spacing: 10
            Label {
                objectName: "dashUndoLineText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                text: history.undoLine
            }
            Label {
                objectName: "dashUndoLineKeys"
                visible: root.editing && root.form === "desktop"
                color: Theme.textDim
                font.pixelSize: Theme.fontCaption
                text: undoKey.nativeText + "  " + redoKey.nativeText
            }
            ThemedButton {
                objectName: "dashUndoLineUndo"
                compact: true
                visible: !root.editing
                enabled: history.canUndo
                text: qsTr("Undo")
                Accessible.name: history.canUndo ? qsTr("Undo: %1").arg(history.undoWords) : qsTr("Undo")
                onClicked: history.undo()
            }
        }
    }

    // The pages: one SwipeView, swiped on the phone form, moved by
    // the strip's tabs and the keys on the desktop form

    PageIndicator {
        id: indicator
        objectName: "dashIndicator"
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        visible: root.form === "phone" && root.pages.length > 1 && !drive.driving
        count: swipe.count
        currentIndex: swipe.currentIndex
    }

    // The first entries' hint: over the gauges, and it takes no touch
    Rectangle {
        objectName: "driveHint"
        visible: drive.driving && drive.driveHintUp
        z: 1
        anchors.centerIn: swipe
        width: driveHintText.implicitWidth + 24
        height: driveHintText.implicitHeight + 12
        radius: Theme.radius
        color: Theme.bg
        Label {
            id: driveHintText
            anchors.centerIn: parent
            text: qsTr("Tap for the controls")
            color: Theme.textDim
        }
    }
}
