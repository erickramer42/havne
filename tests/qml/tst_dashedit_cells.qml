// File/layer: tests/qml/tst_dashedit_cells.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit_cells (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit mode (ADR-0057 section 5), the blank cell, the boxes and the kind change:
// each control's write and what's stored
TestCase {
    id: testCase
    name: "DashboardEditCells"
    when: windowShown
    // The window holds a desktop page whole, so a click lands on it
    width: 1280
    height: 800

    Component { id: pageComponent; DashboardPage { width: 1280; height: 800 } }
    // The page's own store, to put each case's back
    Component {
        id: storeComponent
        Settings {
            category: "dashboards"
            property string phone: ""
            property string desktop: ""
            property int phonePage: 0
            property int desktopPage: 0
            property string phoneUnread: ""
            property string desktopUnread: ""
        }
    }

    // A binding error in the app's QML fails the test. The page's
    // Settings stores a write when the page goes, so the store is put
    // back once the last case's pages are gone, through setValue
    function init() {
        // Each case starts from no session, whatever an earlier one left
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.connected, 5000, "no session at the start")
        wait(0)
        const store = createTemporaryObject(storeComponent, testCase)
        for (const key of ["phone", "desktop", "phoneUnread", "desktopUnread"])
            store.setValue(key, "")
        store.setValue("phonePage", 0)
        store.setValue("desktopPage", 0)
        failOnWarning(/^qrc:\/qt\/qml\/App\//)
        failOnWarning(/^Model size of /)
    }

    function cleanup() {
        DocumentPicker.setPickerForTesting(false)
        Theme.night = "off"
        Theme.look = "dark"
        Bridge.unitSystem = 0
        Bridge.useSignalFiles([])
        for (const p of Bridge.plugins())     // the store starts empty
            Bridge.removePlugin(p.sha256)
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
    }

    function find(within, name) {
        const item = findChild(within, name)
        verify(item, name + " is there")
        return item
    }

    // A page in the window (an item's `visible` is its own there), showing
    // `doc` when one is given
    function pageOf(props, doc) {
        const page = createTemporaryObject(pageComponent, testCase.Window.window.contentItem, props)
        if (doc)
            page.doc = doc
        return page
    }

    // A page showing `doc` with its text stored, as the app has it after an
    // edit, so an undo stores the text it had
    function storedPageOf(props, doc) {
        const page = pageOf(props, doc)
        find(page, "dashStore")[page.form === "phone" ? "phone" : "desktop"] = DashboardBook.serialize(doc)
        return page
    }

    // A one-page document of these cells
    function docOf(cells) {
        const d = DashboardBook.builtIn("desktop")
        d.pages = [{ name: "Test", cells: cells }]
        return d
    }
    // A number by default: a dial is many times slower to build, so a case passes "analog" only where the kind matters
    function cellOf(label, kind) {
        return { kind: kind || "digital", w: 2, h: 4, label: label, refs: ["id:1001"],
                 unit: "RPM", options: { from: 0, to: 8000 } }
    }
    function labels(page) {
        return page.doc.pages[page.pageIndex].cells.map(c => c.label)
    }
    function storedText(page) {
        return find(page, "dashStore")[page.form === "phone" ? "phone" : "desktop"]
    }
    // A press where the item is, once the window is laid out and the item
    // has stopped moving (a page change slides the view). The layout comes
    // first: a row that opens keeps the place it last had, and until the
    // layout pass moves the page below it, the page covers it
    function click(page, name) {
        const item = find(page, name)
        verify(item.visible && item.enabled, name + " can be pressed")
        press(item)
    }
    function press(item) {
        verify(waitForPolish(item.Window.window), item.objectName + " is laid out")
        // A slide's first frame can come after both samples below, with the target still past the window's edge
        for (let up = item.parent; up; up = up.parent) {
            if (up.objectName === "dashPages") {
                const view = up.contentItem
                tryVerify(() => view.currentItem && Math.abs(view.contentX - view.currentItem.x) < 0.5,
                          3000, item.objectName + "'s page has slid in")
            }
        }
        // A cell below a tall panel is scrolled into the page's view first,
        // and the page put back after the press
        let scrolled = null
        let was = 0
        let inSide = false
        // The side column scrolls by itself: bring the item into its view
        for (let up = item.parent; up; up = up.parent) {
            if (up.objectName === "dashSidePanel" && up.visible) {
                inSide = true
                const at = item.mapToItem(up, item.width / 2, item.height / 2)
                if (at.y > up.height - 24 || at.y < 24)
                    up.contentY = Math.max(0, Math.min(up.contentHeight - up.height,
                                                       up.contentY + at.y - up.height / 2))
            }
        }
        for (let up = item.parent; up && !inSide; up = up.parent) {
            if (up.objectName.startsWith("dashPage_")) {
                const at = item.mapToItem(up, item.width / 2, item.height / 2)
                if (at.y > up.height - 24) {
                    was = up.contentY
                    up.contentY = Math.max(0, Math.min(up.contentHeight - up.height,
                                                       up.contentY + at.y - up.height / 2))
                    scrolled = up
                }
            }
        }
        let last = null
        tryVerify(() => {
            const at = item.mapToItem(null, 0, 0)
            const still = last !== null && at.x === last.x && at.y === last.y
            last = at
            return still
        }, 3000, item.objectName + " stops moving")
        mouseClick(item)
        if (scrolled)
            scrolled.contentY = was
    }
    // Text typed into a field and the field left; it emits textEdited, as real typing does
    function type(field, text) {
        field.text = text
        field.textEdited()
        field.editingFinished()
    }
    function spin(box, value) {
        box.value = value
        box.valueModified()
    }

    // Edit shows the bar and each cell's Remove; Done leaves edit mode,
    // and nothing was waiting to be saved
    // The finger down on a cell until the page lifts it, after a short
    // hold unless the case asks for the platform's
    function hold(page, cell, platformHold) {
        tryVerify(() => cell.width > 0, 3000, "laid out")
        verify(waitForPolish(page.Window.window), "laid out")
        if (!platformHold)
            shortHold(cell)
        mousePress(cell)
        tryVerify(() => page.held >= 0, 10000, "the hold completes")
    }
    readonly property int holdMs: Application.styleHints.mousePressAndHoldInterval
    // Set on the held cell alone, just before its press (a reflow can replace
    // the delegate); other cells' taps keep the platform's interval
    readonly property int shortHoldMs: 200
    function shortHold(cell) {
        find(cell, "cellTap").pressAndHoldInterval = shortHoldMs
    }

    // CIE L*a*b* distance (the 1976 formula): how far apart two colors look
    function lab(c) {
        const lin = v => v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
        const r = lin(c.r), g = lin(c.g), b = lin(c.b)
        const x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047
        const y = 0.2126 * r + 0.7152 * g + 0.0722 * b
        const z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883
        const f = t => t > 0.008856 ? Math.cbrt(t) : 7.787 * t + 16 / 116
        return [116 * f(y) - 16, 500 * (f(x) - f(y)), 200 * (f(y) - f(z))]
    }
    function apart(a, b) {
        const p = lab(a), q = lab(b)
        return Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2])
    }

    // ---- Change kind

    function test_changingAKindKeepsTheSignalRangeZonesAndSteps() {
        const own = cellOf("Coolant", "analog")
        own.options = { from: 0, to: 8000, coldBelow: 500, warnFrom: 6000, dangerFrom: 7000,
                        decimals: 1, majorStep: 1000, peak: true }
        const page = storedPageOf({}, docOf([own, cellOf("B")]))
        const before = storedText(page)
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelKind")
        compare(page.panel, "add", "Add's list")
        verify(!findChild(page, "addKind_blank"), "a blank has no signal to keep")
        const own_ = find(page, "addKind_analog")
        verify(own_.checked, "the cell's own kind is marked")
        verify(!find(page, "addKind_ring").checked)
        click(page, "addKind_digital")
        compare(page.panel, "cell", "back to the cell")
        compare(page.selected, 0)
        const c = page.doc.pages[0].cells[0]
        compare(c.kind, "digital")
        compare([c.label, c.unit, c.w, c.h, c.refs], ["Coolant", "RPM", 2, 4, ["id:1001"]])
        compare(c.options, own.options, "zones, range and the dial's step all stay")
        compare(page.doc.pages[0].cells.length, 2, "no cell added")
        click(page, "panelKind")
        click(page, "addKind_analog")
        compare(page.doc.pages[0].cells[0].options.majorStep, 1000, "the way back finds its step")
        click(page, "dashUndo")
        compare(page.doc.pages[0].cells[0].kind, "digital", "one undo is one change")
        compare(find(page, "dashUndo").Accessible.name, "Undo: changed Coolant's kind")
        click(page, "dashUndo")
        compare(page.doc.pages[0].cells[0].kind, "analog")
        compare(storedText(page), before, "and stored as it was")
    }

    function test_thePhonesPanelOffersChangeKindBesideChangeSignal() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, docOf([cellOf("A")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const signal = find(page, "panelSignal")
        const kind = find(page, "panelKind")
        verify(signal.visible && kind.visible, "both buttons")
        verify(kind.height >= Theme.touchTarget && signal.height >= Theme.touchTarget, "a touch's size")
        const a = signal.mapToItem(page, 0, 0)
        const b = kind.mapToItem(page, 0, 0)
        verify(a.x + signal.width <= b.x && b.x + kind.width <= page.width, "side by side, inside the page")
        click(page, "panelKind")
        compare(page.panel, "add")
        click(page, "addKind_ring")
        compare(page.doc.pages[0].cells[0].kind, "ring")
        compare(page.panel, "cell")
    }

    function test_aShiftBandIsNotADialsRange() {
        const dial = cellOf("Tach", "analog")
        dial.options.warnFrom = 6000
        const speed = cellOf("Speed", "analog")
        speed.unit = "mph"
        speed.options = { from: 0, to: 160 }
        const page = pageOf({}, docOf([dial, speed]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelKind")
        click(page, "addKind_shift-light")
        let c = page.doc.pages[0].cells[0]
        compare([c.kind, c.options.from, c.options.to, c.options.warnFrom], ["shift-light", 4500, 6500, 6000],
                "rpm takes the built-in's band, other options carry")
        click(page, "panelKind")
        click(page, "addKind_analog")
        c = page.doc.pages[0].cells[0]
        const back = DashboardBook.defaultRange("RPM")
        compare([c.kind, c.options.from, c.options.to], ["analog", back.from, back.to],
                "leaving takes the signal's default range")
        press(find(page, "dashCell_0_1"))
        click(page, "panelKind")
        click(page, "addKind_shift-light")
        c = page.doc.pages[0].cells[1]
        compare([c.kind, c.options.from, c.options.to], ["shift-light", 0, 160], "another signal keeps its range")
        click(page, "panelKind")
        click(page, "addKind_analog")
        c = page.doc.pages[0].cells[1]
        compare([c.kind, c.options.from, c.options.to], ["analog", 0, 160], "and leaving keeps it too")
    }

    function test_aBlankCellHasNoChangeKind() {
        const page = pageOf({}, docOf([{ kind: "blank", w: 2, h: 4, refs: [] }, cellOf("B")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        verify(!find(page, "panelKind").visible)
        press(find(page, "dashCell_0_1"))
        verify(find(page, "panelKind").visible)
    }

    function test_theKindListBacksOutWithNoChange() {
        const page = pageOf({}, docOf([cellOf("A")]))
        const before = JSON.stringify(page.doc)
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelKind")
        click(page, "addClose")
        compare(page.panel, "cell")
        click(page, "panelKind")
        click(page, "addKind_digital")
        compare(page.panel, "add", "the cell's own kind changes nothing")
        compare(JSON.stringify(page.doc), before)
        page.startAdd()
        verify(findChild(page, "addKind_blank"), "Add still offers a blank")
    }

    // ---- The blank cell

    function test_aBlankCellIsOfferedAndNeedsNoSignal() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        page.startAdd()
        verify(find(page, "preview_blank").item, "its preview loads")
        click(page, "addKind_blank")
        const added = page.doc.pages[0].cells[1]
        compare(added.kind, "blank")
        compare(added.refs, [])
        compare(page.panel, "cell", "straight to its menu, no signal to pick")
        compare(page.selected, 1)
        for (const name of ["panelLabel", "panelSignal", "panelFrom", "panelTo", "panelColdBelow",
                            "panelWarnFrom", "panelDangerFrom", "panelDecimals"])
            verify(!find(page, name).visible, name + " is for a gauge with a signal")
        for (const name of ["panelW", "panelH", "panelCopy", "panelEarlier"])
            verify(find(page, name).visible, name + " is for any cell")
    }

    function test_aBlankCellDrawsNothingOutsideEditMode() {
        const blank = { kind: "blank", w: 2, h: 4, refs: [] }
        const page = pageOf({}, docOf([blank, cellOf("B")]))
        const cell = find(page, "dashCell_0_0")
        tryVerify(() => find(cell, "gauge").item !== null, 3000, "it loads")
        const outline = find(find(cell, "gauge").item, "blankOutline")
        verify(!outline.visible, "no outline outside edit mode")
        compare(String(cell.color), "#00000000", "no card behind it")
        compare(cell.border.width, 0)
        page.startEditing()
        verify(outline.visible, "an outline in edit mode")
        verify(!find(cell, "unknownKind").visible, "it isn't an unknown kind")
        verify(!find(cell, "gauge").item.Accessible.ignored, "announced where it's drawn")
        page.stopEditing()
        verify(find(cell, "gauge").item.Accessible.ignored, "not announced where it isn't")
    }

    // A blank cell has no label: its Remove says what it removes
    function test_aBlankCellsRemoveNamesTheCell() {
        const blank = { kind: "blank", w: 2, h: 4, refs: [] }
        const page = pageOf({}, docOf([blank, cellOf("B")]))
        page.startEditing()
        compare(find(find(page, "dashCell_0_0"), "cellRemove").Accessible.name, "Remove blank cell")
        compare(find(find(page, "dashCell_0_1"), "cellRemove").Accessible.name, "Remove B")
    }

    // Outside edit mode a blank cell has nothing to hold
    function test_aBlankCellIsHeldOnlyInEditMode() {
        const blank = { kind: "blank", w: 2, h: 4, refs: [] }
        const page = pageOf({}, docOf([blank, cellOf("B")]))
        const cell = find(page, "dashCell_0_0")
        tryVerify(() => cell.width > 0, 3000, "laid out")
        verify(waitForPolish(page.Window.window), "laid out")
        mousePress(cell)
        wait(holdMs + 300)   // the platform's interval, as a user's finger meets it
        compare(page.held, -1, "not held outside edit mode")
        mouseRelease(cell)
        compare(page.panel, "")
        page.startEditing()
        hold(page, cell)
        mouseRelease(cell)
        compare(page.panel, "cell")
        compare(page.selected, 0)
    }

    function test_aBlankCellHoldsItsPlaceAndCopies() {
        const blank = { kind: "blank", w: 2, h: 4, refs: [] }
        const page = pageOf({}, docOf([cellOf("A"), blank, cellOf("C")]))
        const a = find(page, "dashCell_0_0")
        const c = find(page, "dashCell_0_2")
        tryVerify(() => c.width > 0, 3000, "laid out")
        verify(c.x >= a.x + a.width + find(page, "dashCell_0_1").width,
               "the next gauge goes past the blank")
        page.select(1)
        page.copySelected()
        page.pasteAfter(1)
        compare(page.doc.pages[0].cells[2], blank, "a blank cell pastes as one")
    }

    // A held peak, low and trace belong to the signal that made them:
    // when two cells of a kind swap signals, each starts again
    function test_aMovedCellDoesNotKeepTheOthersHolds_data() {
        return [
            { tag: "analog", kind: "analog" },
            { tag: "segments", kind: "segments" },
            { tag: "sparkline", kind: "sparkline" },
            { tag: "digital", kind: "digital" }
        ]
    }
    function test_aMovedCellDoesNotKeepTheOthersHolds(data) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const a = cellOf("A", data.kind)
        a.refs = [Bridge.gaugeSignals["coolant"][0]]
        const b = cellOf("B", data.kind)
        b.refs = [Bridge.gaugeSignals["speed"][0]]
        a.options.peak = true
        b.options.peak = true
        const page = pageOf({}, docOf([a, b]))
        page.startEditing()
        const gaugeAt = i => find(find(page, "dashCell_0_" + i), "gauge")
        tryVerify(() => gaugeAt(0).item !== null && gaugeAt(1).item !== null, 3000, "both load")
        const first = gaugeAt(0).item
        first.peakValue = 1e9
        first.lowValue = -1e9
        if (data.kind === "sparkline" || data.kind === "digital")
            first.samples = [0, 1e9]
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        page.moveTo(1)
        compare(labels(page), ["B", "A"])
        const now = gaugeAt(0).item
        verify(now.peakValue !== 1e9, "no peak of A's")
        verify(now.lowValue !== -1e9, "no low of A's")
        if (now.samples !== undefined)
            verify(now.samples.indexOf(1e9) < 0, "no trace of A's")
    }

    // The Close buttons and Done commit a field still holding the focus
    function test_doneCommitsATypedLabel() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const label = find(page, "panelLabel")
        label.forceActiveFocus()
        label.selectAll()
        keyClick("t")
        keyClick("a")
        keyClick("c")
        keyClick("h")
        page.stopEditing()
        compare(labels(page), ["tach"])
    }

    // Standing up moves the bar's default span to the tall one and back,
    // on each form's own grid; a span the user chose stays
    function test_standingUpTakesTheTallSpanAndBack_data() {
        return [{ tag: "desktop", props: {}, flat: [2, 2], tall: [1, 4] },
                { tag: "phone", props: { form: "phone", width: 384, height: 832 },
                  flat: [6, 6], tall: [3, 12] }]
    }
    function test_standingUpTakesTheTallSpanAndBack(data) {
        const bar = cellOf("Level", "bar")
        bar.w = data.flat[0]
        bar.h = data.flat[1]
        const odd = cellOf("Odd", "bar")
        odd.w = 3
        odd.h = 2
        const page = pageOf(data.props, docOf([bar, odd]))
        page.startEditing()
        const at = i => page.doc.pages[0].cells[i]
        page.select(0)
        const vertical = find(page, "panelVertical")
        vertical.toggle()
        vertical.toggled()
        compare([at(0).w, at(0).h], data.tall, "tall")
        compare(at(0).options.vertical, true)
        vertical.toggle()
        vertical.toggled()
        compare([at(0).w, at(0).h], data.flat, "and back")
        page.select(1)
        vertical.toggle()
        vertical.toggled()
        compare([at(1).w, at(1).h], [3, 2], "a chosen span stays")
    }

    // A form's new gauges and the "+" cell take that form's spans
    function test_newCellsTakeTheFormsSpans_data() {
        return [{ tag: "desktop", props: {}, spans: {
                    analog: [1, 4], ring: [1, 4], bar: [2, 2], digital: [1, 4],
                    "shift-light": [12, 1], segments: [2, 3], "seven-segment": [1, 2],
                    sparkline: [2, 3], blank: [1, 4] }, plus: [1, 2] },
                { tag: "phone", props: { form: "phone", width: 384, height: 832 }, spans: {
                    analog: [3, 12], ring: [3, 12], bar: [6, 6], digital: [3, 12],
                    "shift-light": [12, 3], segments: [6, 9], "seven-segment": [3, 6],
                    sparkline: [6, 9], blank: [3, 12] }, plus: [3, 6] }]
    }
    function test_newCellsTakeTheFormsSpans(data) {
        const page = pageOf(data.props, docOf([cellOf("A")]))
        for (const kind in data.spans) {
            const made = page.newCell(kind, { name: "x", ref: "id:1001", decodedUnit: "RPM",
                                              decodedLow: 0, decodedHigh: 8000 })
            compare([made.w, made.h], data.spans[kind], kind)
        }
        page.startEditing()
        const view = find(page, "dashPage_0")
        const plus = find(page, "dashAddCell_0")
        tryVerify(() => view.width > 0 && plus.width > 0, 3000, "laid out")
        compare(view.placed[view.placed.length - 1].w, data.plus[0])
        compare(view.placed[view.placed.length - 1].h, data.plus[1])
        fuzzyCompare(plus.width, data.plus[0] * view.columnWidth
                     + (data.plus[0] - 1) * page.cellGap - (page.joined ? 8 : 0),
                     1e-6, "the + cell is as wide as its span, 4 in from a joined panel's lines")
    }

    // The phone's height control moves in old rows; the desktop's by one to 16
    function test_theHeightControlsStep() {
        const start = cellOf("A")
        start.w = 3
        start.h = 12
        const phone = pageOf({ form: "phone", width: 384, height: 832 }, docOf([start]))
        const h = () => phone.doc.pages[0].cells[0].h
        const setH = v => {
            const d = docOf([cellOf("A")])
            d.pages[0].cells[0].w = 3
            d.pages[0].cells[0].h = v
            phone.doc = d
        }
        phone.startEditing()
        phone.select(0)
        const box = find(phone, "panelH")
        compare(box.to, 48)
        compare(box.stepSize, 3)
        const step = up => {
            if (up) box.increase()
            else box.decrease()
            box.valueModified()
        }
        step(true)
        compare(h(), 15, "up by 3")
        tryCompare(box, "value", 15)
        step(false)
        compare(h(), 12, "and down")
        tryCompare(box, "value", 12)
        // A stored odd h shows as it is and moves to the next multiple
        setH(4)
        tryCompare(box, "value", 4)
        compare(box.from, 3)
        step(false)
        compare(h(), 3)
        tryCompare(box, "value", 3)
        verify(box.value === box.from, "the bottom of the control")
        setH(1)
        tryCompare(box, "value", 1)
        compare(box.from, 1, "an h under the step still shows")
        step(true)
        compare(h(), 3)
        tryCompare(box, "value", 3)
        phone.destroy()

        const desk = pageOf({}, docOf([cellOf("A")]))
        desk.startEditing()
        desk.select(0)
        const deskBox = find(desk, "panelH")
        compare(deskBox.to, 16, "the desktop's control stays as it was")
        compare(deskBox.stepSize, 1)
        compare(deskBox.from, 1)
        spin(deskBox, 5)
        compare(desk.doc.pages[0].cells[0].h, 5)
    }

    // A layout made on 2 and 4 columns keeps every cell's box on 6 and 12,
    // with the page's own gap, and the store is only written by an edit
    function test_aStoredPhoneLayoutKeepsItsBoxes_data() {
        // Drawn heights: a cell runs over the empty squares under it
        return [{ tag: "upright", w: 384, h: 832, before: 2, after: 6, capped: false,
                  heights: [1, 4, 8, 3, 4, 5, 4] },
                { tag: "on its side, with the row's height capped", w: 832, h: 384,
                  before: 4, after: 12, capped: true, heights: [1, 8, 8, 3, 5, 5, 10] }]
    }
    function test_aStoredPhoneLayoutKeepsItsBoxes(data) {
        const rows = (w, h, kind) => ({ kind: kind || "digital", w: w, h: h, label: "c",
                                        refs: ["id:1001"], unit: "RPM", options: { from: 0, to: 100 } })
        const cells = [rows(12, 1, "shift-light"), rows(1, 4), rows(1, 8), rows(2, 3), rows(1, 2),
                       rows(3, 5), rows(1, 4)]
        const text = JSON.stringify({ format: 1, pages: [{ name: "P", cells: cells }] })
        createTemporaryObject(storeComponent, testCase).setValue("phone", text)
        const page = pageOf({ form: "phone", width: data.w, height: data.h })
        const view = find(page, "dashPage_0")
        tryVerify(() => view.width > 0, 3000, "laid out")
        compare(view.columns, data.after)
        // The phone's cells meet, so the columns are the width's shares
        const gap = page.cellGap
        const col = (view.gridWidth - (data.before - 1) * gap) / data.before
        const tallest = cells.reduce((m, c) => Math.max(m, c.h), 1)
        const cap = (Math.max(160, view.height - page.gridTop - page.margin) + gap) / tallest
        const pitch = Math.max(1, Math.min((col + gap) / 4, cap))
        compare(cap < (col + gap) / 4, data.capped, "the case means what it says")
        const was = DashboardBook.place(cells, data.before)
        // Drawn widths: a cell alone in its rows runs to the panel's edge
        const drawn = DashboardBook.stretched(cells, was, data.before)
        if (!data.capped)
            verify(drawn[6].w > was[6].w, "the last cell, alone in its rows, takes the row")
        for (let i = 0; i < cells.length; ++i) {
            const c = find(page, "dashCell_0_" + i)
            const b = was[i]
            tryVerify(() => c.width > 0, 3000, "cell " + i + " laid out")
            fuzzyCompare(c.x, b.x * (col + gap), 1e-6, "cell " + i + " x")
            fuzzyCompare(c.y, b.y * pitch, 1e-6, "cell " + i + " y")
            fuzzyCompare(c.width, drawn[i].w * col + (drawn[i].w - 1) * gap, 1e-6, "cell " + i + " width")
            fuzzyCompare(c.height, data.heights[i] * pitch - gap, 1e-6, "cell " + i + " height")
        }
        compare(page.problem, "", "it reads")
        compare(storedText(page), text, "a read writes nothing")
        verify(page.edit(DashboardBook.renamePage(page.doc, 0, "Q")), "an edit")
        const written = JSON.parse(storedText(page))
        compare(written.format, 2, "the first edit writes format 2")
        compare(written.pages[0].cells[1].h, 12, "with the scaled spans")
    }

    // A digital tile reads its trace only while its page is the one shown
    function test_aDigitalTileOnAnotherPageReadsNothing() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const d = docOf([cellOf("A", "digital")])
        d.pages.push({ name: "P2", cells: [cellOf("B", "digital")] })
        const page = pageOf({}, d)
        const reader = p => find(find(find(page, "dashCell_" + p + "_0"), "gauge").item, "traceReader")
        tryVerify(() => reader(0).running, 3000, "the page shown reads")
        verify(!reader(1).running, "the other page's tile does not")
        page.goTo(1)
        tryVerify(() => reader(1).running && !reader(0).running, 3000, "and it follows the page")
    }

    // Edit mode's Remove sits in a corner the gauge leaves free, so it
    // covers no title
    function test_removeCoversNoTitle() {
        const page = pageOf({}, docOf([cellOf("A", "digital")]))
        const cell = find(page, "dashCell_0_0")
        const gauge = find(cell, "gauge")
        tryVerify(() => gauge.width > 100, 3000, "laid out")
        const before = gauge.width
        page.startEditing()
        const remove = find(cell, "cellRemove")
        tryVerify(() => gauge.width < before, 3000, "the gauge makes room")
        verify(gauge.mapToItem(cell, gauge.width, 0).x <= remove.mapToItem(cell, 0, 0).x + 0.5,
               "the gauge ends where Remove starts")
        page.stopEditing()
        tryCompare(gauge, "width", before, 3000, "and takes the room back")
    }

    // The panel names the span in the grid's units, and Peak hold is a
    // checkbox like every other yes-or-no in the app
    function test_thePanelNamesColumnsRowsAndPeakHold() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        compare(find(page, "panelW").Accessible.name, "Columns")
        compare(find(page, "panelH").Accessible.name, "Rows")
        const peak = find(page, "panelPeak")
        verify(peak.visible)
        compare(peak.text, "Peak hold")
        verify(/^ThemedCheckBox/.test(String(peak)), "a checkbox: " + String(peak))
        verify(peak.height >= Theme.touchTarget - 0.5, "a touch's size: " + peak.height)
    }

    // The Pages row: Reset quiet and apart at its end (wide), or the last
    // of the wrapped controls after a short gap (a phone); the move
    // buttons only with pages to move among
    // The wide page is 1200: under DejaVu (CI's Linux and macOS) the five
    // controls take 856 px of a 968 px row, which leaves Reset no room to
    // stand apart, so it would wrap to a row of its own
    function test_resetStandsApartAndMoveHidesWithOnePage_data() {
        return [{ tag: "wide", form: "desktop", width: 1200, height: 800 },
                { tag: "a phone", form: "phone", width: 384, height: 832 }]
    }
    function test_resetStandsApartAndMoveHidesWithOnePage(data) {
        const page = pageOf({ form: data.form, width: data.width, height: data.height }, docOf([cellOf("A")]))
        page.startEditing()
        click(page, "dashMore")
        const row = find(page, "dashMoreRow")
        const reset = find(page, "moreReset")
        const remove = find(page, "moreRemovePage")
        verify(row.visible)
        verify(!find(page, "moreLeft").visible && !find(page, "moreRight").visible, "nothing to move")
        verify(reset.quiet, "Reset is quiet")
        compare(reset.Accessible.name, "Reset all pages: replace this form's pages with the built-in layout")
        // The row is laid out at the layouts' next polish, after it shows
        tryVerify(() => Math.abs(row.width - (page.width - 2 * page.margin)) < 1, 3000,
                  "the row has the page's width: " + row.width + " of " + page.width + "; its parent "
                  + String(row.parent).replace(/\(0x[0-9a-f]+\)/, "") + " is " + row.parent.width
                  + " wide, x " + row.x + ", implicit " + row.implicitWidth + ", the gap "
                  + find(row, "moreResetGap").width)
        verify(waitForPolish(row), "the row laid out")
        const where = "Reset at " + reset.x + "," + reset.y + ", Remove page ends at "
                      + (remove.x + remove.width) + "," + (remove.y + remove.height) + " in a row of "
                      + row.width
        // Reset peaks follows it when the tool row has no room, which at a
        // phone's width depends on the font's
        const resetPeaks = find(page, "moreResetPeaks")
        const shown = Array.from(row.children).filter(c => c.visible && c !== resetPeaks)
        compare(shown[shown.length - 1], reset, "the last of the row")
        compare(shown[shown.length - 2].objectName, "moreResetGap", "after a gap")
        if (resetPeaks.visible) {
            const all = Array.from(row.children).filter(c => c.visible)
            compare(all[all.length - 1], resetPeaks, "Reset peaks after Reset")
        }
        if (page.width >= 560) {
            fuzzyCompare(reset.x + reset.width, row.width, 0.5, "at the row's end: " + where)
            compare(Math.round(reset.y), Math.round(remove.y), "on the controls' row: " + where)
            verify(reset.x - (remove.x + remove.width) >= 24 - 0.5, "apart from the page's controls: " + where)
        } else {
            verify(reset.y > remove.y - 0.5 || reset.x > remove.x + remove.width + 16 - 0.5,
                   "after the controls, with a short gap: " + where)
        }
        verify(reset.mapToItem(page, reset.width, 0).x <= page.width + 0.5, "inside the page")
        click(page, data.form === "phone" ? "moreAddPage" : "dashPageAdd")
        click(page, "preset_blank")
        verify(find(page, "moreLeft").visible && find(page, "moreRight").visible,
               "two pages: the move buttons show")
    }

    // The cell's panel removes its cell at once, in edit mode and from the
    // hold's menu outside it, and Undo brings it back: the bar's in edit
    // mode, the line's outside it
    function test_thePanelsRemoveIsUndone_data() {
        return [{ tag: "desktop in edit mode", phone: false, edit: true },
                { tag: "phone, the hold's menu", phone: true, edit: false }]
    }
    function test_thePanelsRemoveIsUndone(data) {
        const page = storedPageOf(data.phone ? { form: "phone", width: 384, height: 832 } : {},
                                  docOf([cellOf("A"), cellOf("B"), cellOf("C")]))
        const before = storedText(page)
        const cell = find(page, "dashCell_0_1")
        if (data.edit) {
            page.startEditing()
            press(cell)
        } else {
            hold(page, cell)
            mouseRelease(cell)
        }
        compare(page.panel, "cell")
        const remove = find(page, "panelRemove")
        compare(remove.Accessible.name, "Remove B")
        verify(!findChild(page, "panelRemoveAsk"), "no question")
        click(page, "panelRemove")
        compare(labels(page), ["A", "C"])
        compare(page.panel, "", "the panel goes with its cell")
        compare(page.selected, -1)
        compare(page.editing, data.edit, "edit mode is as it was")
        compare(storedText(page), DashboardBook.serialize(page.doc), "and it's stored")
        click(page, data.edit ? "dashUndo" : "dashUndoLineUndo")
        compare(labels(page), ["A", "B", "C"], "back")
        compare(storedText(page), before, "and stored as it was")
    }

    // The Undo line outside edit mode is the only way back from a panel's
    // Remove and hides after seconds, so its button is a touch's size
    function test_theUndoLinesButtonIsATouchTarget() {
        Theme.compact = true
        try {
            const page = storedPageOf({ form: "phone", width: 384, height: 832 },
                                      docOf([cellOf("A"), cellOf("B")]))
            hold(page, find(page, "dashCell_0_1"))
            mouseRelease(find(page, "dashCell_0_1"))
            find(page, "panelRemove").clicked()   // a press can miss a compact panel's foot
            const undo = find(page, "dashUndoLineUndo")
            tryVerify(() => undo.visible, 2000, "the line offers Undo")
            verify(undo.height >= Theme.touchTarget,
                   "Undo is " + undo.height + " tall, a touch is " + Theme.touchTarget)
        } finally {
            Theme.compact = false
        }
    }

    // A cell with no label is named by its kind, on Remove and in the step's words
    function test_aCellWithNoLabelIsNamedByItsKind() {
        const page = storedPageOf({}, docOf([cellOf("A"), cellOf("", "blank"), cellOf("C")]))
        page.startEditing()
        press(find(page, "dashCell_0_1"))
        compare(find(page, "panelRemove").Accessible.name, "Remove Blank")
        click(page, "panelRemove")
        compare(find(page, "dashUndo").Accessible.name, "Undo: removed Blank")
    }

    // Edit mode's Remove is a touch's size and draws a raised square with
    // an edge inside its cell, a short cell's too (the phone's touch size,
    // which Theme.compact gives outside Main)
    function test_theRemoveBoxReadsAsAButton() {
        Theme.compact = true
        try {
            removeBoxOnThePhone()
        } finally {
            Theme.compact = false
        }
    }
    function removeBoxOnThePhone() {
        const lights = { kind: "shift-light", w: 12, h: 3, label: "Shift", refs: ["id:1001"],
                         unit: "RPM", options: { from: 0, to: 8000 } }
        const page = pageOf({ form: "phone", width: 384, height: 832 },
                            docOf([lights, cellOf("B")]))
        page.startEditing()
        for (const name of ["dashCell_0_0", "dashCell_0_1"]) {
            const cell = find(page, name)
            const box = find(cell, "cellRemove")
            const face = find(box, "cellRemoveFace")
            tryVerify(() => cell.height > 0 && face.width > 0, 3000, name + " laid out")
            verify(box.width >= Theme.touchTarget - 0.5 && box.height >= Theme.touchTarget - 0.5,
                   name + "'s Remove is a touch's size: " + box.width + " x " + box.height)
            compare(face.border.width, 1, name + "'s face has an edge")
            verify(Qt.colorEqual(face.border.color, Theme.outline), "in the outline's color")
            verify(Qt.colorEqual(face.color, Theme.elevated), "on a control's fill")
            const top = face.mapToItem(cell, 0, 0)
            const end = face.mapToItem(cell, face.width, face.height)
            verify(top.x >= 0 && top.y >= 0 && end.x <= cell.width + 0.5 && end.y <= cell.height + 0.5,
                   name + "'s face is inside its cell: " + JSON.stringify([top, end, cell.width, cell.height]))
        }
        verify(find(page, "dashCell_0_0").height < Theme.touchTarget,
               "the lights are shorter than a touch: " + find(page, "dashCell_0_0").height)
    }

    // The phone has no strip: its Page row keeps Add page
    function test_thePhoneKeepsAddPageInTheRow() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, docOf([cellOf("A")]))
        page.startEditing()
        verify(!findChild(page, "dashPageStrip"), "no strip on the phone")
        compare(find(page, "dashMore").text, "Page")
        click(page, "dashMore")
        verify(find(page, "moreAddPage").visible, "Add page in the row")
        click(page, "moreAddPage")
        click(page, "preset_blank")
        compare(page.doc.pages.length, 2)
        compare(page.pageIndex, 1, "the new page shows")
    }
}
