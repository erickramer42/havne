// File/layer: app/qml/DashboardPage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit, QmlTest.phone,
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
    readonly property real topGap: driving ? 0 : phoneForm ? 12 : margin
    // The desktop's page strip shows, and its shown tab meets the panel:
    // not with an edit panel open, which sits a gap under the strip
    readonly property bool stripShown: stripHost.visible
    readonly property bool stripMeetsPanel: !phoneForm && stripShown && panel === "" && !driving
    // The space above the panel, or above an edit panel over it
    readonly property real gridTop: stripMeetsPanel ? 0 : topGap
    // The panel's distance from the page's sides; none in the drive view
    readonly property real panelMargin: driving ? 0 : margin
    // One pixel of the display in the layout's units, and a hairline
    // that many whole pixels wide, so every line draws the same width
    readonly property real pixel: 1 / ((Screen.devicePixelRatio || 1) * Theme.zoom)
    readonly property real hairlineWidth: Math.max(1, Math.round(1 / pixel)) * pixel
    function snap(v) { return Math.round(v / pixel) * pixel }

    // The platform's form, never the window's width (section 4)
    property string form: Theme.mobile ? "phone" : "desktop"

    SignalHub { id: hub }

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
    // Settings' URI and profile, whose signals Add offers with no session
    property string offlineUri: ""
    onOfflineUriChanged: if (panel === "signal") refreshRows()

    // The stored text read: the document, or the built-in layout with
    // the reason when there is none or it can't be used
    readonly property var parsed: DashboardBook.parse(storedText, form)
    property var doc: parsed.doc
    readonly property string problem: parsed.problem
    // What a "dropped" read left out, in words
    readonly property string droppedWords: {
        const d = parsed.dropped
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
    property bool problemDismissed: false
    // A different stored text is a different problem, so its line shows again
    readonly property string storedText: form === "phone" ? store.phone : store.desktop
    onStoredTextChanged: problemDismissed = false

    // ---- Edit mode (section 5). Each edit is stored as it's made, so
    // Done leaves edit mode and ends the undo history, and nothing waits
    // to be saved

    property bool editing: false
    property int selected: -1        // the cell edited, on the page shown
    property int selectedDigits: 4   // the selected Digits card's count
    property int selectedCount: 15   // the selected Shift lights card's lights
    property string panel: ""        // "", "cell", "add", "signal"
    property string addKind: ""      // Add's kind; "" when changing a cell's signal
    property bool moreShown: false
    property bool confirmReset: false
    property bool confirmRemovePage: false

    // ---- The gesture (section 4): hold, then release or drag

    property int held: -1            // the cell lifted on the page shown, -1 for none
    property int dropAt: -1          // the place a drag would drop it; -1 until it moves
    // A copied cell and the form it came from (section 5), for the app's run
    property var copied: null        // { form, cell }
    readonly property bool canPaste: copied !== null && copied.form === form
                                     && DashboardBook.canAddCell(doc, pageIndex)

    // ---- Undo (ADR-0057's appendix "undo in edit mode as built"). An
    // entry keeps the text stored before its step, so an undo stores what
    // was there: "" (nothing, the built-in layout) included

    property var undoStack: []       // [{ text, step }], the newest last
    property var redoStack: []
    property string lastKey: ""      // the last step's grouping key
    property double lastAt: 0        // when it was made (Date.now())
    readonly property int undoLimit: 50
    readonly property int groupMs: 1500
    property int undoLineMs: 5000    // the line's time; a test shortens it
    property string undoLine: ""     // the line's words; "" hides it
    readonly property bool canUndo: undoStack.length > 0
    readonly property bool canRedo: redoStack.length > 0
    readonly property string undoWords: canUndo ? undoStack[undoStack.length - 1].step.words : ""
    readonly property string redoWords: canRedo ? redoStack[redoStack.length - 1].step.words : ""
    // The options whose control steps or toggles: a run of them is one step
    readonly property var steppedOptions: ["decimals", "peak", "cells", "digits", "window",
                                           "count", "marks", "vertical"]
    // An option's word in a step's words, as its caption on the panel says it
    readonly property var optionWords: ({
        "from": qsTr("range"), "to": qsTr("range"), "coldBelow": qsTr("zones"),
        "warnFrom": qsTr("zones"), "dangerFrom": qsTr("zones"), "decimals": qsTr("decimals"),
        "peak": qsTr("peak hold"), "majorStep": qsTr("major step"),
        "minorStep": qsTr("major step"), "labelDivisor": qsTr("major step"),
        "caption": qsTr("caption"), "description": qsTr("description"), "cells": qsTr("cells"),
        "rise": qsTr("curve"), "digits": qsTr("digits"), "window": qsTr("window"),
        "vertical": qsTr("standing up"), "marks": qsTr("scale marks"), "count": qsTr("lights")
    })
    onFormChanged: clearHistory()

    // A step on the page shown: its grouping key ("" never groups), its
    // words, and its cell before and after (-1 for none)
    function stepOf(key, words, cell, cellAfter, pageAfter) {
        return { key: key, words: words, page: pageIndex,
                 pageAfter: pageAfter ?? pageIndex, cell: cell, cellAfter: cellAfter }
    }
    // A cell's name in a step's words: its label, or its kind's name
    function cellName(c) {
        return c && c.label ? c.label : c ? (kindNames[c.kind] ?? c.kind) : ""
    }
    // The text the store holds for what's shown
    function shownText() {
        return store[formKey] === "" ? "" : DashboardBook.serialize(doc)
    }
    function record(before, after, step) {
        const now = Date.now()
        const n = undoStack.length
        if (n > 0 && step.key !== "" && step.key === lastKey && now - lastAt <= groupMs) {
            // A run: the top already holds the text from before it
            const top = undoStack[n - 1]
            const rest = undoStack.slice(0, n - 1)
            if (after === top.text) {
                // Back where the run began, so an Undo would change nothing
                undoStack = rest
                lastKey = ""
            } else {
                undoStack = rest.concat([{ text: top.text, step: Object.assign({}, top.step,
                    { pageAfter: step.pageAfter, cellAfter: step.cellAfter }) }])
                lastKey = step.key
            }
        } else {
            undoStack = undoStack.concat([{ text: before, step: step }]).slice(-undoLimit)
            lastKey = step.key
        }
        lastAt = now
        redoStack = []
        // Edit mode's bar has Undo; outside it the line is the only way back
        if (editing)
            undoLine = ""
        else
            showLine(step.words.charAt(0).toUpperCase() + step.words.slice(1))
    }
    function restore(text) {
        store[formKey] = text
        doc = text === "" ? DashboardBook.reset(form) : DashboardBook.parse(text, form).doc
    }
    // An undo or a redo goes from (page, cell) to (toPage, toCell). The
    // cell panel follows only when it was on the step's cell, so it never
    // shows another cell at a moved index; Add's and a signal's close
    function travel(text, page, cell, toPage, toCell) {
        const shown = panel
        const follows = selected >= 0 && selected === cell && page === pageIndex
                        && (shown === "cell" || shown === "")
        restore(text)
        goTo(toPage)
        if (follows && toCell >= 0 && toCell < shownCells.length) {
            selected = toCell
            panel = shown
            return
        }
        panel = ""
        addKind = ""
        selected = -1
    }
    function undo() {
        // A field still open commits as its own step, which this then takes back
        commitTyping()
        if (!canUndo)
            return
        const top = undoStack[undoStack.length - 1]
        const s = top.step
        undoStack = undoStack.slice(0, -1)
        redoStack = redoStack.concat([{ text: shownText(), step: s }])
        travel(top.text, s.pageAfter, s.cellAfter, s.page, s.cell)
        lastKey = ""
        showLine(qsTr("Undid: %1").arg(s.words))
    }
    function redo() {
        commitTyping()
        if (!canRedo)
            return
        const top = redoStack[redoStack.length - 1]
        const s = top.step
        redoStack = redoStack.slice(0, -1)
        undoStack = undoStack.concat([{ text: shownText(), step: s }])
        travel(top.text, s.page, s.cell, s.pageAfter, s.cellAfter)
        lastKey = ""
        showLine(qsTr("Redid: %1").arg(s.words))
    }
    function clearHistory() {
        undoStack = []
        redoStack = []
        lastKey = ""
        undoLine = ""
    }
    function showLine(words) {
        undoLine = words
        undoLineTimer.restart()
    }
    Timer {
        id: undoLineTimer
        interval: root.undoLineMs
        onTriggered: root.undoLine = ""
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
    function stopEditing() {
        commitTyping()
        cancelHold()
        editing = false
        selected = -1
        panel = ""
        moreShown = false
        confirmReset = false
        confirmRemovePage = false
        clearHistory()
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
            record(store[formKey] === "" ? "" : now, after, step)
        const texts = DashboardBook.saveTexts(problem, store[formKey], next)
        if (texts.unread !== "")
            store[formKey + "Unread"] = texts.unread
        store[formKey] = texts.text
        doc = next
        return true
    }

    // Back to the built-in layout: nothing stored, so a later version's
    // built-in layout reaches it. Undo stores the replaced pages again
    function resetForm() {
        if (store[formKey] !== "")
            record(shownText(), "", stepOf("", qsTr("reset the layout"), -1, -1, 0))
        const texts = DashboardBook.saveTexts(problem, store[formKey], doc)
        if (texts.unread !== "")
            store[formKey + "Unread"] = texts.unread
        store[formKey] = ""
        doc = DashboardBook.reset(form)
        confirmReset = false
        moreShown = false
        selected = -1
        panel = ""
        goTo(0)
    }

    function select(i) {
        commitTyping()
        selected = i
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
        panel = ""
        if (!editing)
            selected = -1
    }

    // ---- The drive view (ADR-0059): this page with every bar hidden

    property bool driving: false
    property bool barsUp: false          // the reveal
    property bool barsHeld: false        // a fault or a log's notice keeps them up
    // What Main hands over: Settings' two values, the header's height and
    // the system's insets
    property int driveHideMs: 4000
    property string driveOrientation: "follow"
    property real driveHeaderHeight: 0
    property real driveInsetTop: 0
    property real driveInsetBottom: 0
    property string driveLogTime: ""
    property string driveLogSize: ""   // "" until the header shows one
    // The window made full screen; a test's stand-in keeps it off the desktop
    property var driveWindow: Window.window

    // How far the bars are in: 0 hidden, 1 revealed. Main's bars follow it
    property real barsShown: driving && barsUp ? 1 : 0
    Behavior on barsShown { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

    property bool driveHintUp: false
    function isReplay(uri) { return uri.toLowerCase().startsWith("replay://") }

    function enterDrive() {
        if (driving || !phoneForm || !Bridge.connected || editing)
            return
        closePanel()
        cancelHold()
        barsUp = false
        barsHeld = false
        // A fault or a notice already up raises the bars through their handlers
        driving = true
        store.phoneDrive = true
        Immersive.enter(driveWindow, form)
        PhoneWindow.lockOrientation(driveOrientation)
        if (store.driveHints < 3) {
            ++store.driveHints
            driveHintUp = true
        }
    }
    // Exit drive and a tab are for good; a session's end comes back at the next connect
    function leaveDrive(forGood) {
        if (!driving)
            return
        driving = false
        barsUp = false
        barsHeld = false
        driveHintUp = false
        driveLogEnded = false
        if (forGood)
            store.phoneDrive = false
        Immersive.leave()
        PhoneWindow.lockOrientation("follow")
    }
    Component.onDestruction: leaveDrive(false)

    // Above the revealed tool row: a press restarts the hide delay and goes
    // on to the buttons
    MouseArea {
        objectName: "driveToolsTouch"
        enabled: root.driving
        visible: topBar.visible && root.driving
        z: 3
        x: topBar.x
        y: topBar.y
        width: topBar.width
        height: topBar.height
        onPressed: mouse => {
            root.barTouched()
            mouse.accepted = false
        }
    }

    // A tap on the gauges: the bars in or out, and a held reveal is over
    function driveTap() {
        barsUp = !barsUp
        barsHeld = false
        driveHintUp = false
    }
    // Android's Back in the view: the bars first, then out for good
    function driveBack() {
        if (!driving)
            return false
        if (barsUp)
            leaveDrive(true)
        else
            barsUp = true
        return true
    }
    // Android's Back on Dash: the drive view, a gauge's panel, then edit
    // mode; false leaves it to the system
    function goBack() {
        if (driveBack())
            return true
        if (panel !== "") {
            closePanel()
            return true
        }
        if (editing) {
            stopEditing()
            return true
        }
        return false
    }
    function raiseBars(attention) {
        if (!driving)
            return
        if (attention) {
            barsUp = true
            barsHeld = true
        } else if (!driveFault && !driveNotice) {
            barsHeld = false
        }
    }
    // A fault may need Retry or Disconnect; the other states are for the status line
    function raisesBars(state) { return state === "fault" }
    readonly property bool driveFault: driving && raisesBars(Bridge.linkState)
    readonly property bool driveNotice: driving && Bridge.logNotice !== ""
    onDriveFaultChanged: raiseBars(driveFault)
    onDriveNoticeChanged: raiseBars(driveNotice)

    Timer {
        id: driveHideTimer
        objectName: "driveHideTimer"
        interval: root.driveHideMs
        running: root.driving && root.barsUp && !root.barsHeld && root.driveHideMs > 0
        onTriggered: root.barsUp = false
    }
    // A touch on a bar starts its delay again
    function barTouched() {
        ++barTouches
        if (driveHideTimer.running)
            driveHideTimer.restart()
    }
    property int barTouches: 0    // counted for the tests
    Timer {
        objectName: "driveHintTimer"
        interval: root.driveHideMs > 0 ? root.driveHideMs : 4000
        running: root.driveHintUp
        onTriggered: root.driveHintUp = false
    }
    // Stop log stays in the view, so the status line says it ended; the bars
    // stay as they are, since the driver asked for it
    property bool driveLogEnded: false
    Connections {
        target: Bridge
        function onLogStopped(folder, byUser) {
            if (root.driving && byUser)
                root.driveLogEnded = true
        }
    }
    Timer {
        objectName: "driveLogEndedTimer"
        interval: root.driveHideMs > 0 ? root.driveHideMs : 4000
        // The header covers the status line while the bars are up
        running: root.driveLogEnded && !root.barsUp
        onTriggered: root.driveLogEnded = false
    }

    // ---- Copy and paste (section 5)

    function copySelected() {
        const c = DashboardBook.copyCell(doc, pageIndex, selected)
        if (c !== null)
            copied = { form: form, cell: c }
    }
    function pasteAfter(i) {
        if (!canPaste)
            return
        commitTyping()
        edit(DashboardBook.pasteCell(doc, pageIndex, i, copied.cell),
             stepOf("", qsTr("pasted %1").arg(cellName(copied.cell)), -1, i + 1))
    }

    // ---- Add: a kind, then a signal

    property var profileRows: []
    function refreshRows() {
        profileRows = Bridge.profileSignals(Bridge.connected ? Bridge.currentUri : offlineUri)
    }
    // The rows a gauge of `kind` can show: numbers with references, an
    // enumeration only on a digital tile
    function pickable(kind) {
        return profileRows.filter(r => r.ref !== "" && !r.isText && (!r.isEnum || kind === "digital"))
    }
    function startAdd() {
        if (!DashboardBook.canAddCell(doc, pageIndex))
            return
        commitTyping()
        selected = -1
        addKind = ""
        panel = "add"
    }
    function pickKind(kind) {
        if (kind === "blank") {
            const span = newSpans[kind]
            if (edit(DashboardBook.addCell(doc, pageIndex, { kind: kind, w: span[0], h: span[1], refs: [] }),
                     stepOf("", qsTr("added %1").arg(kindNames[kind]), -1, shownCells.length)))
                select(shownCells.length - 1)
            return
        }
        addKind = kind
        refreshRows()
        panel = "signal"
    }
    function changeSignal() {
        addKind = ""
        refreshRows()
        panel = "signal"
    }
    // The picked ref and its counterparts in the other built-in set
    function counterparts(ref) {
        const sets = Bridge.gaugeSignals
        for (const key in sets) {
            const refs = sets[key]
            if (refs.indexOf(ref) >= 0)
                return [ref].concat(refs.filter(r => r !== ref))
        }
        return [ref]
    }
    function labelFor(row) {
        return Array.from(String(row.name)).slice(0, DashboardBook.maxLabel).join("")
    }
    // A new gauge's cell (section 5): the row's name, its unit and range
    // as core decodes them (section 6), no zones; decimals follow the row
    function newCell(kind, row) {
        const span = newSpans[kind] ?? [1, 4]
        const r = DashboardBook.defaultRange(row.decodedUnit, row.decodedLow, row.decodedHigh)
        return { kind: kind, w: span[0], h: span[1], label: labelFor(row),
                 refs: counterparts(row.ref), unit: row.decodedUnit,
                 options: { from: r.from, to: r.to } }
    }
    function pickSignal(row) {
        if (addKind !== "") {
            const cell = newCell(addKind, row)
            if (edit(DashboardBook.addCell(doc, pageIndex, cell),
                     stepOf("", qsTr("added %1").arg(cellName(cell)), -1, shownCells.length))) {
                addKind = ""
                select(shownCells.length - 1)
            }
            return
        }
        if (selected < 0)
            return
        // Another signal: its name, unit and range, and none of the old
        // signal's zones, steps, words or decimals, which were its own
        const r = DashboardBook.defaultRange(row.decodedUnit, row.decodedLow, row.decodedHigh)
        let next = DashboardBook.setCell(doc, pageIndex, selected, {
            refs: counterparts(row.ref), unit: row.decodedUnit, label: labelFor(row) })
        next = DashboardBook.setOptions(next, pageIndex, selected, {
            from: r.from, to: r.to, coldBelow: null, warnFrom: null, dangerFrom: null,
            majorStep: null, minorStep: null, labelDivisor: null, caption: null,
            description: null, peakLabel: null, decimals: null })
        edit(next, stepOf("", qsTr("changed %1's signal").arg(cellName(selectedCell)), selected, selected))
        panel = "cell"
    }
    Connections {
        target: Bridge
        enabled: root.panel === "signal"
        function onConnectedChanged() { root.refreshRows() }
        function onUnitSystemChanged() { root.refreshRows() }
    }

    // ---- The cell's panel

    // The selected cell's options, on `base` when given (a write already
    // composed for the same step). A run of a stepped option on one cell
    // is one undo step; a typed one never groups, since a field commits once
    function setOptions(changes, base) {
        const names = Object.keys(changes).sort()
        const key = names.every(n => steppedOptions.indexOf(n) >= 0)
            ? "opt/" + pageIndex + "/" + selected + "/" + names.join(",") : ""
        const word = optionWords[names[0]] ?? names[0]
        return edit(DashboardBook.setOptions(base ?? doc, pageIndex, selected, changes),
                    stepOf(key, qsTr("changed %1's %2").arg(cellName(selectedCell)).arg(word),
                           selected, selected))
    }
    function setLabel(text) {
        return edit(DashboardBook.setCell(doc, pageIndex, selected,
                                          { label: text === "" ? null : text }),
                    stepOf("", qsTr("changed %1's label").arg(cellName(selectedCell)), selected, selected))
    }
    function resize(w, h) {
        return edit(DashboardBook.resizeCell(doc, pageIndex, selected, w, h),
                    stepOf("size/" + pageIndex + "/" + selected,
                           qsTr("resized %1").arg(cellName(selectedCell)), selected, selected))
    }
    // Earlier, later, to the start and to the end: the cell's place in
    // the order, which both forms' grids follow
    function moveTo(to) {
        const t = Math.max(0, Math.min(shownCells.length - 1, to))
        if (edit(DashboardBook.moveCell(doc, pageIndex, selected, t),
                 stepOf("", qsTr("moved %1").arg(cellName(selectedCell)), selected, t)))
            selected = t
    }
    function removeCell(i) {
        commitTyping()
        if (!edit(DashboardBook.removeCell(doc, pageIndex, i),
                  stepOf("", qsTr("removed %1").arg(cellName(shownCells[i])), i, -1)))
            return
        if (selected === i) {
            selected = -1
            panel = ""
        } else if (selected > i) {
            --selected
        }
    }

    // ---- The pages' edits

    function addPage() {
        if (edit(DashboardBook.addPage(doc, qsTr("Page %1").arg(pages.length + 1)),
                 stepOf("", qsTr("added a page"), -1, -1, pages.length)))
            goTo(pages.length - 1)
    }
    function movePage(by) {
        const to = pageIndex + by
        if (edit(DashboardBook.movePage(doc, pageIndex, to), stepOf("", qsTr("moved the page"), -1, -1, to)))
            goTo(to)
    }
    function renamePage(name) {
        edit(DashboardBook.renamePage(doc, pageIndex, name), stepOf("", qsTr("renamed the page"), -1, -1))
    }
    // Another page takes this one's index, so nothing stays selected
    function removePage() {
        confirmRemovePage = false
        selected = -1
        panel = ""
        addKind = ""
        const p = pages[pageIndex]
        const name = p && p.name ? p.name : qsTr("Page %1").arg(pageIndex + 1)
        edit(DashboardBook.removePage(doc, pageIndex),
             stepOf("", qsTr("removed the page %1").arg(name), -1, -1,
                    Math.max(0, Math.min(pageIndex, pages.length - 2))))
    }

    // ---- Pages

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
        cancelHold()
        selected = -1
        panel = ""
        addKind = ""
        if (swipe.count > pageIndex && swipe.currentIndex !== pageIndex)
            swipe.setCurrentIndex(pageIndex)
        // A tab with the focus hands it on, so the ring is on the page shown
        const strip = stripHost.item as DashPageStrip
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
    readonly property bool undoKeys: visible && form === "desktop" && !typing && !driving
    Shortcut {
        id: undoKey
        sequence: "Ctrl+Z"
        enabled: root.undoKeys && root.canUndo
        onActivated: root.undo()
    }
    Shortcut {
        id: redoKey
        sequence: "Ctrl+Shift+Z"
        enabled: root.undoKeys && root.canRedo
        onActivated: root.redo()
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: root.undoKeys && root.canRedo
        onActivated: root.redo()
    }

    // ---- The sweep and the peaks, on every page

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
        // Any page but Dash ends the view for good: a tab, the status's Log
        if (!visible)
            leaveDrive(true)
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
                root.leaveDrive(false)
            else if (store.phoneDrive && root.visible && !root.isReplay(Bridge.currentUri))
                root.enterDrive()
        }
        // A peak held in the old units would read wrong in the new ones
        function onUnitSystemChanged() { root.resetPeaks() }
    }

    // ---- Units

    // Scale ends and zones are written in the unit the signal decodes
    // to and shown in the current unit system through core (ADR-0022).
    // Reading Bridge.unitSystem here makes each binding follow a change.
    function shown(unit, v) {
        return Bridge.unitSystem === 0 ? v : Bridge.displayValue(unit, v)
    }
    function shownUnit(unit) {
        return Bridge.unitSystem === 0 ? unit : Bridge.displayUnit(unit)
    }
    // A number typed in the unit shown, in the unit stored. Each of
    // core's conversions is linear, so two points give it back; six
    // decimals drop the arithmetic's noise (212 deg F is 100 deg C)
    function stored(unit, v) {
        if (Bridge.unitSystem === 0)
            return v
        const a = Bridge.displayValue(unit, 0), b = Bridge.displayValue(unit, 1)
        const x = b !== a ? (v - a) / (b - a) : v
        return Math.round(x * 1e6) / 1e6
    }
    // A number for a field: at most four decimals, none trailing
    function fieldText(v) {
        return typeof v === "number" && isFinite(v) ? String(Number(v.toFixed(4))) : ""
    }
    // A round tick spacing near `step`: 1, 2 or 5 times a power of ten
    function niceStep(step) {
        if (!(step > 0) || !isFinite(step)) return 1
        const p = Math.pow(10, Math.floor(Math.log(step) / Math.LN10))
        const m = step / p
        return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p
    }

    // ---- A cell: its gauge's inputs, from the cell and its entry

    component Cell: GaugeCard {
        id: card
        required property var cell
        // On a page two or more away from the one shown, the gauge loads
        // between frames, so a full document doesn't hold back the first
        // one; on the page shown and its neighbors (a swipe drags one in)
        // it finishes at once
        property bool later: false
        // Whether this card's page is the one on screen and the app is
        // active: the sparkline samples only then (a SwipeView keeps its
        // neighbors visible)
        property bool shown: true
        property int pageNo: -1
        property int cellIndex: -1
        readonly property bool editable: root.editing && pageNo === root.pageIndex
        readonly property bool lifted: root.held === cellIndex && pageNo === root.pageIndex
        // The panel names this cell, in edit mode or out of it
        readonly property bool chosen: pageNo === root.pageIndex && root.selected === cellIndex
                                       && (root.editing || root.panel !== "")

        // What the panel's Digits and Lights show with the option unset: what
        // the cell draws, in or out of edit mode
        Binding {
            when: card.chosen
            target: root
            property: "selectedDigits"
            value: card.digits
        }
        Binding {
            when: card.chosen
            target: root
            property: "selectedCount"
            value: card.count
        }

        // A press held still lifts the cell; these go to its page's grid
        signal holdStarted(point scene)
        signal holdMoved(point scene)
        signal holdEnded(bool canceled)

        // A blank cell draws its own outline in edit mode and nothing else.
        // On a joined panel no cell paints, except the one held up: a card
        // again, which covers what it passes over
        plain: (root.joined && !lifted) || kind === "blank"
        scale: lifted ? 1.03 : 1
        z: lifted ? 5 : 0

        // The sweep plays on the dials, the cells and the shift lights only
        function sweep() {
            if (gauge.item && kind !== "bar" && kind !== "digital"
                    && kind !== "seven-segment" && kind !== "sparkline")
                (gauge.item as GaugeBase).sweep()
        }

        readonly property string kind: cell && typeof cell.kind === "string" ? cell.kind : ""
        readonly property var options: cell && cell.options ? cell.options : ({})
        readonly property string cellUnit: cell && typeof cell.unit === "string" ? cell.unit : ""
        readonly property string label: cell && typeof cell.label === "string" ? cell.label : ""
        readonly property SignalHub.Entry entry: hub.lookup(cell && cell.refs ? cell.refs : [])

        readonly property bool hasValue: Bridge.connected && entry !== null && entry.hasValue
        readonly property real value: entry ? entry.value : 0
        readonly property real windowHigh: entry ? entry.windowHigh : 0
        readonly property real windowLow: entry ? entry.windowLow : 0
        readonly property bool stale: hasValue && entry.ageMs > Theme.staleMs
        // An enumeration's label, which only `digital` is offered
        readonly property string valueLabel: entry && entry.isEnum ? entry.label : ""

        // The row's unit against the one the cell was made for (section 6)
        readonly property string expectedUnit: root.shownUnit(cellUnit)
        readonly property bool mismatch: entry !== null && entry.unit !== ""
                                         && expectedUnit !== "" && entry.unit !== expectedUnit
        readonly property string unit: entry && entry.unit !== "" ? entry.unit : expectedUnit
        readonly property int decimals: options.decimals !== undefined ? options.decimals
                                        : entry ? entry.decimals : 0

        // Section 3: the first state that applies
        readonly property string note: {
            let words = ""
            if (!Bridge.connected) words = qsTr("no session")
            else if (entry === null) words = qsTr("not in this profile: %1").arg(label)
            // Live's rule (hiddenCount): the ECU reported it unsupported, or
            // never answered it
            else if (entry.hidden) words = qsTr("not sent by this ECU")
            else if (entry.refusedCode !== "") words = qsTr("refused (%1)").arg(entry.refusedCode)
            else if (!entry.hasValue) words = qsTr("waiting for data")
            return words !== "" && mismatch ? qsTr("%1 (expected %2)").arg(words).arg(expectedUnit)
                                            : words
        }

        function opt(name, fallback) {
            return options[name] !== undefined ? options[name] : fallback
        }
        // A zone in the unit shown; none under another unit's row
        function zone(name) {
            return mismatch || options[name] === undefined ? NaN
                                                           : root.shown(cellUnit, options[name])
        }

        // The scale: the stored ends shown in the current units (a
        // dial's converted end snapped to its major step), or the row's
        // own range under another unit. A step that would draw more
        // ticks than a face holds gives a round one instead.
        readonly property var axis: {
            let a = 0, b = 100
            let snap = false
            const d = DashboardBook.defaultRange(cellUnit)
            const f = opt("from", d.from), t = opt("to", d.to)
            if (mismatch) {
                const lo = entry.rangeLow, hi = entry.rangeHigh
                if (isFinite(lo) && isFinite(hi) && lo < hi) {
                    a = lo
                    b = hi
                }
            } else {
                a = root.shown(cellUnit, f)
                b = root.shown(cellUnit, t)
                snap = kind === "analog" && Bridge.unitSystem !== 0
            }
            if (!(a < b)) b = a + 1
            let major = opt("majorStep", NaN)
            if (!(major > 0) || (b - a) / major > 40)
                major = root.niceStep((b - a) / 8)
            if (snap) {
                const sa = a !== f ? Math.round(a / major) * major : a
                const sb = b !== t ? Math.round(b / major) * major : b
                if (sa < sb) {
                    a = sa
                    b = sb
                }
            }
            let minor = opt("minorStep", major / 2)
            if (!(minor > 0) || (b - a) / minor > 200)
                minor = major / 2
            return { from: a, to: b, major: major, minor: minor }
        }
        readonly property real from: axis.from
        readonly property real to: axis.to
        readonly property real majorStep: axis.major
        readonly property real minorStep: axis.minor
        readonly property real coldBelow: zone("coldBelow")
        readonly property real warnFrom: zone("warnFrom")
        readonly property real dangerFrom: zone("dangerFrom")
        // A stored "info" draws as the value's own color: blue means cold alone
        readonly property color accent: {
            const a = opt("accent", "accent")
            return a === "success" ? Theme.success : Theme.gaugeValue
        }

        // Each kind's own
        readonly property real labelDivisor: opt("labelDivisor", 1)
        readonly property string caption: opt("caption", unit)
        readonly property string peakLabel: opt("peakLabel", qsTr("Peak"))
        readonly property bool showPeak: opt("peak", false)
        // The new kinds' own options (ADR-0057 section 1): the cells' count
        // and curve, the digits, the sparkline's window, the bar standing up
        // and its ticks
        readonly property int cells: opt("cells", 24)
        readonly property real rise: opt("rise", 1.6)
        readonly property int digits: opt("digits", DashboardBook.digitsFor(from, to, decimals))
        readonly property int windowS: opt("window", 30)
        readonly property bool vertical: opt("vertical", false)
        readonly property bool marks: opt("marks", true)
        readonly property bool showProgress: opt("progress", false)
        readonly property string description: opt("description", "")
        readonly property var scaleMarks: {
            const marks = []
            for (let i = 0; i <= 4; ++i)
                marks.push(from + (to - from) * i / 4)
            return marks
        }
        readonly property int count: opt("count", root.compact ? 11 : 15)
        readonly property real ledSpacing: root.compact ? 6 : 10

        readonly property bool drawn: gauge.sourceComponent !== null
        padding: kind === "shift-light" ? 8 : kind === "bar" ? 14 : 12

        readonly property var shared: ["value", "windowHigh", "windowLow", "hasValue", "stale",
                                       "from", "to", "label", "unit", "note", "decimals",
                                       "coldBelow", "warnFrom", "dangerFrom", "accent", "showPeak"]
        readonly property var own: ({
            "analog": ["majorStep", "minorStep", "labelDivisor", "caption", "peakLabel",
                       "showProgress"],
            "bar": ["scaleMarks", "vertical", "marks"],
            "segments": ["cells", "rise"],
            "seven-segment": ["digits"],
            "sparkline": ["windowS", "shown"],
            "digital": ["description", "valueLabel", "shown"],
            "shift-light": ["count", "ledSpacing"]
        })

        // A held peak, low and trace belong to the signal that made them:
        // a cell that shows another signal starts them again
        onEntryChanged: if (gauge.item) (gauge.item as GaugeBase).resetPeaks()

        Loader {
            id: gauge
            objectName: "gauge"
            anchors.fill: parent
            // Editable, the gauge leaves Remove's corner free, so the
            // button covers no title
            anchors.rightMargin: card.editable ? Theme.touchTarget - card.padding + 2 : 0
            asynchronous: card.later
            sourceComponent: root.kindComponent(card.kind)
            onLoaded: {
                const g = item
                for (const name of card.shared.concat(card.own[card.kind] ?? []))
                    g[name] = Qt.binding(() => card[name])
                if (root.sweepIsOn()) card.sweep()
            }
            Connections {
                target: root
                function onSweepRequested() { card.sweep() }
                function onPeaksResetRequested() {
                    if (gauge.item) (gauge.item as GaugeBase).resetPeaks()
                }
            }
        }

        // A tap on a dial that shows its peak resets the peak, and in edit
        // mode a tap opens the cell's panel. A hold, in or out of edit mode,
        // lifts the cell. A blank cell has nothing to hold outside edit mode
        MouseArea {
            objectName: "cellTap"
            z: 1
            anchors.fill: parent
            anchors.margins: -card.padding
            enabled: card.pageNo === root.pageIndex && (card.kind !== "blank" || root.editing)
            // Moved past the drag distance before the hold completed: a
            // swipe or a scroll, even where nothing has taken the finger
            property bool strayed: false
            property point pressedAt: Qt.point(0, 0)
            onPressed: (mouse) => {
                strayed = false
                pressedAt = Qt.point(mouse.x, mouse.y)
            }
            onClicked: {
                if (strayed)
                    return
                if (root.driving) {
                    root.driveTap()
                    return
                }
                if (card.kind === "analog" && card.showPeak && gauge.item)
                    (gauge.item as GaugeBase).resetPeaks()
                if (card.editable)
                    root.select(card.cellIndex)
            }
            onPressAndHold: (mouse) => {
                if (root.driving)
                    return
                if (strayed)
                    mouse.accepted = false
                else
                    card.holdStarted(mapToItem(null, mouse.x, mouse.y))
            }
            onPositionChanged: (mouse) => {
                if (card.lifted)
                    card.holdMoved(mapToItem(null, mouse.x, mouse.y))
                else if (Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y)
                         > Application.styleHints.startDragDistance)
                    strayed = true
            }
            onReleased: if (card.lifted) card.holdEnded(false)
            onCanceled: if (card.lifted) card.holdEnded(true)
        }
        // The outline and Remove exist only while the cell is editable
        Loader {
            z: 2
            active: card.chosen || card.lifted
            anchors.fill: parent
            anchors.margins: -card.padding + 2
            sourceComponent: Rectangle {
                objectName: "cellSelected"
                color: "transparent"
                border.color: Theme.primary
                border.width: 2
                radius: Theme.radius
            }
        }
        Loader {
            z: 3
            active: card.editable
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: -card.padding + 2
            width: Theme.touchTarget
            height: Theme.touchTarget
            sourceComponent: ThemedButton {
                id: removeBox
                objectName: "cellRemove"
                compact: true
                text: "\u00D7"
                Accessible.name: qsTr("Remove %1").arg(card.label)
                onClicked: root.removeCell(card.cellIndex)
                // A touch's size drawn as a smaller raised square with an edge:
                // a button, not an empty tile, and inside a short cell
                background: Item {
                    Rectangle {
                        objectName: "cellRemoveFace"
                        anchors.centerIn: parent
                        width: 32
                        height: 32
                        radius: Theme.radius
                        color: removeBox.down ? removeBox.pressedColor
                             : removeBox.hovered ? removeBox.hoverColor
                             : removeBox.baseColor
                        border.width: removeBox.visualFocus ? 2 : 1
                        border.color: removeBox.visualFocus ? Theme.text : Theme.outline
                    }
                }
                contentItem: Text {
                    text: removeBox.text
                    font.pixelSize: Theme.fontBody + 6
                    font.weight: Font.DemiBold
                    color: Theme.text
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        // A kind this version doesn't draw, kept so it can be removed
        Label {
            objectName: "unknownKind"
            visible: !card.drawn
            anchors.centerIn: parent
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("a gauge this version of Havne doesn't have")
            color: Theme.textDim
            font.pixelSize: Theme.fontCaption
        }
    }

    // ---- Edit mode's panels

    // A number option, typed in the unit shown when `converted`; empty
    // removes it when `clearable`, else puts the shown value back
    component OptionField: TextField {
        id: field
        required property string option
        property bool converted: true
        property bool clearable: true
        property real fallback: NaN
        readonly property var cell: root.selectedCell
        readonly property var value: {
            const o = cell && cell.options ? cell.options[field.option] : undefined
            return o !== undefined ? o : field.fallback
        }
        readonly property string shownText: cell === null ? ""
            : root.fieldText(converted ? root.shown(cell.unit ?? "", value) : value)
        Layout.fillWidth: true
        Layout.preferredHeight: Theme.touchTarget
        text: shownText
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        onEditingFinished: {
            const t = text.trim()
            if (t === "" && clearable) {
                root.setOptions({ [option]: null })
            } else {
                const v = Number(t)
                if (t !== "" && isFinite(v))
                    root.setOptions({ [option]: converted ? root.stored(cell.unit ?? "", v) : v })
            }
            text = Qt.binding(() => field.shownText)
        }
    }

    component Caption: Label {
        color: Theme.textDim
        font.pixelSize: Theme.fontCaption
    }

    Component {
        id: cellPanel
        GaugeCard {
            id: panelCard
            objectName: "dashCellPanel"
            readonly property real touchMin: Theme.touchTarget
            readonly property var cell: root.selectedCell
            readonly property string kind: cell ? cell.kind : ""
            readonly property string unitShown: cell ? root.shownUnit(cell.unit ?? "") : ""
            readonly property var range: DashboardBook.defaultRange(cell ? cell.unit ?? "" : "")
            // The row the cell shows, whose decimals it uses until it has its own
            readonly property SignalHub.Entry entry: hub.lookup(cell && cell.refs ? cell.refs : [])
            readonly property bool phone: root.form === "phone"
            readonly property bool hasSignal: kind !== "blank"
            // The cell's label, or its kind's name when it has none
            readonly property string name: cell && cell.label ? cell.label
                                           : (root.kindNames[kind] ?? kind)
            implicitHeight: panelColumn.implicitHeight + 2 * padding
            padding: 12

            ColumnLayout {
                id: panelColumn
                width: parent.width
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.pixelSize: Theme.fontBody
                        font.bold: true
                        color: Theme.text
                        text: (root.kindNames[panelCard.kind] ?? panelCard.kind)
                              + (panelCard.cell && panelCard.cell.label ? ": " + panelCard.cell.label : "")
                    }
                    ThemedButton {
                        objectName: "panelClose"
                        text: qsTr("Close")
                        onClicked: root.closePanel()
                    }
                }

                // A copy of the whole cell, for this run (section 5)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ThemedButton {
                        objectName: "panelCopy"
                        Layout.fillWidth: true
                        text: qsTr("Copy")
                        onClicked: root.copySelected()
                    }
                    ThemedButton {
                        objectName: "panelPaste"
                        Layout.fillWidth: true
                        enabled: root.canPaste
                        text: qsTr("Paste after")
                        onClicked: root.pasteAfter(root.selected)
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: panelCard.phone ? 2 : 4
                    columnSpacing: 8
                    rowSpacing: 8

                    Caption { visible: panelCard.hasSignal; text: qsTr("Label") }
                    TextField {
                        id: labelField
                        objectName: "panelLabel"
                        visible: panelCard.hasSignal
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.touchTarget
                        maximumLength: DashboardBook.maxLabel
                        text: panelCard.cell && panelCard.cell.label ? panelCard.cell.label : ""
                        onEditingFinished: {
                            root.setLabel(text)
                            text = Qt.binding(() => panelCard.cell && panelCard.cell.label
                                                    ? panelCard.cell.label : "")
                        }
                    }
                    Caption { visible: panelCard.hasSignal; text: qsTr("Signal") }
                    ThemedButton {
                        objectName: "panelSignal"
                        visible: panelCard.hasSignal
                        Layout.fillWidth: true
                        text: qsTr("Change signal")
                        onClicked: root.changeSignal()
                    }

                    Caption { visible: panelCard.hasSignal; text: qsTr("From (%1)").arg(panelCard.unitShown) }
                    OptionField {
                        objectName: "panelFrom"
                        visible: panelCard.hasSignal
                        option: "from"
                        clearable: false
                        fallback: panelCard.range.from
                    }
                    Caption { visible: panelCard.hasSignal; text: qsTr("To (%1)").arg(panelCard.unitShown) }
                    OptionField {
                        objectName: "panelTo"
                        visible: panelCard.hasSignal
                        option: "to"
                        clearable: false
                        fallback: panelCard.range.to
                    }

                    Caption { visible: panelCard.hasSignal; text: qsTr("Cold below") }
                    OptionField { objectName: "panelColdBelow"; visible: panelCard.hasSignal; option: "coldBelow" }
                    Caption { visible: panelCard.hasSignal; text: qsTr("Warning from") }
                    OptionField { objectName: "panelWarnFrom"; visible: panelCard.hasSignal; option: "warnFrom" }
                    Caption { visible: panelCard.hasSignal; text: qsTr("Danger from") }
                    OptionField { objectName: "panelDangerFrom"; visible: panelCard.hasSignal; option: "dangerFrom" }

                    Caption { visible: panelCard.hasSignal; text: qsTr("Decimals") }
                    SpinBox {
                        objectName: "panelDecimals"
                        visible: panelCard.hasSignal
                        Accessible.name: qsTr("Decimals")
                        Layout.preferredHeight: Theme.touchTarget
                        from: 0
                        to: 6
                        value: panelCard.cell && panelCard.cell.options
                               && panelCard.cell.options.decimals !== undefined
                               ? panelCard.cell.options.decimals
                               : panelCard.entry ? panelCard.entry.decimals : 0
                        onValueModified: root.setOptions({ decimals: value })
                    }

                    // The span, in the grid's units, which the words name
                    Caption { text: qsTr("Columns") }
                    SpinBox {
                        objectName: "panelW"
                        Accessible.name: qsTr("Columns")
                        Layout.preferredHeight: Theme.touchTarget
                        from: 1
                        to: DashboardBook.maxColumns
                        value: panelCard.cell ? panelCard.cell.w : 1
                        onValueModified: root.resize(value, panelCard.cell.h)
                    }
                    Caption { text: qsTr("Rows") }
                    SpinBox {
                        objectName: "panelH"
                        Accessible.name: qsTr("Rows")
                        Layout.preferredHeight: Theme.touchTarget
                        // A stored h under the phone's step stays reachable
                        from: root.phoneForm && panelCard.cell
                              ? Math.min(root.spanScale, panelCard.cell.h) : 1
                        // The desktop's 16, or a taller stored cell's own height
                        to: root.phoneForm ? DashboardBook.maxRows
                            : Math.max(DashboardBook.format1MaxRows, panelCard.cell ? panelCard.cell.h : 1)
                        stepSize: root.spanScale
                        value: panelCard.cell ? panelCard.cell.h : 1
                        onValueModified: root.resize(panelCard.cell.w, root.phoneForm
                            ? DashboardBook.stepRows(panelCard.cell.h, value > panelCard.cell.h)
                            : value)
                    }

                    // A checkbox, as every other yes-or-no in the app
                    ThemedCheckBox {
                        objectName: "panelPeak"
                        text: qsTr("Peak hold")
                        Accessible.name: qsTr("Peak hold")
                        visible: root.peakKinds.indexOf(panelCard.kind) >= 0
                        implicitHeight: Math.max(implicitIndicatorHeight + topPadding + bottomPadding,
                                                 Theme.touchTarget)
                        checked: panelCard.cell && panelCard.cell.options
                                 ? panelCard.cell.options.peak === true : false
                        onToggled: root.setOptions({ peak: checked })
                    }

                    // The kind's own
                    Caption { visible: panelCard.kind === "analog"; text: qsTr("Major step") }
                    OptionField {
                        objectName: "panelMajorStep"
                        visible: panelCard.kind === "analog"
                        option: "majorStep"
                        converted: false
                    }
                    Caption { visible: panelCard.kind === "analog"; text: qsTr("Caption") }
                    TextField {
                        objectName: "panelCaption"
                        visible: panelCard.kind === "analog"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.touchTarget
                        maximumLength: 40
                        readonly property string current: panelCard.cell && panelCard.cell.options
                                                          && panelCard.cell.options.caption
                                                          ? panelCard.cell.options.caption : ""
                        text: current
                        onEditingFinished: {
                            root.setOptions({ caption: text === "" ? null : text })
                            text = Qt.binding(() => current)
                        }
                    }
                    Caption { visible: panelCard.kind === "digital"; text: qsTr("Description") }
                    TextField {
                        objectName: "panelDescription"
                        visible: panelCard.kind === "digital"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.touchTarget
                        maximumLength: 80
                        readonly property string current: panelCard.cell && panelCard.cell.options
                                                          && panelCard.cell.options.description
                                                          ? panelCard.cell.options.description : ""
                        text: current
                        onEditingFinished: {
                            root.setOptions({ description: text === "" ? null : text })
                            text = Qt.binding(() => current)
                        }
                    }
                    Caption { visible: panelCard.kind === "segments"; text: qsTr("Cells") }
                    SpinBox {
                        objectName: "panelCells"
                        Accessible.name: qsTr("Cells")
                        visible: panelCard.kind === "segments"
                        Layout.preferredHeight: Theme.touchTarget
                        from: 2
                        to: 64
                        value: panelCard.cell && panelCard.cell.options
                               && panelCard.cell.options.cells !== undefined
                               ? panelCard.cell.options.cells : 24
                        onValueModified: root.setOptions({ cells: value })
                    }
                    Caption { visible: panelCard.kind === "segments"; text: qsTr("Curve") }
                    OptionField {
                        objectName: "panelRise"
                        Accessible.name: qsTr("Curve")
                        visible: panelCard.kind === "segments"
                        option: "rise"
                        converted: false
                        fallback: 1.6
                    }
                    Caption { visible: panelCard.kind === "seven-segment"; text: qsTr("Digits") }
                    SpinBox {
                        objectName: "panelDigits"
                        Accessible.name: qsTr("Digits")
                        visible: panelCard.kind === "seven-segment"
                        Layout.preferredHeight: Theme.touchTarget
                        from: 1
                        to: 8
                        value: panelCard.cell && panelCard.cell.options
                               && panelCard.cell.options.digits !== undefined
                               ? panelCard.cell.options.digits : root.selectedDigits
                        onValueModified: root.setOptions({ digits: value })
                    }
                    Caption { visible: panelCard.kind === "sparkline"; text: qsTr("Window (s)") }
                    SpinBox {
                        objectName: "panelWindow"
                        Accessible.name: qsTr("Window (s)")
                        visible: panelCard.kind === "sparkline"
                        Layout.preferredHeight: Theme.touchTarget
                        from: 10
                        to: 120
                        stepSize: 10
                        value: panelCard.cell && panelCard.cell.options
                               && panelCard.cell.options.window !== undefined
                               ? panelCard.cell.options.window : 30
                        onValueModified: root.setOptions({ window: value })
                    }
                    Caption { visible: panelCard.kind === "bar"; text: qsTr("Standing up") }
                    Switch {
                        objectName: "panelVertical"
                        Accessible.name: qsTr("Standing up")
                        visible: panelCard.kind === "bar"
                        implicitHeight: Math.max(implicitIndicatorHeight + topPadding + bottomPadding,
                                                 Theme.touchTarget)
                        checked: panelCard.cell && panelCard.cell.options
                                 ? panelCard.cell.options.vertical === true : false
                        onToggled: {
                            // Standing up is a tall, narrow bar: the default
                            // span follows the toggle, and a size the user
                            // chose stays; one write, so one undo step
                            // (the write rebinds this switch, so its state is read first)
                            const on = checked
                            const c = panelCard.cell
                            const s = root.spanScale
                            let base = root.doc
                            if (c && on && c.w === 2 * s && c.h === 2 * s)
                                base = DashboardBook.resizeCell(base, root.pageIndex, root.selected, s, 4 * s)
                            else if (c && !on && c.w === s && c.h === 4 * s)
                                base = DashboardBook.resizeCell(base, root.pageIndex, root.selected, 2 * s, 2 * s)
                            root.setOptions({ vertical: on }, base)
                        }
                    }
                    Caption { visible: panelCard.kind === "bar"; text: qsTr("Scale marks") }
                    Switch {
                        objectName: "panelMarks"
                        Accessible.name: qsTr("Scale marks")
                        visible: panelCard.kind === "bar"
                        implicitHeight: Math.max(implicitIndicatorHeight + topPadding + bottomPadding,
                                                 Theme.touchTarget)
                        checked: panelCard.cell && panelCard.cell.options
                                 ? panelCard.cell.options.marks !== false : true
                        onToggled: root.setOptions({ marks: checked })
                    }
                    Caption { visible: panelCard.kind === "shift-light"; text: qsTr("Lights") }
                    SpinBox {
                        objectName: "panelCount"
                        Accessible.name: qsTr("Lights")
                        visible: panelCard.kind === "shift-light"
                        Layout.preferredHeight: Theme.touchTarget
                        from: 1
                        to: 32
                        value: panelCard.cell && panelCard.cell.options
                               && panelCard.cell.options.count !== undefined
                               ? panelCard.cell.options.count : root.selectedCount
                        onValueModified: root.setOptions({ count: value })
                    }
                }

                // The order: both forms' grids follow it
                GridLayout {
                    Layout.fillWidth: true
                    columns: panelCard.phone ? 2 : 4
                    columnSpacing: 8
                    rowSpacing: 8
                    ThemedButton {
                        objectName: "panelEarlier"
                        Layout.fillWidth: true
                        enabled: root.selected > 0
                        text: qsTr("Move earlier")
                        onClicked: root.moveTo(root.selected - 1)
                    }
                    ThemedButton {
                        objectName: "panelLater"
                        Layout.fillWidth: true
                        enabled: root.selected < root.shownCells.length - 1
                        text: qsTr("Move later")
                        onClicked: root.moveTo(root.selected + 1)
                    }
                    ThemedButton {
                        objectName: "panelStart"
                        Layout.fillWidth: true
                        enabled: root.selected > 0
                        text: qsTr("To the start")
                        onClicked: root.moveTo(0)
                    }
                    ThemedButton {
                        objectName: "panelEnd"
                        Layout.fillWidth: true
                        enabled: root.selected < root.shownCells.length - 1
                        text: qsTr("To the end")
                        onClicked: root.moveTo(root.shownCells.length - 1)
                    }
                }

                // Remove, last and quiet, in edit mode or out of it; Undo
                // takes it back, so it asks nothing
                ThemedButton {
                    objectName: "panelRemove"
                    flat: true
                    text: qsTr("Remove")
                    Accessible.name: qsTr("Remove %1").arg(panelCard.name)
                    onClicked: root.removeCell(root.selected)
                }
            }
        }
    }

    // Add's kinds, each with a still preview
    Component {
        id: addPanel
        GaugeCard {
            objectName: "dashAddPanel"
            readonly property real touchMin: Theme.touchTarget
            implicitHeight: addColumn.implicitHeight + 2 * padding
            padding: 12
            ColumnLayout {
                id: addColumn
                width: parent.width
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        font.pixelSize: Theme.fontBody
                        font.bold: true
                        color: Theme.text
                        text: qsTr("Add a gauge: its kind")
                    }
                    ThemedButton {
                        objectName: "addClose"
                        text: qsTr("Close")
                        onClicked: {
                            root.commitTyping()
                            root.panel = ""
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: root.drawnKinds
                        ColumnLayout {
                            id: kindTile
                            required property string modelData
                            // Comparisons, not an indexOf on an array literal: the QML
                            // compiler's C++ for that narrows qsizetype to int, which
                            // Xcode's -Wshorten-64-to-32 rejects
                            readonly property bool wide: modelData === "bar"
                                                         || modelData === "shift-light"
                                                         || modelData === "segments"
                                                         || modelData === "sparkline"
                            spacing: 4
                            Loader {
                                objectName: "preview_" + kindTile.modelData
                                Layout.preferredWidth: kindTile.wide ? 200 : 120
                                Layout.preferredHeight: kindTile.modelData === "shift-light" ? 40
                                                      : kindTile.modelData === "segments"
                                                        || kindTile.modelData === "sparkline" ? 100
                                                      : kindTile.wide || kindTile.modelData === "seven-segment" ? 70
                                                      : 120
                                sourceComponent: root.kindComponent(kindTile.modelData)
                                // Still: 60 percent of a 0 to 100 range, never live
                                onLoaded: {
                                    const g = item as GaugeBase
                                    g.from = 0
                                    g.to = 100
                                    g.hasValue = true
                                    g.value = 60
                                    g.label = root.kindNames[kindTile.modelData]
                                    // A sparkline's still is a made-up trace, and it
                                    // takes no samples of its own
                                    if (kindTile.modelData === "sparkline") {
                                        const spark = item as SparkGauge
                                        spark.shown = false
                                        spark.windowS = 12   // 120 points at 10 Hz fill the box
                                        spark.history = Array.from({ length: 120 },
                                                                   (_, i) => 55 + 25 * Math.sin(i / 10))
                                    }
                                }
                            }
                            ThemedButton {
                                objectName: "addKind_" + kindTile.modelData
                                Layout.fillWidth: true
                                text: root.kindNames[kindTile.modelData]
                                onClicked: root.pickKind(kindTile.modelData)
                            }
                        }
                    }
                }
            }
        }
    }

    // A signal for Add's kind, or the edited cell's new one: the session's
    // rows with references, or those of the profile Settings has chosen
    Component {
        id: signalPanel
        GaugeCard {
            id: signalCard
            objectName: "dashSignalPanel"
            readonly property real touchMin: Theme.touchTarget
            // The rows the bar leaves: every filter property shares one
            // change signal, so reading active re-filters on any change
            readonly property var offered: root.pickable(root.addKind !== "" ? root.addKind
                                                         : root.selectedCell ? root.selectedCell.kind : "")
            readonly property var rows: {
                const f = rowFilter
                const all = offered
                if (!f.active)
                    return all
                return all.filter(r => f.matches(r.name, r.unit, r.group, false))
            }
            implicitHeight: signalColumn.implicitHeight + 2 * padding
            padding: 12
            SignalFilter { id: rowFilter }
            ColumnLayout {
                id: signalColumn
                width: parent.width
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        font.pixelSize: Theme.fontBody
                        font.bold: true
                        color: Theme.text
                        elide: Text.ElideRight
                        text: root.addKind !== ""
                              ? qsTr("Add a gauge: a signal for the %1").arg(root.kindNames[root.addKind])
                              : qsTr("A signal for this gauge")
                    }
                    ThemedButton {
                        objectName: "signalBack"
                        text: qsTr("Back")
                        onClicked: root.panel = root.addKind !== "" ? "add" : "cell"
                    }
                }
                // The text alone: a Popup doesn't take the page's zoom, and
                // Plotted only means nothing to a gauge
                SignalFilterBar {
                    objectName: "signalFilter"
                    Layout.fillWidth: true
                    filter: rowFilter
                    filterButtonShown: false
                }
                Label {
                    objectName: "signalNone"
                    visible: signalCard.rows.length === 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    text: signalCard.offered.length > 0 ? qsTr("No signal matches.")
                        : Bridge.connected ? qsTr("This session's profile has no signal a gauge can show.")
                        : qsTr("No signals: connect, or choose a profile in Settings.")
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        model: signalCard.rows
                        ThemedButton {
                            required property var modelData
                            objectName: "pickSignal_" + modelData.ref
                            text: modelData.unit !== "" ? qsTr("%1 (%2)").arg(modelData.name).arg(modelData.unit)
                                                        : modelData.name
                            onClicked: root.pickSignal(modelData)
                        }
                    }
                }
            }
        }
    }

    // No session on the phone: the connect list, in the panel's place above
    // the gauges, on every page. Settings owns connecting
    property var connectUris: []
    property string connectVehicleUri: ""
    // A tapped row is the user's choice; else the picked vehicle's address
    property string connectTapped: ""
    readonly property int connectPick: {
        const wanted = connectTapped !== "" ? connectTapped : connectVehicleUri
        return Math.max(0, connectUris.indexOf(wanted))
    }
    property string connectError: ""
    readonly property bool connectShown: form === "phone" && !Bridge.connected && !editing
    signal connectToUri(string uri)
    signal otherAddressRequested()
    signal logRequested()
    onConnectUrisChanged: connectTapped = ""
    onConnectVehicleUriChanged: connectTapped = ""
    Connections {
        target: Bridge
        function onConnectResult(uri, error) { root.connectError = error }
    }
    Component {
        id: connectPanel
        DashConnectList {
            uris: root.connectUris
            selected: root.connectPick
            errorText: root.connectError
            onPicked: index => root.connectTapped = root.connectUris[index]
            onConnectTo: uri => {
                root.connectError = ""
                root.connectToUri(uri)
            }
            onOtherAddress: root.otherAddressRequested()
            onShowLog: root.logRequested()
        }
    }

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

    // The desktop's link state: the dot and words, and the Night switch at
    // their end (ADR-0058 section 6: a status with a tap)
    Component {
        id: linkState
        RowLayout {
            spacing: 10
            // Under 60 px the label would read "N...n": the dot and the label
            // fade (opacity, so the layout still holds them) and the header's
            // pill carries the state alone
            Rectangle {
                objectName: "linkDot"
                implicitWidth: 8; implicitHeight: 8   // layout-managed
                radius: 4
                color: Theme.linkColor(Bridge.linkState)
                opacity: linkLabel.opacity
            }
            Label {
                id: linkLabel
                objectName: "linkLabel"
                Layout.fillWidth: true
                opacity: width >= 60 ? 1 : 0
                elide: Text.ElideMiddle
                // Short statuses come from Theme.linkLabel; "ok" adds the
                // address where the row leaves room for it (the header
                // shows it anyway). Long-form detail (episode counts,
                // error codes) stays in LiveView's banner.
                readonly property bool roomForAddress: width >= 150
                text: Bridge.linkState === "ok"
                      ? (roomForAddress ? qsTr("Live · %1").arg(Bridge.uriLabel) : qsTr("Live"))
                      : Theme.linkLabel(Bridge.linkState)
                font.pixelSize: Theme.fontCaption
                color: Theme.linkColor(Bridge.linkState)
            }
            NightSwitch {}
        }
    }
    Component { id: toolNight; NightSwitch {} }

    // The drive view's one line of chrome: the link state and the address,
    // and a log's or recording's progress (section 2). It takes no taps
    Item {
        id: driveStatus
        objectName: "driveStatusLine"
        visible: root.driving
        x: 0
        y: root.driveInsetTop
        width: root.width
        height: 28
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: root.margin
            anchors.rightMargin: root.margin
            spacing: 6
            Rectangle {
                objectName: "driveStatusDot"
                implicitWidth: 8; implicitHeight: 8   // layout-managed
                radius: 4
                color: Theme.linkColor(Bridge.linkState)
            }
            Label {
                objectName: "driveStatusWord"
                font.pixelSize: Theme.fontCaption
                color: Theme.text
                text: Theme.linkShortLabel(Bridge.linkState)
            }
            Label {
                objectName: "driveStatusAddress"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                font.pixelSize: Theme.fontCaption
                color: Theme.textDim
                text: qsTr("· %1").arg(Bridge.uriLabel)
            }
            Label {
                objectName: "driveStatusRun"
                visible: Bridge.logActive || Bridge.recordingActive || root.driveLogEnded
                font.pixelSize: Theme.fontCaption
                color: Theme.text
                text: Bridge.logActive ? qsTr("Log")
                    : root.driveLogEnded ? qsTr("Log ended") : qsTr("Recording")
            }
            Label {
                objectName: "driveStatusTime"
                visible: Bridge.logActive
                font.family: Theme.valueFamily
                font.features: { "tnum": 1 }
                font.pixelSize: Theme.fontCaption
                color: Theme.text
                text: root.driveLogTime
            }
            Label {
                objectName: "driveStatusDetail"
                readonly property string detail:
                    Bridge.logActive ? root.driveLogSize
                  : !Bridge.recordingActive || Bridge.markCount === 0 ? ""
                  : Bridge.markCount === 1 ? qsTr("1 mark")
                  : qsTr("%1 marks").arg(Bridge.markCount)
                visible: detail !== ""
                font.pixelSize: Theme.fontCaption
                color: Theme.textDim
                text: qsTr("· %1").arg(detail)
            }
        }
    }

    // Behind the revealed tool row, so the gauges don't show through it.
    // A touch on it counts as a touch on the bars
    Rectangle {
        objectName: "driveToolsBackdrop"
        visible: topBar.visible && root.driving
        z: 1
        x: 0
        y: topBar.y - root.gap / 2
        width: root.width
        height: topBar.height + root.gap
        color: Theme.bg
        MouseArea {
            anchors.fill: parent
            enabled: root.driving
            onPressed: root.barTouched()
        }
        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: root.hairlineWidth
            color: Theme.hairline
        }
    }

    // ---- The header: on the desktop the title, the link state and the
    // tools, on the phone the tools alone (the tab bar names the page and
    // the top bar has the link state); why the stored layout isn't shown;
    // last, the desktop's page strip, on the panel's upper edge

    ColumnLayout {
        id: topBar
        readonly property real revealedY: root.driveHeaderHeight + root.gap / 2
        visible: !root.driving || root.barsShown > 0
        // Over the pages, so the shown tab covers the panel's top line
        z: root.driving ? 2 : 1
        x: (root.width - width) / 2
        // In the drive view it comes down from under the header's edge
        y: root.driving ? revealedY - (1 - root.barsShown) * (revealedY + height + root.gap) : root.topGap
        width: Math.max(0, root.width - 2 * root.margin)
        spacing: root.gap / 2

        PageHeader {
            id: header
            Layout.fillWidth: true
            title: root.phoneForm ? "" : qsTr("Dashboard")
            spacing: root.phoneForm ? 8 : 10
            // At a narrow width the row's buttons are compact, the link
            // state gives way first and the title last, so the row fits
            // whatever the font's widths
            titleShrinks: true
            // Reset peaks and Edit at a phone's touch size
            readonly property real touchMin: Theme.touchTarget

            // The desktop's link state follows the title. No width of
            // its own: it takes what the title and buttons leave
            Loader {
                objectName: "linkInRow"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                active: !root.phoneForm
                visible: active
                sourceComponent: linkState
            }
            // Drive, at the start of the phone's row. Its tools sit at the
            // right. With no session it stays, and says what it waits for
            ThemedButton {
                id: driveButton
                objectName: "dashDrive"
                flat: true
                compact: true
                Layout.minimumWidth: Theme.touchTarget
                visible: root.phoneForm
                enabled: Bridge.connected && !root.editing
                text: !Bridge.connected ? qsTr("Connect first")
                    : root.driving ? qsTr("Exit drive") : qsTr("Drive")
                Accessible.name: !Bridge.connected ? qsTr("Connect first: a session shows the gauges full screen")
                               : root.driving ? qsTr("Exit drive: leave the full-screen gauges")
                                              : qsTr("Drive: show the gauges full screen")
                onClicked: root.driving ? root.leaveDrive(true) : root.enterDrive()
            }
            Item {
                objectName: "toolRowStart"
                Layout.fillWidth: true
                visible: root.phoneForm
            }
            // A row too narrow for every tool (a small window, a phone zoomed
            // far in, or no session, whose Drive words are longer) drops Reset
            // peaks, which edit mode's Pages row then offers (moreResetPeaks).
            // Judged against the bar's width (the row can't shrink under its
            // tools) and the tools' own widths, never the row's: no loop.
            // The layout rounds each width up to a whole pixel, and each item
            // shown ahead of the tools (the fills too) takes a spacing
            readonly property real toolsWidth: {
                let ahead = 0
                for (const item of children) {
                    if (item === resetPeaksButton)
                        break
                    if (item.visible)
                        ahead += spacing + (item === driveButton ? Math.ceil(item.implicitWidth) : 0)
                }
                return ahead + Math.ceil(resetPeaksButton.implicitWidth) + spacing
                    + Math.ceil(editButton.implicitWidth)
                    + (nightLoader.active ? spacing + Math.ceil(nightLoader.implicitWidth) : 0)
            }
            readonly property bool holdsResetPeaks: topBar.width >= Math.max(root.toolRowFull, toolsWidth)
            ThemedButton {
                id: resetPeaksButton
                objectName: "resetPeaks"
                flat: true
                compact: root.compact || root.phoneForm
                visible: header.holdsResetPeaks
                text: qsTr("Reset peaks")
                onClicked: root.resetPeaks()
            }
            ThemedButton {
                id: editButton
                objectName: "dashEdit"
                compact: root.compact || root.phoneForm
                horizontalPadding: root.phoneForm ? 18 : (compact ? 6 : 8)
                visible: !root.editing && !root.driving
                text: qsTr("Edit")
                onClicked: root.startEditing()
            }
            Loader {
                id: nightLoader
                objectName: "toolRowNight"
                active: root.phoneForm
                visible: active
                sourceComponent: toolNight
            }
        }

        // What the log did without being asked: Start log's refusal, or
        // an end the user didn't ask for (a failed write, the session's
        // end), in words beside a warning dot, until Dismiss or the next
        // log. Start log sits in Main's header, so the bridge holds the
        // words (Bridge.logNotice) and this page shows them
        RowLayout {
            objectName: "dashLogNotice"
            Layout.fillWidth: true
            visible: Bridge.logNotice !== ""
            spacing: 10
            Rectangle {
                implicitWidth: 8; implicitHeight: 8   // layout-managed
                radius: 4
                color: Theme.warning
            }
            Label {
                objectName: "dashLogNoticeText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                text: Bridge.logNotice
            }
            ThemedButton {
                objectName: "dashLogNoticeDismiss"
                compact: true
                text: qsTr("Dismiss")
                Accessible.name: qsTr("Dismiss the log's notice")
                onClicked: Bridge.clearLogNotice()
            }
        }

        // Edit mode's bar: Undo and Redo where Drive stands outside edit
        // mode, Add and Page, and Done in Edit's place at the end. Compact
        // on the phone, and Add's words shortened on a narrow page, so the
        // row holds them at a 320 px width and a large font
        RowLayout {
            objectName: "dashEditBar"
            readonly property real touchMin: Theme.touchTarget
            Layout.fillWidth: true
            visible: root.editing
            spacing: 8
            // Disabled rather than hidden with nothing to take back, so the row never moves
            ThemedButton {
                objectName: "dashUndo"
                flat: true
                enabled: root.canUndo
                text: qsTr("Undo")
                Accessible.name: root.canUndo ? qsTr("Undo: %1").arg(root.undoWords) : qsTr("Undo")
                onClicked: root.undo()
            }
            ThemedButton {
                objectName: "dashRedo"
                flat: true
                enabled: root.canRedo
                text: qsTr("Redo")
                Accessible.name: root.canRedo ? qsTr("Redo: %1").arg(root.redoWords) : qsTr("Redo")
                onClicked: root.redo()
            }
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "dashAdd"
                compact: root.form === "phone"
                enabled: DashboardBook.canAddCell(root.doc, root.pageIndex)
                text: root.compact ? qsTr("Add") : qsTr("Add gauge")
                Accessible.name: qsTr("Add a gauge")
                onClicked: root.startAdd()
            }
            ThemedButton {
                objectName: "dashMore"
                compact: root.form === "phone"
                checkable: true
                checked: root.moreShown
                text: qsTr("Page")
                onClicked: {
                    root.moreShown = !root.moreShown
                    root.confirmReset = false
                    root.confirmRemovePage = false
                }
            }
            ThemedButton {
                objectName: "dashDone"
                compact: root.form === "phone"
                highlighted: true
                text: qsTr("Done")
                onClicked: root.stopEditing()
            }
        }

        // After an undo or a redo, the step's words; outside edit mode,
        // after each step too, with an Undo, since there is no bar there
        RowLayout {
            objectName: "dashUndoLine"
            Layout.fillWidth: true
            visible: root.undoLine !== "" && !root.driving
            spacing: 10
            Label {
                objectName: "dashUndoLineText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                text: root.undoLine
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
                enabled: root.canUndo
                text: qsTr("Undo")
                Accessible.name: root.canUndo ? qsTr("Undo: %1").arg(root.undoWords) : qsTr("Undo")
                onClicked: root.undo()
            }
        }

        // The pages' controls, then a gap, then Reset (it replaces every page,
        // so it stands apart) and Reset peaks where the tool row has no room
        Flow {
            id: moreRow
            objectName: "dashMoreRow"
            readonly property real touchMin: Theme.touchTarget
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1
            visible: root.editing && root.moreShown && !root.confirmReset && !root.confirmRemovePage
            spacing: 8
            TextField {
                id: pageName
                objectName: "moreRename"
                width: Math.min(220, topBar.width)
                height: Theme.touchTarget
                maximumLength: DashboardBook.maxLabel
                text: root.pages[root.pageIndex] ? root.pages[root.pageIndex].name : ""
                placeholderText: qsTr("Page name")
                Accessible.name: qsTr("This page's name")
                onEditingFinished: {
                    root.renamePage(text)
                    text = Qt.binding(() => root.pages[root.pageIndex]
                                            ? root.pages[root.pageIndex].name : "")
                }
            }
            // The phone's only way to a new page; the desktop's is the strip's "+"
            ThemedButton {
                id: moreAddPage
                objectName: "moreAddPage"
                visible: root.phoneForm
                enabled: DashboardBook.canAddPage(root.doc)
                text: qsTr("Add page")
                onClicked: root.addPage()
            }
            // Nothing to move with one page
            ThemedButton {
                id: moreLeft
                objectName: "moreLeft"
                visible: root.pages.length > 1
                enabled: root.pageIndex > 0
                text: qsTr("Move page left")
                onClicked: root.movePage(-1)
            }
            ThemedButton {
                id: moreRight
                objectName: "moreRight"
                visible: root.pages.length > 1
                enabled: root.pageIndex < root.pages.length - 1
                text: qsTr("Move page right")
                onClicked: root.movePage(1)
            }
            ThemedButton {
                id: moreRemovePage
                objectName: "moreRemovePage"
                enabled: DashboardBook.canRemovePage(root.doc)
                text: qsTr("Remove page")
                onClicked: root.confirmRemovePage = true
            }
            Item {
                objectName: "moreResetGap"
                // The bar's width and the controls' own, never the flow's: a
                // hidden flow sizes itself from its children, and would loop
                readonly property real used: pageName.width + moreRemovePage.width + moreRow.spacing
                    + (moreAddPage.visible ? moreAddPage.width + moreRow.spacing : 0)
                    + (moreLeft.visible ? moreLeft.width + moreRight.width + 2 * moreRow.spacing : 0)
                readonly property real tail: moreReset.width
                    + (moreResetPeaks.visible ? moreResetPeaks.width + moreRow.spacing : 0)
                width: Math.max(16, topBar.width - used - tail - 2 * moreRow.spacing)
                height: 1
            }
            ThemedButton {
                id: moreReset
                objectName: "moreReset"
                flat: true
                text: qsTr("Reset")
                Accessible.name: qsTr("Reset: replace this form's pages with the built-in layout")
                onClicked: root.confirmReset = true
            }
            // Reset peaks, when the tool row has no room for it
            ThemedButton {
                id: moreResetPeaks
                objectName: "moreResetPeaks"
                flat: true
                visible: !header.holdsResetPeaks
                text: qsTr("Reset peaks")
                onClicked: root.resetPeaks()
            }
        }
        RowLayout {
            objectName: "dashRemovePageConfirm"
            readonly property real touchMin: Theme.touchTarget
            Layout.fillWidth: true
            visible: root.editing && root.confirmRemovePage
            spacing: 8
            Label {
                objectName: "removePageQuestion"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                wrapMode: Text.WordWrap
                color: Theme.warning
                font.pixelSize: Theme.fontCaption
                text: qsTr("Remove the page \"%1\" and its gauges?")
                      .arg(root.pages[root.pageIndex] ? root.pages[root.pageIndex].name : "")
            }
            ThemedButton {
                objectName: "removePageConfirm"
                compact: root.form === "phone"
                text: qsTr("Remove")
                onClicked: root.removePage()
            }
            ThemedButton {
                objectName: "removePageKeep"
                compact: root.form === "phone"
                text: qsTr("Keep")
                onClicked: root.confirmRemovePage = false
            }
        }
        RowLayout {
            objectName: "dashResetConfirm"
            readonly property real touchMin: Theme.touchTarget
            Layout.fillWidth: true
            visible: root.editing && root.confirmReset
            spacing: 8
            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                wrapMode: Text.WordWrap
                color: Theme.warning
                font.pixelSize: Theme.fontCaption
                text: qsTr("Replace this form's pages with the built-in layout?")
            }
            ThemedButton {
                objectName: "resetConfirm"
                compact: root.form === "phone"
                text: qsTr("Reset")
                onClicked: root.resetForm()
            }
            ThemedButton {
                objectName: "resetKeep"
                compact: root.form === "phone"
                text: qsTr("Keep")
                onClicked: root.confirmReset = false
            }
        }

        RowLayout {
            objectName: "dashProblem"
            Layout.fillWidth: true
            visible: (root.problem === "damaged" || root.problem === "newer"
                      || root.problem === "dropped") && !root.problemDismissed
            Label {
                objectName: "dashProblemText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.warning
                font.pixelSize: Theme.fontCaption
                text: root.problem === "newer"
                      ? qsTr("The saved dashboard was made by a newer version of Havne. "
                             + "This shows the built-in one; the saved one is kept.")
                      : root.problem === "dropped"
                      ? qsTr("Part of the saved dashboard couldn't be read: %1 left out. "
                             + "The saved text is kept.").arg(root.droppedWords)
                      : qsTr("The saved dashboard couldn't be read. "
                             + "This shows the built-in one; the saved text is kept.")
            }
            ThemedButton {
                objectName: "dashProblemDismiss"
                compact: true
                text: qsTr("Dismiss")
                onClicked: root.problemDismissed = true
            }
        }

        // The desktop's pages as tabs, with two or more or in edit mode;
        // last in the column, so the panel starts at its foot
        Loader {
            id: stripHost
            objectName: "dashPageStripHost"
            Layout.fillWidth: true
            Layout.preferredHeight: item ? (item as DashPageStrip).implicitHeight : 0
            active: !root.phoneForm
            visible: active && (root.pages.length > 1 || root.editing)
            sourceComponent: Component {
                DashPageStrip {
                    pages: root.pages
                    current: root.pageIndex
                    editing: root.editing
                    canAddPage: DashboardBook.canAddPage(root.doc)
                    hairlineWidth: root.hairlineWidth
                    onPageChosen: (i) => root.goTo(i)
                    onAddRequested: root.addPage()
                }
            }
        }
    }

    // ---- The pages: one SwipeView, swiped on the phone form, moved by
    // the strip's tabs and the keys on the desktop form

    SwipeView {
        id: swipe
        objectName: "dashPages"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: root.driving ? driveStatus.bottom : topBar.bottom
        anchors.bottom: indicator.visible ? indicator.top : parent.bottom
        anchors.bottomMargin: root.driving ? root.driveInsetBottom : 0
        clip: true
        interactive: root.form === "phone" && root.held < 0
        // The page shown goes in once every page is in: the first page
        // added makes the index 0, which would override a stored one
        onCountChanged: {
            if (count === root.pages.length && currentIndex !== root.pageIndex)
                setCurrentIndex(root.pageIndex)
        }
        onCurrentIndexChanged: {
            if (count === root.pages.length && currentIndex >= 0) {
                if (currentIndex !== root.pageIndex)
                    root.commitTyping()
                root.pageIndex = currentIndex
            }
        }

        Repeater {
            model: root.pages.length

            Flickable {
                id: pageView
                required property int index
                readonly property var page: root.pages[index]
                readonly property var cells: page && Array.isArray(page.cells) ? page.cells : []
                objectName: "dashPage_" + index
                clip: true
                contentWidth: width
                contentHeight: grid.y + grid.height + root.panelMargin
                interactive: root.held < 0
                // Shown whenever there's more below, not only while
                // scrolling, so a short window says it scrolls
                ScrollBar.vertical: ScrollBar {
                    policy: pageView.contentHeight > pageView.height ? ScrollBar.AlwaysOn
                                                                     : ScrollBar.AsNeeded
                }

                // The grid (section 4): the phone's 6 columns upright and
                // 12 on its side, the desktop's one per 220 px from 2 to 12
                readonly property real gridWidth: Math.max(0, width - 2 * root.panelMargin)
                readonly property int columns: root.form === "phone"
                    ? (root.width > root.height ? 12 : 6)
                    : Math.max(2, Math.min(12, Math.round(gridWidth / 220)))
                readonly property real columnWidth: Math.max(0, (gridWidth - (columns - 1)
                                                                 * root.cellGap) / columns)
                readonly property bool shown: index === root.pageIndex
                // In edit mode the page ends with a "+" cell while it has room
                readonly property bool addCell: root.editing && shown
                                                && DashboardBook.canAddCell(root.doc, index)
                // While a held cell moves, the order its drop would make
                readonly property bool reflowing: shown && root.dropAt >= 0
                readonly property var ordered: reflowing
                    ? DashboardBook.reordered(cells, root.held, root.dropAt) : cells
                readonly property var placed: addCell
                    ? ordered.concat([{ w: root.spanScale, h: 2 * root.spanScale }]) : ordered
                // Edit mode and a held cell show the true sizes
                readonly property var boxes: {
                    const b = DashboardBook.place(placed, columns)
                    return root.joined && !root.editing && root.held < 0
                        ? DashboardBook.stretched(placed, b, columns) : b
                }
                readonly property var seams: root.joined
                    ? DashboardBook.seams(placed, boxes, columns) : []
                readonly property int tallest: placed.reduce((m, c) => Math.max(m, c.h), 1)
                // The row unit is a quarter of a column, so h = 4w is
                // square; never so tall that the tallest cell overflows
                // what the page shows, down to a floor of 160 px, below
                // which the page scrolls instead
                readonly property real pitch: Math.max(1, Math.min(
                    (columnWidth + root.cellGap) / 4,
                    (Math.max(160, height - root.gridTop - root.panelMargin) + root.cellGap) / tallest))
                readonly property int rows: boxes.reduce((m, b) => Math.max(m, b.y + b.h), 0)
                // The drop's places are measured on this grid
                onColumnsChanged: root.cancelHold()
                onPitchChanged: root.cancelHold()

                function boxLeft(b) { return b.x * (columnWidth + root.cellGap) }
                function boxTop(b) { return b.y * pitch }
                function boxWidth(b) { return b.w * columnWidth + (b.w - 1) * root.cellGap }
                function boxHeight(b) { return Math.max(0, b.h * pitch - root.cellGap) }

                // ---- The held cell (section 4)

                property var dropCenters: []     // the held cell's center at each place
                property size heldSize: Qt.size(0, 0)
                property point grab: Qt.point(0, 0)      // the finger within the cell
                property point pressedAt: Qt.point(0, 0)
                property point finger: Qt.point(0, 0)
                property real dragX: 0
                property real dragY: 0

                function lift(i, scene) {
                    root.commitTyping()
                    const at = grid.mapFromItem(null, scene.x, scene.y)
                    const b = DashboardBook.place(placed, columns)[i]
                    heldSize = Qt.size(boxWidth(b), boxHeight(b))
                    // A press on the stretched part lifts the cell under
                    // the finger at its true size, not past its edge
                    grab = Qt.point(Math.max(0, Math.min(at.x - boxLeft(b), heldSize.width)),
                                    Math.max(0, Math.min(at.y - boxTop(b), heldSize.height)))
                    pressedAt = scene
                    finger = scene
                    dropCenters = DashboardBook.dropBoxes(cells, i, columns).map(
                        d => ({ x: boxLeft(d) + boxWidth(d) / 2, y: boxTop(d) + boxHeight(d) / 2 }))
                    root.held = i
                }
                function follow(scene) {
                    finger = scene
                    if (root.dropAt < 0 && Math.hypot(scene.x - pressedAt.x, scene.y - pressedAt.y)
                            <= Application.styleHints.startDragDistance)
                        return
                    const at = grid.mapFromItem(null, scene.x, scene.y)
                    dragX = at.x - grab.x
                    dragY = at.y - grab.y
                    root.dropAt = DashboardBook.nearest(dropCenters, dragX + heldSize.width / 2,
                                                        dragY + heldSize.height / 2)
                }
                function letGo(canceled) {
                    const from = root.held, to = root.dropAt
                    root.cancelHold()
                    if (canceled)
                        return
                    if (to < 0)
                        root.select(from)
                    else if (root.edit(DashboardBook.moveCell(root.doc, root.pageIndex, from, to),
                                       root.stepOf("", qsTr("moved %1").arg(root.cellName(pageView.cells[from])),
                                                   from, to))
                             && root.selected >= 0)
                        root.selected = DashboardBook.movedIndex(root.selected, from, to)
                }
                // Near the view's top or bottom edge the page scrolls, at
                // up to a quarter of the edge's width per tick
                function scrollAtTheEdge() {
                    const y = mapFromItem(null, finger.x, finger.y).y
                    const edge = Theme.touchTarget
                    const by = y < edge ? y - edge : y > height - edge ? y - (height - edge) : 0
                    if (by === 0)
                        return
                    const bottom = Math.max(0, contentHeight - height)
                    contentY = Math.max(0, Math.min(bottom,
                        contentY + Math.max(-edge, Math.min(edge, by)) / 4))
                    follow(finger)
                }
                Timer {
                    interval: 16
                    repeat: true
                    running: pageView.reflowing
                    onTriggered: pageView.scrollAtTheEdge()
                }

                // A tap between the cells reveals the drive view's bars too
                MouseArea {
                    objectName: "driveTapArea"
                    enabled: root.driving
                    width: pageView.width
                    height: Math.max(pageView.height, pageView.contentHeight)
                    onClicked: root.driveTap()
                }

                // The edit panel, in the page above its grid (never a Popup)
                Loader {
                    id: panelHost
                    objectName: "dashPanel"
                    x: (pageView.width - width) / 2
                    y: root.gridTop
                    width: pageView.gridWidth
                    active: (pageView.shown && root.panel !== "") || root.connectShown
                    sourceComponent: root.panel === "cell" ? cellPanel
                                   : root.panel === "add" ? addPanel
                                   : root.panel === "signal" ? signalPanel
                                   : root.connectShown ? connectPanel : null
                    onLoaded: pageView.contentY = 0
                }

                Label {
                    objectName: "dashEmptyPage"
                    visible: pageView.cells.length === 0 && !root.editing
                    x: root.margin
                    y: root.topGap
                    width: pageView.gridWidth
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    text: qsTr("No gauges on this page. Edit adds them.")
                }

                Item {
                    id: grid
                    // On whole pixels of the display, so the seams snapped inside it are
                    x: root.snap((pageView.width - width) / 2)
                    y: root.snap(swipe.y + root.gridTop + (panelHost.item ? (panelHost.item as Item).height + root.gap : 0)) - swipe.y
                    width: pageView.gridWidth
                    height: Math.max(0, pageView.rows * pageView.pitch - root.cellGap)

                    // The joined panel, under its cells: the flat surface
                    // with its edge, and the hairlines between neighbors
                    Rectangle {
                        objectName: "dashPanelSurface"
                        anchors.fill: parent
                        visible: root.joined && pageView.rows > 0
                        color: Theme.surface
                        radius: root.driving ? 0 : Theme.radiusCard
                        // Under the first tab while it's shown: its sheet runs straight in
                        topLeftRadius: root.stripMeetsPanel && root.pageIndex === 0 ? 0 : radius
                        border.color: Theme.hairline
                        border.width: root.driving ? 0 : root.hairlineWidth
                    }
                    Repeater {
                        model: pageView.seams
                        Rectangle {
                            required property var modelData
                            readonly property real along: modelData.vertical
                                ? modelData.y * pageView.pitch : modelData.x * pageView.columnWidth
                            readonly property real across: root.snap((modelData.vertical
                                ? modelData.x * pageView.columnWidth : modelData.y * pageView.pitch)
                                - root.hairlineWidth / 2)
                            readonly property real run: modelData.length
                                * (modelData.vertical ? pageView.pitch : pageView.columnWidth)
                            objectName: "dashSeam"
                            x: modelData.vertical ? across : along
                            y: modelData.vertical ? along : across
                            width: modelData.vertical ? root.hairlineWidth : run
                            height: modelData.vertical ? run : root.hairlineWidth
                            color: Theme.hairline
                        }
                    }

                    Repeater {
                        model: pageView.cells.length

                        Cell {
                            id: placedCell
                            required property int index
                            readonly property var box: pageView.boxes[pageView.reflowing
                                ? DashboardBook.movedIndex(index, root.held, root.dropAt) : index]
                            readonly property bool carried: pageView.reflowing && root.held === index
                            objectName: "dashCell_" + pageView.index + "_" + index
                            cell: pageView.cells[index]
                            pageNo: pageView.index
                            cellIndex: index
                            later: pageView !== null && Math.abs(pageView.index - root.pageIndex) > 1
                            // qmllint disable missing-property
                            shown: pageView !== null && pageView.index === root.pageIndex
                                   && Qt.application.state !== Qt.ApplicationSuspended
                                   && Qt.application.state !== Qt.ApplicationHidden
                            // qmllint enable missing-property
                            x: carried ? pageView.dragX : box ? pageView.boxLeft(box) : 0
                            y: carried ? pageView.dragY : box ? pageView.boxTop(box) : 0
                            width: box ? pageView.boxWidth(box) : 0
                            height: box ? pageView.boxHeight(box) : 0
                            Behavior on x {
                                enabled: pageView.reflowing && !placedCell.carried
                                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                            }
                            Behavior on y {
                                enabled: pageView.reflowing && !placedCell.carried
                                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                            }
                            onHoldStarted: (scene) => pageView.lift(index, scene)
                            onHoldMoved: (scene) => pageView.follow(scene)
                            onHoldEnded: (canceled) => pageView.letGo(canceled)
                        }
                    }

                    // The "+" cell, split with Paste while a copy exists
                    Item {
                        id: addBox
                        readonly property var box: pageView.boxes[pageView.cells.length]
                        // Off the joined panel's hairlines
                        readonly property real inset: root.joined ? 4 : 0
                        readonly property bool split: root.copied !== null
                        readonly property real half: (width - 8) / 2
                        objectName: "dashAddCellBox_" + pageView.index
                        visible: pageView.addCell
                        x: box ? pageView.boxLeft(box) + inset : 0
                        y: box ? pageView.boxTop(box) + inset : 0
                        width: box ? pageView.boxWidth(box) - 2 * inset : 0
                        height: box ? Math.max(Theme.touchTarget,
                                               pageView.boxHeight(box) - 2 * inset) : 0
                        ThemedButton {
                            objectName: "dashAddCell_" + pageView.index
                            width: addBox.split ? addBox.half : addBox.width
                            height: addBox.height
                            text: "+"
                            font.pixelSize: Theme.fontHero
                            Accessible.name: qsTr("Add a gauge")
                            onClicked: root.startAdd()
                        }
                        ThemedButton {
                            objectName: "dashPasteCell_" + pageView.index
                            visible: addBox.split
                            x: addBox.width - width
                            width: addBox.half
                            height: addBox.height
                            enabled: root.canPaste
                            text: qsTr("Paste")
                            Accessible.name: qsTr("Paste the copied gauge")
                            onClicked: root.pasteAfter(root.shownCells.length - 1)
                        }
                    }
                }
            }
        }
    }

    PageIndicator {
        id: indicator
        objectName: "dashIndicator"
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        visible: root.form === "phone" && root.pages.length > 1 && !root.driving
        count: swipe.count
        currentIndex: swipe.currentIndex
    }

    // The first entries' hint: over the gauges, and it takes no touch
    Rectangle {
        objectName: "driveHint"
        visible: root.driving && root.driveHintUp
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
