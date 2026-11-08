// File/layer: app/qml/DashboardPage.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboard, QmlTest.dashedit, QmlTest.phone,
//          QmlTest.linkhealth (test_qml); Manual smoke

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
    readonly property real gap: compact ? 10 : 16

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
    }
    // The last URI that connected (SettingsPage's), whose profile's
    // signals Add offers when there's no session
    Settings {
        id: sessionStore
        category: "session"
        property string uri: ""
    }

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
    // Done only leaves edit mode and nothing waits to be saved

    property bool editing: false
    property int selected: -1        // the cell edited, on the page shown
    property string panel: ""        // "", "cell", "add", "signal"
    property string addKind: ""      // Add's kind; "" when changing a cell's signal
    property bool moreShown: false
    property bool confirmReset: false
    property bool confirmRemovePage: false

    // The kinds this page draws, in the order Add offers them, and the
    // component that makes each (a cell's and Add's preview); null for
    // a kind this version doesn't draw
    readonly property var drawnKinds: ["analog", "ring", "bar", "digital", "shift-light"]
    function kindComponent(kind) {
        return kind === "analog" ? analogKind
             : kind === "ring" ? ringKind
             : kind === "bar" ? barKind
             : kind === "digital" ? digitalKind
             : kind === "shift-light" ? shiftKind
             : null
    }
    // The kinds that draw a peak, so the panel offers Peak hold only there
    readonly property var peakKinds: ["analog"]
    readonly property var kindNames: ({
        "analog": qsTr("Dial"), "ring": qsTr("Ring"), "bar": qsTr("Bar"),
        "digital": qsTr("Number"), "shift-light": qsTr("Shift lights")
    })
    // A new gauge's span: columns, and rows of a quarter column
    readonly property var newSpans: ({
        "analog": [1, 4], "ring": [1, 4], "bar": [2, 2], "digital": [1, 4],
        "shift-light": [12, 1]
    })

    readonly property string formKey: form === "phone" ? "phone" : "desktop"
    readonly property var shownCells: pages[pageIndex] && Array.isArray(pages[pageIndex].cells)
                                      ? pages[pageIndex].cells : []
    readonly property var selectedCell: editing && selected >= 0 && selected < shownCells.length
                                        ? shownCells[selected] : null
    onSelectedCellChanged: if (selectedCell === null && panel === "cell") panel = ""

    function startEditing() {
        editing = true
    }
    function stopEditing() {
        editing = false
        selected = -1
        panel = ""
        moreShown = false
        confirmReset = false
        confirmRemovePage = false
    }

    // Takes the focus off a text field, so the field commits what was
    // typed to the cell or page it was typed for before the selection or
    // the page changes (a tap on a cell and a swipe take no focus). Its
    // fields exist only in edit mode, and outside it the focus is left
    // where it is. The field's own focus goes: the page is a focus scope,
    // so focusing the page would give it back to the field
    function commitTyping() {
        if (!editing)
            return
        const f = Window.activeFocusItem
        if (f !== null && (f instanceof TextInput || f instanceof TextEdit))
            f.focus = false
        Qt.inputMethod.hide()
    }

    // Stores next as this form's document, unless it's the same (a
    // refused write gives an equal copy). The first edit after a stored
    // text that couldn't be used keeps that text in <form>Unread
    function edit(next) {
        if (DashboardBook.serialize(next) === DashboardBook.serialize(doc))
            return false
        const texts = DashboardBook.saveTexts(problem, store[formKey], next)
        if (texts.unread !== "")
            store[formKey + "Unread"] = texts.unread
        store[formKey] = texts.text
        doc = next
        return true
    }

    // Back to the built-in layout: nothing stored, so a later version's
    // built-in layout reaches it
    function resetForm() {
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

    // ---- Add: a kind, then a signal

    property var profileRows: []
    function refreshRows() {
        profileRows = Bridge.profileSignals(Bridge.connected ? Bridge.currentUri : sessionStore.uri)
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
            if (edit(DashboardBook.addCell(doc, pageIndex, newCell(addKind, row)))) {
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
        edit(next)
        panel = "cell"
    }
    Connections {
        target: Bridge
        enabled: root.panel === "signal"
        function onConnectedChanged() { root.refreshRows() }
        function onUnitSystemChanged() { root.refreshRows() }
    }

    // ---- The cell's panel

    function setOptions(changes) {
        return edit(DashboardBook.setOptions(doc, pageIndex, selected, changes))
    }
    function setLabel(text) {
        return edit(DashboardBook.setCell(doc, pageIndex, selected,
                                          { label: text === "" ? null : text }))
    }
    function resize(w, h) {
        return edit(DashboardBook.resizeCell(doc, pageIndex, selected, w, h))
    }
    // Earlier, later, to the start and to the end: the cell's place in
    // the order, which both forms' grids follow
    function moveTo(to) {
        const t = Math.max(0, Math.min(shownCells.length - 1, to))
        if (edit(DashboardBook.moveCell(doc, pageIndex, selected, t)))
            selected = t
    }
    function removeCell(i) {
        if (!edit(DashboardBook.removeCell(doc, pageIndex, i)))
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
        if (edit(DashboardBook.addPage(doc, qsTr("Page %1").arg(pages.length + 1))))
            goTo(pages.length - 1)
    }
    function movePage(by) {
        const to = pageIndex + by
        if (edit(DashboardBook.movePage(doc, pageIndex, to)))
            goTo(to)
    }
    // Another page takes this one's index, so nothing stays selected
    function removePage() {
        confirmRemovePage = false
        selected = -1
        panel = ""
        addKind = ""
        edit(DashboardBook.removePage(doc, pageIndex))
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
        selected = -1
        panel = ""
        addKind = ""
        if (swipe.count > pageIndex && swipe.currentIndex !== pageIndex)
            swipe.setCurrentIndex(pageIndex)
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
    onVisibleChanged: maybeSweep()
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
        property int pageNo: -1
        property int cellIndex: -1
        readonly property bool editable: root.editing && pageNo === root.pageIndex

        // The sweep plays on the dials and the shift lights only
        function sweep() {
            if (gauge.item && kind !== "bar" && kind !== "digital")
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
            else if (entry.hidden) words = qsTr("not supported by this car")
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
        readonly property color accent: {
            const a = opt("accent", "accent")
            return a === "info" ? Theme.info : a === "success" ? Theme.success
                                             : Qt.lighter(Theme.primary, 1.3)
        }

        // Each kind's own
        readonly property real labelDivisor: opt("labelDivisor", 1)
        readonly property string caption: opt("caption", unit)
        readonly property string peakLabel: opt("peakLabel", qsTr("PEAK"))
        readonly property bool showPeak: opt("peak", false)
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
                                       "coldBelow", "warnFrom", "dangerFrom", "accent"]
        readonly property var own: ({
            "analog": ["majorStep", "minorStep", "labelDivisor", "caption", "peakLabel",
                       "showPeak", "showProgress"],
            "bar": ["scaleMarks"],
            "digital": ["description", "valueLabel"],
            "shift-light": ["count", "ledSpacing"]
        })

        Loader {
            id: gauge
            objectName: "gauge"
            anchors.fill: parent
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

        // In edit mode a tap opens the cell's panel; on the phone a long
        // press starts edit mode at the cell
        TapHandler {
            objectName: "cellTap"
            onTapped: if (card.editable) root.select(card.cellIndex)
            onLongPressed: {
                if (root.form !== "phone" || card.pageNo !== root.pageIndex)
                    return
                root.startEditing()
                root.select(card.cellIndex)
            }
        }
        // The outline and Remove exist only while the cell is editable
        Loader {
            z: 2
            active: card.editable && root.selected === card.cellIndex
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
                objectName: "cellRemove"
                compact: true
                text: "\u00D7"
                Accessible.name: qsTr("Remove %1").arg(card.label)
                onClicked: root.removeCell(card.cellIndex)
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
            font.pixelSize: 13
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
        font.pixelSize: 13
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
                        font.pixelSize: 15
                        font.bold: true
                        color: Theme.text
                        text: (root.kindNames[panelCard.kind] ?? panelCard.kind) + ": "
                              + (panelCard.cell && panelCard.cell.label ? panelCard.cell.label : "")
                    }
                    ThemedButton {
                        objectName: "panelClose"
                        text: qsTr("Close")
                        onClicked: root.panel = ""
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: panelCard.phone ? 2 : 4
                    columnSpacing: 8
                    rowSpacing: 8

                    Caption { text: qsTr("Label") }
                    TextField {
                        id: labelField
                        objectName: "panelLabel"
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
                    Caption { text: qsTr("Signal") }
                    ThemedButton {
                        objectName: "panelSignal"
                        Layout.fillWidth: true
                        text: qsTr("Change signal")
                        onClicked: root.changeSignal()
                    }

                    Caption { text: qsTr("From (%1)").arg(panelCard.unitShown) }
                    OptionField {
                        objectName: "panelFrom"
                        option: "from"
                        clearable: false
                        fallback: panelCard.range.from
                    }
                    Caption { text: qsTr("To (%1)").arg(panelCard.unitShown) }
                    OptionField {
                        objectName: "panelTo"
                        option: "to"
                        clearable: false
                        fallback: panelCard.range.to
                    }

                    Caption { text: qsTr("Cold below") }
                    OptionField { objectName: "panelColdBelow"; option: "coldBelow" }
                    Caption { text: qsTr("Warning from") }
                    OptionField { objectName: "panelWarnFrom"; option: "warnFrom" }
                    Caption { text: qsTr("Danger from") }
                    OptionField { objectName: "panelDangerFrom"; option: "dangerFrom" }

                    Caption { text: qsTr("Decimals") }
                    SpinBox {
                        objectName: "panelDecimals"
                        Layout.preferredHeight: Theme.touchTarget
                        from: 0
                        to: 6
                        value: panelCard.cell && panelCard.cell.options
                               && panelCard.cell.options.decimals !== undefined
                               ? panelCard.cell.options.decimals
                               : panelCard.entry ? panelCard.entry.decimals : 0
                        onValueModified: root.setOptions({ decimals: value })
                    }

                    Caption { text: qsTr("Width") }
                    SpinBox {
                        objectName: "panelW"
                        Layout.preferredHeight: Theme.touchTarget
                        from: 1
                        to: DashboardBook.maxColumns
                        value: panelCard.cell ? panelCard.cell.w : 1
                        onValueModified: root.resize(value, panelCard.cell.h)
                    }
                    Caption { text: qsTr("Height") }
                    SpinBox {
                        objectName: "panelH"
                        Layout.preferredHeight: Theme.touchTarget
                        from: 1
                        to: DashboardBook.maxRows
                        value: panelCard.cell ? panelCard.cell.h : 1
                        onValueModified: root.resize(panelCard.cell.w, value)
                    }

                    Caption {
                        visible: root.peakKinds.indexOf(panelCard.kind) >= 0
                        text: qsTr("Peak hold")
                    }
                    Switch {
                        objectName: "panelPeak"
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
                    Caption { visible: panelCard.kind === "shift-light"; text: qsTr("Lights") }
                    SpinBox {
                        objectName: "panelCount"
                        visible: panelCard.kind === "shift-light"
                        Layout.preferredHeight: Theme.touchTarget
                        from: 1
                        to: 32
                        value: panelCard.cell && panelCard.cell.options
                               && panelCard.cell.options.count !== undefined
                               ? panelCard.cell.options.count : 15
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
                        font.pixelSize: 15
                        font.bold: true
                        color: Theme.text
                        text: qsTr("Add a gauge: its kind")
                    }
                    ThemedButton {
                        objectName: "addClose"
                        text: qsTr("Close")
                        onClicked: root.panel = ""
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
                            readonly property bool wide: modelData === "bar" || modelData === "shift-light"
                            spacing: 4
                            Loader {
                                objectName: "preview_" + kindTile.modelData
                                Layout.preferredWidth: kindTile.wide ? 200 : 120
                                Layout.preferredHeight: kindTile.modelData === "shift-light" ? 40
                                                      : kindTile.wide ? 70 : 120
                                sourceComponent: root.kindComponent(kindTile.modelData)
                                // Still: 60 percent of a 0 to 100 range, never live
                                onLoaded: {
                                    const g = item as GaugeBase
                                    g.from = 0
                                    g.to = 100
                                    g.hasValue = true
                                    g.value = 60
                                    g.label = root.kindNames[kindTile.modelData]
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
    // rows with references, or those of the last URI that connected
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
                        font.pixelSize: 15
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

    Component { id: analogKind; AnalogGauge {} }
    Component { id: ringKind; RingGauge {} }
    Component { id: barKind; BarGauge {} }
    Component { id: digitalKind; DigitalGauge {} }
    Component { id: shiftKind; ShiftLightBar {} }

    // The link's dot and words
    Component {
        id: linkState
        RowLayout {
            spacing: 10
            Rectangle {
                objectName: "linkDot"
                implicitWidth: 8; implicitHeight: 8   // layout-managed
                radius: 4
                color: Theme.linkColor(Bridge.linkState)
            }
            Label {
                objectName: "linkLabel"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                // Short statuses come from Theme.linkLabel; "ok" keeps
                // the richer URI text. Long-form detail (episode
                // counts, error codes) stays in LiveView's banner.
                text: Bridge.linkState === "ok"
                      ? qsTr("Live · %1").arg(Bridge.uriLabel)
                      : Theme.linkLabel(Bridge.linkState)
                font.pixelSize: 13
                color: Theme.linkColor(Bridge.linkState)
            }
        }
    }

    // ---- The header: title, link state, peak reset; the desktop's
    // page buttons; why the stored layout isn't shown

    ColumnLayout {
        id: topBar
        x: (root.width - width) / 2
        y: root.margin
        width: Math.max(0, root.width - 2 * root.margin)
        spacing: root.gap / 2

        PageHeader {
            id: header
            Layout.fillWidth: true
            title: qsTr("Dashboard")
            // At a narrow width the row's buttons are compact, the link
            // state gives way first and the title last, so the row fits
            // whatever the font's widths
            titleShrinks: true
            // Reset peaks and Edit at a phone's touch size
            readonly property real touchMin: Theme.touchTarget

            // The desktop's link state follows the title; the phone's
            // row has no room for it beside Reset peaks and Edit, so it
            // takes a line of its own below. No width of its own: it
            // takes what the title and buttons leave
            Loader {
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                active: root.form !== "phone"
                visible: active
                sourceComponent: linkState
            }
            Item {
                Layout.fillWidth: true
                visible: root.form === "phone"
            }
            ThemedButton {
                objectName: "resetPeaks"
                compact: root.compact
                text: qsTr("Reset peaks")
                onClicked: root.resetPeaks()
            }
            ThemedButton {
                objectName: "dashEdit"
                compact: root.compact
                visible: !root.editing
                text: qsTr("Edit")
                onClicked: root.startEditing()
            }
        }

        Loader {
            Layout.fillWidth: true
            active: root.form === "phone"
            visible: active
            sourceComponent: linkState
        }

        // Edit mode's bar: three buttons, compact on the phone, so its
        // row holds them
        RowLayout {
            objectName: "dashEditBar"
            readonly property real touchMin: Theme.touchTarget
            Layout.fillWidth: true
            visible: root.editing
            spacing: 8
            ThemedButton {
                objectName: "dashDone"
                compact: root.form === "phone"
                highlighted: true
                text: qsTr("Done")
                onClicked: root.stopEditing()
            }
            ThemedButton {
                objectName: "dashAdd"
                compact: root.form === "phone"
                enabled: DashboardBook.canAddCell(root.doc, root.pageIndex)
                text: qsTr("Add gauge")
                onClicked: root.startAdd()
            }
            ThemedButton {
                objectName: "dashMore"
                compact: root.form === "phone"
                highlighted: root.moreShown
                text: qsTr("Pages")
                onClicked: {
                    root.moreShown = !root.moreShown
                    root.confirmReset = false
                    root.confirmRemovePage = false
                }
            }
            Item { Layout.fillWidth: true }
        }

        // The pages' controls and Reset, in the page (never a menu)
        Flow {
            objectName: "dashMoreRow"
            readonly property real touchMin: Theme.touchTarget
            Layout.fillWidth: true
            visible: root.editing && root.moreShown && !root.confirmReset && !root.confirmRemovePage
            spacing: 8
            TextField {
                id: pageName
                objectName: "moreRename"
                width: Math.min(220, parent.width)
                height: Theme.touchTarget
                maximumLength: DashboardBook.maxLabel
                text: root.pages[root.pageIndex] ? root.pages[root.pageIndex].name : ""
                placeholderText: qsTr("Page name")
                Accessible.name: qsTr("This page's name")
                onEditingFinished: {
                    root.edit(DashboardBook.renamePage(root.doc, root.pageIndex, text))
                    text = Qt.binding(() => root.pages[root.pageIndex]
                                            ? root.pages[root.pageIndex].name : "")
                }
            }
            ThemedButton {
                objectName: "moreAddPage"
                enabled: DashboardBook.canAddPage(root.doc)
                text: qsTr("Add page")
                onClicked: root.addPage()
            }
            ThemedButton {
                objectName: "moreLeft"
                enabled: root.pageIndex > 0
                text: qsTr("Move page left")
                onClicked: root.movePage(-1)
            }
            ThemedButton {
                objectName: "moreRight"
                enabled: root.pageIndex < root.pages.length - 1
                text: qsTr("Move page right")
                onClicked: root.movePage(1)
            }
            ThemedButton {
                objectName: "moreRemovePage"
                enabled: DashboardBook.canRemovePage(root.doc)
                text: qsTr("Remove page")
                onClicked: root.confirmRemovePage = true
            }
            ThemedButton {
                objectName: "moreReset"
                text: qsTr("Reset")
                onClicked: root.confirmReset = true
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
                font.pixelSize: 13
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
                font.pixelSize: 13
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

        Flow {
            objectName: "dashPageButtons"
            Layout.fillWidth: true
            visible: root.form === "desktop" && root.pages.length > 1
            spacing: 6
            Repeater {
                model: root.pages.length
                ThemedButton {
                    required property int index
                    objectName: "dashPageButton_" + index
                    compact: true
                    highlighted: index === root.pageIndex
                    text: root.pages[index] && root.pages[index].name
                          ? root.pages[index].name : qsTr("Page %1").arg(index + 1)
                    onClicked: root.goTo(index)
                }
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
                font.pixelSize: 13
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
    }

    // ---- The pages: one SwipeView, swiped on the phone form, moved by
    // the buttons and keys on the desktop form

    SwipeView {
        id: swipe
        objectName: "dashPages"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: indicator.visible ? indicator.top : parent.bottom
        clip: true
        interactive: root.form === "phone"
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
                contentHeight: grid.y + grid.height + root.margin
                // Shown whenever there's more below, not only while
                // scrolling, so a short window says it scrolls
                ScrollBar.vertical: ScrollBar {
                    policy: pageView.contentHeight > pageView.height ? ScrollBar.AlwaysOn
                                                                     : ScrollBar.AsNeeded
                }

                // The grid (section 4): the phone's 2 columns upright and
                // 4 on its side, the desktop's one per 220 px from 2 to 12
                readonly property real gridWidth: Math.max(0, width - 2 * root.margin)
                readonly property int columns: root.form === "phone"
                    ? (root.width > root.height ? 4 : 2)
                    : Math.max(2, Math.min(12, Math.round(gridWidth / 220)))
                readonly property real columnWidth: Math.max(0, (gridWidth - (columns - 1)
                                                                 * root.gap) / columns)
                readonly property bool shown: index === root.pageIndex
                // In edit mode the page ends with a "+" cell while it has room
                readonly property bool addCell: root.editing && shown
                                                && DashboardBook.canAddCell(root.doc, index)
                readonly property var placed: addCell ? cells.concat([{ w: 1, h: 2 }]) : cells
                readonly property var boxes: DashboardBook.place(placed, columns)
                readonly property int tallest: placed.reduce((m, c) => Math.max(m, c.h), 1)
                // The row unit is a quarter of a column, so h = 4w is
                // square; never so tall that the tallest cell overflows
                // what the page shows, down to a floor of 160 px, below
                // which the page scrolls instead
                readonly property real pitch: Math.max(1, Math.min(
                    (columnWidth + root.gap) / 4,
                    (Math.max(160, height - 2 * root.margin) + root.gap) / tallest))
                readonly property int rows: boxes.reduce((m, b) => Math.max(m, b.y + b.h), 0)

                // The edit panel, in the page above its grid (never a Popup)
                Loader {
                    id: panelHost
                    objectName: "dashPanel"
                    x: (pageView.width - width) / 2
                    y: root.margin
                    width: pageView.gridWidth
                    active: root.editing && pageView.shown && root.panel !== ""
                    sourceComponent: root.panel === "cell" ? cellPanel
                                   : root.panel === "add" ? addPanel
                                   : root.panel === "signal" ? signalPanel : null
                    onLoaded: pageView.contentY = 0
                }

                Label {
                    objectName: "dashEmptyPage"
                    visible: pageView.cells.length === 0 && !root.editing
                    x: root.margin
                    y: root.margin
                    width: pageView.gridWidth
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    text: qsTr("No gauges on this page. Edit adds them.")
                }

                Item {
                    id: grid
                    x: (pageView.width - width) / 2
                    y: root.margin + (panelHost.item ? (panelHost.item as Item).height + root.gap : 0)
                    width: pageView.gridWidth
                    height: Math.max(0, pageView.rows * pageView.pitch - root.gap)

                    Repeater {
                        model: pageView.cells.length

                        Cell {
                            required property int index
                            readonly property var box: pageView.boxes[index]
                            objectName: "dashCell_" + pageView.index + "_" + index
                            cell: pageView.cells[index]
                            pageNo: pageView.index
                            cellIndex: index
                            later: Math.abs(pageView.index - root.pageIndex) > 1
                            x: box ? box.x * (pageView.columnWidth + root.gap) : 0
                            y: box ? box.y * pageView.pitch : 0
                            width: box ? box.w * pageView.columnWidth + (box.w - 1) * root.gap : 0
                            height: box ? Math.max(0, box.h * pageView.pitch - root.gap) : 0
                        }
                    }

                    ThemedButton {
                        readonly property var box: pageView.boxes[pageView.cells.length]
                        objectName: "dashAddCell_" + pageView.index
                        visible: pageView.addCell
                        x: box ? box.x * (pageView.columnWidth + root.gap) : 0
                        y: box ? box.y * pageView.pitch : 0
                        width: box ? pageView.columnWidth : 0
                        height: box ? Math.max(Theme.touchTarget, box.h * pageView.pitch - root.gap) : 0
                        text: "+"
                        font.pixelSize: 28
                        Accessible.name: qsTr("Add a gauge")
                        onClicked: root.startAdd()
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
        visible: root.form === "phone" && root.pages.length > 1
        count: swipe.count
        currentIndex: swipe.currentIndex
    }
}
