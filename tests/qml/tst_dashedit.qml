// File/layer: tests/qml/tst_dashedit.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit mode (ADR-0057 section 5): each control's write,
// what's stored, and a layout built through them surviving a restart
TestCase {
    id: testCase
    name: "DashboardEdit"
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
        Bridge.unitSystem = 0
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
        let last = null
        tryVerify(() => {
            const at = item.mapToItem(null, 0, 0)
            const still = last !== null && at.x === last.x && at.y === last.y
            last = at
            return still
        }, 3000, item.objectName + " stops moving")
        mouseClick(item)
    }
    // Text typed into a field and the field left
    function type(field, text) {
        field.text = text
        field.editingFinished()
    }
    function spin(box, value) {
        box.value = value
        box.valueModified()
    }

    // Edit shows the bar and each cell's Remove; Done leaves edit mode,
    // and nothing was waiting to be saved
    function test_editAndDone() {
        // B's dial shows its peak, so its gauge takes taps too
        const peaked = cellOf("B", "analog")
        peaked.options.peak = true
        const page = pageOf({}, docOf([cellOf("A"), peaked]))
        const bar = find(page, "dashEditBar")
        verify(!bar.visible, "no bar outside edit mode")
        verify(!findChild(find(page, "dashCell_0_0"), "cellRemove"), "no Remove outside edit mode")
        verify(!find(page, "dashAddCell_0").visible, "no + cell outside edit mode")
        click(page, "dashEdit")
        verify(page.editing)
        verify(bar.visible)
        verify(!find(page, "dashEdit").visible, "Edit hides while editing")
        verify(find(find(page, "dashCell_0_1"), "cellRemove").visible)
        verify(find(page, "dashAddCell_0").visible, "the page ends with a + cell")
        // A tap selects; another switches the panel to it
        press(find(page, "dashCell_0_1"))
        compare(page.selected, 1)
        compare(page.panel, "cell")
        verify(find(find(page, "dashCell_0_1"), "cellSelected").visible, "outlined")
        verify(!findChild(find(page, "dashCell_0_0"), "cellSelected"))
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        click(page, "panelClose")
        compare(page.panel, "")
        click(page, "dashDone")
        verify(!page.editing)
        verify(!bar.visible)
        compare(page.selected, -1)
        tryVerify(() => !findChild(find(page, "dashCell_0_1"), "cellRemove"), 3000,
                  "Remove goes with edit mode")
        compare(storedText(page), "", "no edit, nothing stored")
        // Outside edit mode a tap selects nothing
        press(find(page, "dashCell_0_0"))
        compare(page.selected, -1)
        compare(page.panel, "")
    }

    // Only the page shown is edited: another page's cells show no Remove
    // and no outline at the selected index
    function test_onlyTheShownPageIsEdited() {
        const two = docOf([cellOf("A")])
        two.pages.push({ name: "More", cells: [cellOf("B")] })
        const page = pageOf({}, two)
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        const other = find(page, "dashCell_1_0")
        verify(find(find(page, "dashCell_0_0"), "cellRemove").visible)
        verify(!findChild(other, "cellRemove"), "no Remove off the page shown")
        verify(!findChild(other, "cellSelected"), "no outline off the page shown")
    }

    // Add: a kind with its still preview, then a signal of the session's;
    // the cell stores the signal's decoded unit and its default range
    // (DashboardBook.defaultRange) in any unit system, with its
    // counterpart's ref
    function test_addTakesTheDecodedUnitAndRange_data() {
        return [{ tag: "metric", units: 0, from: "0", to: "150" },
                { tag: "imperial", units: 1, from: "32", to: "302" }]
    }
    function test_addTakesTheDecodedUnitAndRange(data) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        Bridge.unitSystem = data.units
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        click(page, "dashEdit")
        click(page, "dashAdd")
        compare(page.panel, "add")
        const kinds = find(page, "dashAddPanel")
        for (const kind of page.drawnKinds) {
            find(kinds, "addKind_" + kind)
            const preview = find(kinds, "preview_" + kind)
            tryCompare(preview, "status", Loader.Ready)
            compare(preview.item.value, 60, kind + ": still at 60 percent")
            compare(preview.item.from, 0)
            compare(preview.item.to, 100)
            verify(!preview.item.sweeping, kind + ": no sweep")
        }
        click(page, "addKind_analog")
        compare(page.panel, "signal")
        const coolant = Bridge.gaugeSignals["coolant"]
        const panel = find(page, "dashSignalPanel")
        verify(!findChild(panel, "pickSignal_"), "no row without a ref")
        click(page, "pickSignal_" + coolant[0])

        const cells = page.doc.pages[0].cells
        compare(cells.length, 2)
        const made = cells[1]
        compare(made.kind, "analog")
        compare(made.w, 1)
        compare(made.h, 4)
        compare(made.label, "Coolant Temp")
        compare(made.unit, "°C", "the decoded unit")
        compare(JSON.stringify(made.refs), JSON.stringify(coolant), "with its counterpart")
        compare(made.options.from, 0, "the unit's default range")
        compare(made.options.to, 150)
        verify(made.options.decimals === undefined, "decimals follow the row")
        verify(made.options.warnFrom === undefined, "no zones")
        compare(page.selected, 1, "the new cell is selected")
        compare(page.panel, "cell")
        compare(storedText(page), DashboardBook.serialize(page.doc), "stored as it's made")

        // The panel shows the range in the unit shown, and stores what's
        // typed in the decoded unit
        compare(find(page, "panelFrom").text, data.from)
        compare(find(page, "panelTo").text, data.to)
        type(find(page, "panelTo"), data.units === 1 ? "212" : "100")
        compare(page.doc.pages[0].cells[1].options.to, 100)
        compare(find(page, "panelTo").text, data.units === 1 ? "212" : "100", "re-bound")
        type(find(page, "panelWarnFrom"), data.units === 1 ? "194" : "90")
        compare(page.doc.pages[0].cells[1].options.warnFrom, 90)
        tryCompare(find(page, "dashCell_0_1"), "cell", page.doc.pages[0].cells[1])
    }

    // An enumeration only for a digital tile; text never
    function test_thePickerOffersWhatTheKindCanShow() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        page.refreshRows()
        const rows = page.profileRows
        verify(rows.length > 0, "the session's rows")
        const analog = page.pickable("analog")
        const digital = page.pickable("digital")
        verify(analog.every(r => r.ref !== "" && !r.isText && !r.isEnum))
        verify(digital.every(r => r.ref !== "" && !r.isText))
        compare(digital.length, rows.filter(r => r.ref !== "" && !r.isText).length)
        // The demo has no enumeration: one made up, beside a number and a
        // text
        page.profileRows = [{ ref: "key:mode", isText: false, isEnum: true },
                            { ref: "key:rpm", isText: false, isEnum: false },
                            { ref: "key:vin", isText: true, isEnum: false }]
        compare(page.pickable("bar").map(r => r.ref), ["key:rpm"])
        compare(page.pickable("digital").map(r => r.ref), ["key:mode", "key:rpm"])
    }

    // No session: the rows of the last URI that connected; none, a line
    function test_withNoSessionThePickerReadsWhatSettingsChose() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        page.pickKind("ring")
        find(page, "dashSignalPanel")
        verify(find(page, "signalNone").visible, "nothing chosen, nothing to pick")

        // While the list is open it follows Settings
        page.offlineUri = "mock://?profile=obd2"
        tryVerify(() => findChild(page, "pickSignal_" + Bridge.gaugeSignals["speed"][1]) !== null,
                  3000, "obd2's speed, offered")
        verify(!find(page, "signalNone").visible)

        page.offlineUri = "mock://demo"
        tryVerify(() => findChild(page, "pickSignal_" + Bridge.gaugeSignals["speed"][1]) === null,
                  3000, "and gone with obd2")
        click(page, "pickSignal_" + Bridge.gaugeSignals["speed"][0])
        compare(page.doc.pages[0].cells[1].kind, "ring")
        compare(page.doc.pages[0].cells[1].unit, "km/h")
        compare(page.doc.pages[0].cells[1].options.to, 260)
    }

    // Change signal: the bar narrows the rows; the cell takes the row's
    // name, decoded unit, default range and counterpart's ref, its kind,
    // size and peak hold stay, and its zones, scale, words and decimals
    // go with the old signal
    function test_anotherSignalDropsTheZones() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const zoned = cellOf("A", "analog")
        zoned.options.coldBelow = 500
        zoned.options.warnFrom = 6000
        zoned.options.dangerFrom = 7000
        zoned.options.decimals = 1
        zoned.options.majorStep = 1000
        zoned.options.minorStep = 250
        zoned.options.labelDivisor = 1000
        zoned.options.caption = "x1000 r/min"
        zoned.options.description = "Engine"
        zoned.options.peakLabel = "max"
        zoned.options.peak = true
        const page = pageOf({}, docOf([zoned]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelSignal")
        compare(page.panel, "signal")
        const coolant = Bridge.gaugeSignals["coolant"]
        const speed = Bridge.gaugeSignals["speed"][0]
        // The bar narrows the rows; a find with no match says so
        const panel = find(page, "dashSignalPanel")
        const findField = find(panel, "filterText")
        verify(!find(panel, "filterButton").visible, "no Filter button: its options are a Popup")
        verify(find(panel, "pickSignal_" + speed).visible)
        findField.text = "coolant"
        findField.textEdited()
        tryVerify(() => !findChild(panel, "pickSignal_" + speed), 3000, "speed filtered out")
        verify(find(panel, "pickSignal_" + coolant[0]).visible)
        verify(!find(panel, "signalNone").visible)
        findField.text = "no such signal"
        findField.textEdited()
        tryVerify(() => find(panel, "signalNone").visible, 3000, "nothing matches")
        compare(find(panel, "signalNone").text, "No signal matches.")
        findField.text = "coolant"
        findField.textEdited()
        click(page, "pickSignal_" + coolant[0])
        const c = page.doc.pages[0].cells[0]
        compare(c.kind, "analog")
        compare(c.w, 2)
        compare(c.label, "Coolant Temp")
        compare(c.unit, "°C")
        compare(JSON.stringify(c.refs), JSON.stringify(coolant))
        compare(c.options.from, 0)
        compare(c.options.to, 150)
        for (const gone of ["decimals", "majorStep", "minorStep", "labelDivisor", "caption",
                            "description", "peakLabel"])
            verify(c.options[gone] === undefined, gone + " goes with the old signal")
        compare(c.options.peak, true, "peak hold stays")
        verify(c.options.coldBelow === undefined, "no cold zone")
        verify(c.options.warnFrom === undefined, "no warning zone")
        verify(c.options.dangerFrom === undefined, "no danger zone")
        compare(page.panel, "cell")
        compare(page.selected, 0)
        compare(storedText(page), DashboardBook.serialize(page.doc))
    }

    // The cell's panel: each field writes its option, an emptied zone
    // goes, a refused write (from past to) stores nothing
    function test_theCellsPanelSetsItsOptions() {
        const page = pageOf({}, docOf([cellOf("A", "analog"), cellOf("D", "digital"),
                                       cellOf("S", "shift-light")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const at = () => page.doc.pages[0].cells[0]
        type(find(page, "panelLabel"), "Tach")
        compare(at().label, "Tach")
        type(find(page, "panelFrom"), "500")
        compare(at().options.from, 500)
        type(find(page, "panelDangerFrom"), "6500")
        compare(at().options.dangerFrom, 6500)
        type(find(page, "panelDangerFrom"), "")
        verify(at().options.dangerFrom === undefined, "emptied, removed")
        type(find(page, "panelFrom"), "")
        compare(at().options.from, 500, "an empty end puts the end back")
        compare(find(page, "panelFrom").text, "500")
        spin(find(page, "panelDecimals"), 2)
        compare(at().options.decimals, 2)
        const peak = find(page, "panelPeak")
        peak.toggle()
        peak.toggled()
        compare(at().options.peak, true)
        type(find(page, "panelMajorStep"), "1000")
        compare(at().options.majorStep, 1000)
        type(find(page, "panelCaption"), "x1000")
        compare(at().options.caption, "x1000")
        verify(!find(page, "panelDescription").visible, "a digital tile's own")

        const before = storedText(page)
        type(find(page, "panelFrom"), "9000")
        compare(at().options.from, 500, "refused: from past to")
        compare(storedText(page), before, "nothing stored")
        compare(find(page, "panelFrom").text, "500", "the field shows what's kept")
        type(find(page, "panelFrom"), "abc")
        compare(storedText(page), before)

        press(find(page, "dashCell_0_1"))
        verify(find(page, "panelDescription").visible)
        verify(!find(page, "panelMajorStep").visible)
        type(find(page, "panelDescription"), "Gear")
        compare(page.doc.pages[0].cells[1].options.description, "Gear")
        press(find(page, "dashCell_0_2"))
        spin(find(page, "panelCount"), 10)
        compare(page.doc.pages[0].cells[2].options.count, 10)
        compare(storedText(page), DashboardBook.serialize(page.doc))
    }

    // Step 5's kinds offer their own options in the panel, the bar its two
    // switches, and the peak switch shows for the kinds that draw a peak
    function test_theNewKindsOwnOptionsAreEditable() {
        const narrow = (label, kind) => Object.assign(cellOf(label, kind), { w: 1 })
        const digits = Object.assign(narrow("Digits", "seven-segment"), { options: { from: 0, to: 100 } })
        const page = pageOf({}, docOf([narrow("Cells", "segments"), digits,
                                       narrow("Trace", "sparkline"), narrow("Level", "bar"),
                                       narrow("Temp", "ring"), narrow("Gear", "digital")]))
        page.startEditing()
        const at = i => page.doc.pages[0].cells[i]
        const hidden = names => { for (const n of names) verify(!find(page, n).visible, n + " is hidden") }

        press(find(page, "dashCell_0_0"))
        compare(find(page, "panelCells").Accessible.name, "Cells")
        compare(find(page, "panelRise").Accessible.name, "Curve")
        compare(find(page, "panelDecimals").Accessible.name, "Decimals")
        spin(find(page, "panelCells"), 12)
        compare(at(0).options.cells, 12)
        type(find(page, "panelRise"), "2.5")
        compare(at(0).options.rise, 2.5)
        verify(find(page, "panelPeak").visible, "the cells draw a peak")
        hidden(["panelDigits", "panelWindow", "panelVertical", "panelMarks", "panelMajorStep"])

        press(find(page, "dashCell_0_1"))
        tryCompare(find(page, "panelDigits"), "value", 3, 1000, "Digits shows the range's count")
        spin(find(page, "panelDigits"), 6)
        compare(at(1).options.digits, 6)
        verify(!find(page, "panelPeak").visible, "the seven-segment readout has no peak")
        hidden(["panelCells", "panelWindow", "panelVertical"])

        press(find(page, "dashCell_0_2"))
        compare(find(page, "panelWindow").Accessible.name, "Window (s)")
        spin(find(page, "panelWindow"), 60)
        compare(at(2).options.window, 60)
        verify(!find(page, "panelPeak").visible, "the sparkline has no peak")
        hidden(["panelCells", "panelDigits", "panelVertical"])

        press(find(page, "dashCell_0_3"))
        compare(page.selected, 3, "the bar is selected")
        compare(at(3).kind, "bar")
        const vertical = find(page, "panelVertical")
        vertical.toggle()
        vertical.toggled()
        compare(at(3).options.vertical, true)
        const marks = find(page, "panelMarks")
        compare(marks.checked, true, "the ticks are on until turned off")
        marks.toggle()
        marks.toggled()
        compare(at(3).options.marks, false)
        verify(find(page, "panelPeak").visible, "the bar draws a peak")

        press(find(page, "dashCell_0_4"))
        verify(find(page, "panelPeak").visible, "the ring draws a peak")
        hidden(["panelVertical", "panelCells"])
        press(find(page, "dashCell_0_5"))
        verify(!find(page, "panelPeak").visible, "the number shows its own MIN and MAX instead")
        compare(storedText(page), DashboardBook.serialize(page.doc))
    }

    // Add offers the new kinds, each with a preview and its default span
    function test_addOffersTheNewKinds_data() {
        return [{ tag: "segments", kind: "segments", span: [2, 3] },
                { tag: "seven-segment", kind: "seven-segment", span: [1, 2] },
                { tag: "sparkline", kind: "sparkline", span: [2, 3] }]
    }
    function test_addOffersTheNewKinds(data) {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        page.startAdd()
        verify(find(page, "addKind_" + data.kind), "a tile for it")
        verify(find(page, "preview_" + data.kind).item, "its preview loads")
        compare(page.newSpans[data.kind].join("x"), data.span.join("x"))
        compare(page.drawnKinds.indexOf(data.kind) >= 0, true)
        if (data.kind === "sparkline") {
            const spark = find(page, "preview_sparkline").item
            verify(spark.history.length > 10, "a still trace to look at")
            wait(300)
            compare(spark.history.length, 120, "and it takes no samples of its own")
            verify(spark.tracePoints(200, 50)[0].x < 2, "the trace spans the preview")
        }
    }

    // The span and the order; the selection follows the cell, and
    // Remove takes a cell out with the selection kept on its own
    function test_resizeMoveAndRemove() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B"), cellOf("C")]))
        page.startEditing()
        press(find(page, "dashCell_0_1"))
        spin(find(page, "panelW"), 3)
        spin(find(page, "panelH"), 6)
        compare(page.doc.pages[0].cells[1].w, 3)
        compare(page.doc.pages[0].cells[1].h, 6)
        click(page, "panelLater")
        compare(labels(page), ["A", "C", "B"])
        compare(page.selected, 2)
        verify(!find(page, "panelLater").enabled, "the last")
        verify(!find(page, "panelEnd").enabled)
        click(page, "panelStart")
        compare(labels(page), ["B", "A", "C"])
        compare(page.selected, 0)
        verify(!find(page, "panelEarlier").enabled, "the first")
        click(page, "panelEnd")
        compare(labels(page), ["A", "C", "B"])
        click(page, "panelEarlier")
        compare(labels(page), ["A", "B", "C"])
        compare(page.selected, 1)

        press(find(find(page, "dashCell_0_0"), "cellRemove"))
        compare(labels(page), ["B", "C"])
        compare(page.selected, 0, "still B")
        compare(page.panel, "cell")
        press(find(find(page, "dashCell_0_0"), "cellRemove"))
        compare(labels(page), ["C"])
        compare(page.selected, -1, "its own cell went")
        compare(page.panel, "")
        press(find(find(page, "dashCell_0_0"), "cellRemove"))
        compare(labels(page), [])
        verify(find(page, "dashAddCell_0").visible, "the + cell on an empty page")
        click(page, "dashDone")
        verify(find(page, "dashEmptyPage").visible, "an empty page says so")
        compare(storedText(page), DashboardBook.serialize(page.doc))
    }

    // A page that's full has no + cell and Add is off
    function test_aFullPageTakesNoMore() {
        const cells = []
        for (let i = 0; i < DashboardBook.maxCells; ++i)
            cells.push(cellOf("C" + i))
        const page = pageOf({}, docOf(cells))
        page.startEditing()
        verify(!find(page, "dashAddCell_0").visible)
        verify(!find(page, "dashAdd").enabled)
        page.startAdd()
        compare(page.panel, "", "Add opens nothing")
    }

    // The pages' row: add, rename, move, remove (never the last)
    function test_thePagesControls() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        verify(!find(page, "dashMoreRow").visible)
        click(page, "dashMore")
        verify(find(page, "dashMoreRow").visible)
        verify(!find(page, "moreRemovePage").enabled, "never the last page")
        click(page, "moreAddPage")
        compare(page.doc.pages.length, 2)
        compare(page.doc.pages[1].name, "Page 2")
        compare(page.pageIndex, 1, "the new page shows")
        type(find(page, "moreRename"), "Temps")
        compare(page.doc.pages[1].name, "Temps")
        compare(find(page, "moreRename").text, "Temps")
        verify(!find(page, "moreRight").enabled)
        click(page, "moreLeft")
        compare(page.doc.pages.map(p => p.name), ["Temps", "Test"])
        compare(page.pageIndex, 0, "the page moved shows")
        compare(find(page, "moreRename").text, "Temps")
        verify(!find(page, "moreLeft").enabled)
        click(page, "moreRight")
        compare(page.doc.pages.map(p => p.name), ["Test", "Temps"])
        compare(page.pageIndex, 1)
        // Remove page asks in the page; Keep changes nothing
        click(page, "moreRemovePage")
        verify(find(page, "dashRemovePageConfirm").visible)
        verify(!find(page, "dashMoreRow").visible)
        verify(find(page, "removePageQuestion").text.indexOf("Temps") >= 0, "names the page")
        click(page, "removePageKeep")
        verify(!find(page, "dashRemovePageConfirm").visible)
        verify(find(page, "dashMoreRow").visible)
        compare(page.doc.pages.length, 2, "kept")
        click(page, "moreRemovePage")
        click(page, "removePageConfirm")
        verify(!find(page, "dashRemovePageConfirm").visible)
        compare(page.doc.pages.map(p => p.name), ["Test"])
        compare(page.pageIndex, 0)
        compare(storedText(page), DashboardBook.serialize(page.doc))
        // A page change drops the selection
        click(page, "moreAddPage")
        page.goTo(0)
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        page.goTo(1)
        compare(page.selected, -1)
        compare(page.panel, "")
    }

    // Removing the page shown closes its panel, though the next page takes
    // its index (so no page change says so)
    function test_removingThePageShownClosesItsPanel_data() {
        return [{ tag: "a cell", add: false }, { tag: "Add", add: true }]
    }
    function test_removingThePageShownClosesItsPanel(data) {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const two = docOf([cellOf("A"), cellOf("B")])
        two.pages.push({ name: "More", cells: [cellOf("C"), cellOf("D")] })
        const page = pageOf({}, two)
        page.startEditing()
        if (data.add) {
            page.startAdd()
            page.pickKind("ring")
            compare(page.panel, "signal")
            compare(page.addKind, "ring")
        } else {
            press(find(page, "dashCell_0_1"))
            compare(page.selected, 1)
            compare(page.panel, "cell")
        }
        click(page, "dashMore")
        click(page, "moreRemovePage")
        click(page, "removePageConfirm")
        compare(page.doc.pages.map(p => p.name), ["More"])
        compare(page.pageIndex, 0)
        compare(page.selected, -1)
        compare(page.panel, "")
        compare(page.addKind, "")
    }

    // A page change closes whatever panel was open: a cell's signal, Add
    // and Add's signals
    function test_aPageChangeClosesAnyPanel() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const two = docOf([cellOf("A", "analog")])
        two.pages.push({ name: "More", cells: [cellOf("B", "analog")] })
        const page = pageOf({}, two)
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelSignal")
        compare(page.panel, "signal")
        page.goTo(1)
        compare(page.panel, "", "the signal panel of a cell no longer selected")
        page.startAdd()
        compare(page.panel, "add")
        page.goTo(0)
        compare(page.panel, "")
        page.startAdd()
        page.pickKind("bar")
        compare(page.addKind, "bar")
        page.goTo(1)
        compare(page.panel, "")
        compare(page.addKind, "")
    }

    // Typed text goes to the cell or page it was typed for: a tap on
    // another cell and a swipe take no focus of their own
    function test_typedTextStaysWithItsCellAndPage() {
        const two = docOf([cellOf("A"), cellOf("B")])
        two.pages.push({ name: "More", cells: [cellOf("C")] })
        const page = pageOf({}, two)
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const label = find(page, "panelLabel")
        label.forceActiveFocus()
        label.selectAll()
        keyClick("t")
        keyClick("a")
        keyClick("c")
        keyClick("h")
        compare(label.text, "tach", "typed")
        press(find(page, "dashCell_0_1"))
        compare(page.selected, 1)
        compare(labels(page), ["tach", "B"], "the label went to the cell it was typed for")

        click(page, "dashMore")
        const rename = find(page, "moreRename")
        rename.forceActiveFocus()
        rename.selectAll()
        keyClick("x")
        keyClick("y")
        find(page, "dashPages").setCurrentIndex(1)
        tryCompare(page, "pageIndex", 1)
        compare(page.doc.pages.map(p => p.name), ["xy", "More"], "the name went to its page")
        rename.forceActiveFocus()
        rename.selectAll()
        keyClick("z")
        page.goTo(0)
        compare(page.doc.pages.map(p => p.name), ["xy", "z"])
    }

    // Reset asks first; Keep changes nothing; Reset stores nothing, so the
    // built-in layout shows from then on
    function test_resetAsksFirst() {
        const stored = createTemporaryObject(storeComponent, testCase)
        const two = docOf([cellOf("A")])
        two.pages.push({ name: "More", cells: [cellOf("B")] })
        stored.setValue("desktop", DashboardBook.serialize(two))
        const page = pageOf({}, null)
        compare(page.pages.length, 2)
        page.goTo(1)
        page.startEditing()
        click(page, "dashMore")
        click(page, "moreReset")
        verify(find(page, "dashResetConfirm").visible)
        verify(!find(page, "dashMoreRow").visible)
        click(page, "resetKeep")
        verify(!find(page, "dashResetConfirm").visible)
        compare(page.pages.length, 2, "kept")
        click(page, "moreReset")
        click(page, "resetConfirm")
        compare(JSON.stringify(page.doc), JSON.stringify(DashboardBook.builtIn("desktop")))
        compare(page.pageIndex, 0)
        compare(storedText(page), "", "nothing stored")
        verify(page.editing, "still editing")
    }

    // Reset closes the cell's panel on the page it was on, and keeps a
    // text that couldn't be read in <form>Unread as an edit does
    function test_resetClosesThePanel() {
        const stored = createTemporaryObject(storeComponent, testCase)
        const two = docOf([cellOf("A")])
        two.pages.push({ name: "More", cells: [cellOf("B")] })
        stored.setValue("desktop", DashboardBook.serialize(two))
        const page = pageOf({}, null)
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        compare(page.panel, "cell")
        click(page, "dashMore")
        click(page, "moreReset")
        click(page, "resetConfirm")
        compare(page.pageIndex, 0)
        compare(page.selected, -1)
        compare(page.panel, "")
    }
    function test_resetKeepsATextThatCantBeRead() {
        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", "{")
        const page = pageOf({}, null)
        compare(page.problem, "damaged")
        page.startEditing()
        click(page, "dashMore")
        click(page, "moreReset")
        click(page, "resetConfirm")
        const store = find(page, "dashStore")
        compare(store.desktopUnread, "{", "kept")
        compare(store.desktop, "", "nothing stored")
    }

    // The kinds drawn as a strip take the picker's wide tile
    function test_thePickersStripKindsTakeTheWideTile() {
        const page = pageOf({}, docOf([cellOf("A")]))
        click(page, "dashEdit")
        click(page, "dashAdd")
        const kinds = find(page, "dashAddPanel")
        const strips = ["bar", "shift-light", "segments", "sparkline"]
        for (const kind of strips)
            verify(page.drawnKinds.includes(kind), kind + " is offered")
        for (const kind of page.drawnKinds)
            tryCompare(find(kinds, "preview_" + kind), "width",
                       strips.includes(kind) ? 200 : 120, 3000, kind)
    }

    // Done closes the pages' row and each question, so Edit opens the
    // bar alone
    function test_doneClosesTheRowAndTheQuestions() {
        const two = docOf([cellOf("A")])
        two.pages.push({ name: "More", cells: [cellOf("B")] })
        const page = pageOf({}, two)
        page.startEditing()
        click(page, "dashMore")
        verify(find(page, "dashMoreRow").visible)
        click(page, "dashDone")
        click(page, "dashEdit")
        verify(!find(page, "dashMoreRow").visible, "the row closed")
        click(page, "dashMore")
        click(page, "moreReset")
        verify(find(page, "dashResetConfirm").visible)
        click(page, "dashDone")
        click(page, "dashEdit")
        verify(!find(page, "dashResetConfirm").visible, "Reset's question closed")
        verify(!find(page, "dashMoreRow").visible)
        click(page, "dashMore")
        click(page, "moreRemovePage")
        verify(find(page, "dashRemovePageConfirm").visible)
        click(page, "dashDone")
        click(page, "dashEdit")
        verify(!find(page, "dashRemovePageConfirm").visible, "Remove page's question closed")
        compare(page.doc.pages.length, 2)
    }

    // Peak hold shows only for a kind that draws it: the dial, the ring, the
    // bar and the cells do; a number (it shows its MIN and MAX), the shift
    // lights, the seven-segment readout and the sparkline don't
    function test_peakHoldOnlyWhereItDraws_data() {
        return [{ tag: "the kinds that draw one", kinds: ["analog", "ring", "bar", "segments"], draws: true },
                { tag: "the kinds that don't", kinds: ["digital", "shift-light", "seven-segment", "sparkline"],
                  draws: false }]
    }
    function test_peakHoldOnlyWhereItDraws(data) {
        const page = pageOf({}, docOf(data.kinds.map(k => Object.assign(cellOf(k, k), { w: 1 }))))
        page.startEditing()
        for (let i = 0; i < data.kinds.length; ++i) {
            press(find(page, "dashCell_0_" + i))
            compare(page.selected, i, "cell " + i + " is the one selected")
            compare(find(page, "panelPeak").visible, data.draws, data.kinds[i])
        }
    }

    // The Decimals stepper starts at the row's when the cell sets none
    function test_theDecimalsStepperStartsAtTheRows() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const maf = cellOf("MAF")
        maf.refs = Array.from(Bridge.gaugeSignals["maf"])
        maf.unit = "g/s"
        maf.options = { from: 0, to: 300 }
        const page = pageOf({}, docOf([maf]))
        page.startEditing()
        const cell = find(page, "dashCell_0_0")
        tryVerify(() => cell.entry !== null && cell.entry.decimals > 0, 5000, "the row's decimals")
        press(cell)
        const box = find(page, "panelDecimals")
        compare(box.value, cell.entry.decimals)
        verify(page.doc.pages[0].cells[0].options.decimals === undefined, "nothing stored")
        spin(box, 0)
        compare(page.doc.pages[0].cells[0].options.decimals, 0)
        compare(box.value, 0)
    }

    // A profile with no number to show says so, not "connect"
    function test_aProfileWithNothingToPickSaysSo() {
        compare(Bridge.connectSession("mock://demo?profile=raw"), "", "raw connects")
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        page.startAdd()
        page.pickKind("ring")
        const none = find(page, "signalNone")
        verify(none.visible)
        compare(none.text, "This session's profile has no signal a gauge can show.")
    }

    // The major step is typed in the stored unit, in any unit system
    function test_theMajorStepIsNotConverted() {
        Bridge.unitSystem = 1
        const t = cellOf("T", "analog")
        t.unit = "°C"
        t.options = { from: 0, to: 150 }
        const page = pageOf({}, docOf([t]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        type(find(page, "panelMajorStep"), "20")
        compare(page.doc.pages[0].cells[0].options.majorStep, 20)
        compare(find(page, "panelMajorStep").text, "20")
    }

    // ADR-0057 section 10's restart: a layout built through the controls
    // is the one a new page reads, on the page it was left on
    function test_aLayoutBuiltInEditModeSurvivesARestart() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const first = pageOf({}, null)
        first.startEditing()
        click(first, "dashMore")
        click(first, "moreAddPage")
        type(find(first, "moreRename"), "Built")
        click(first, "dashMore")
        compare(first.pageIndex, 1, "the new page shows")
        click(first, "dashAddCell_1")
        compare(first.panel, "add", "the + cell opens Add")
        click(first, "addKind_bar")
        click(first, "pickSignal_" + Bridge.gaugeSignals["rpm"][0])
        spin(find(first, "panelW"), 4)
        click(first, "dashAdd")
        click(first, "addKind_digital")
        click(first, "pickSignal_" + Bridge.gaugeSignals["speed"][0])
        click(first, "panelEarlier")
        click(first, "dashDone")
        const built = DashboardBook.serialize(first.doc)
        compare(first.doc.pages[1].cells.map(c => c.kind), ["digital", "bar"])
        compare(first.doc.pages[1].cells[1].w, 4)
        first.destroy()
        wait(0)

        const again = pageOf({}, null)
        compare(again.problem, "", "the stored text reads")
        compare(DashboardBook.serialize(again.doc), built, "the same document")
        compare(again.pageIndex, 1, "the same page")
        tryCompare(find(again, "dashPages"), "currentIndex", 1)
        compare(find(again, "dashCell_1_0").cell.label, "Vehicle Speed")
        compare(find(again, "dashCell_1_1").cell.label, "RPM")
    }

    // A stored text that can't be read, or read only in part: the first
    // edit keeps it in <form>Unread and stores the edited document; a
    // later one leaves it
    function test_theFirstEditKeepsATextThatCantBeRead_data() {
        const part = docOf([cellOf("Bad"), cellOf("Good")])
        part.pages[0].cells[0].w = "x"
        return [{ tag: "damaged", text: "{" },
                { tag: "newer", text: JSON.stringify({ format: 3, pages: [] }) },
                { tag: "dropped", text: JSON.stringify(part) }]
    }
    function test_theFirstEditKeepsATextThatCantBeRead(data) {
        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", data.text)
        const page = pageOf({}, null)
        compare(page.problem, data.tag)
        if (data.tag === "dropped") {
            compare(labels(page), ["Good"], "the cell that reads")
            verify(find(page, "dashProblem").visible)
            verify(find(page, "dashProblemText").text.indexOf("1 gauge left out") >= 0,
                   find(page, "dashProblemText").text)
        }
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        type(find(page, "panelLabel"), "Edited")
        const store = find(page, "dashStore")
        compare(store.desktopUnread, data.text, "kept")
        compare(store.desktop, DashboardBook.serialize(page.doc))
        compare(page.problem, "", "the stored text reads")
        compare(store.phoneUnread, "", "the phone's is its own")
        type(find(page, "panelLabel"), "Again")
        compare(store.desktopUnread, data.text, "a later edit leaves it")
        page.destroy()
        wait(0)
        compare(String(stored.value("desktopUnread")), data.text, "stored as the page goes")
    }

    // ---- The gesture (ADR-0057's appendix, section 4)

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
    // An item's middle, in the page's coordinates
    function centerOf(page, item) {
        return item.mapToItem(page, item.width / 2, item.height / 2)
    }
    readonly property int holdMs: Application.styleHints.mousePressAndHoldInterval
    // Set on the held cell alone, just before its press (a reflow can replace
    // the delegate); other cells' taps keep the platform's interval
    readonly property int shortHoldMs: 200
    function shortHold(cell) {
        find(cell, "cellTap").pressAndHoldInterval = shortHoldMs
    }

    // A release after the hold opens the cell's menu, on both forms and
    // in or out of edit mode, and leaves the mode as it was
    function test_aHoldThenReleaseOpensTheMenu_data() {
        return [{ tag: "phone", phone: true, edit: false },
                { tag: "phone in edit mode", phone: true, edit: true },
                { tag: "desktop", phone: false, edit: false },
                { tag: "desktop in edit mode", phone: false, edit: true }]
    }
    function test_aHoldThenReleaseOpensTheMenu(data) {
        const page = pageOf(data.phone ? { form: "phone", width: 384, height: 832 } : {},
                            docOf([cellOf("A"), cellOf("B")]))
        if (data.edit)
            page.startEditing()
        const cell = find(page, "dashCell_0_1")
        const view = find(page, "dashPage_0")
        const swipe = find(page, "dashPages")
        hold(page, cell)
        verify(cell.lifted, "the cell lifts")
        verify(!view.interactive, "the page doesn't scroll under a held cell")
        verify(!swipe.interactive, "nor swipe")
        compare(page.panel, "", "the menu waits for the release")
        mouseMove(cell, cell.width / 2 + 1, cell.height / 2, -1, Qt.LeftButton)
        compare(page.dropAt, -1, "a finger's tremble isn't a drag")
        mouseRelease(cell)
        compare(page.held, -1)
        compare(page.selected, 1)
        compare(page.panel, "cell")
        compare(page.editing, data.edit, "edit mode is as it was")
        verify(find(cell, "cellSelected").visible, "the panel's cell is outlined")
        verify(find(page, "panelCopy").visible)
        verify(find(page, "panelPaste").visible)
        verify(view.interactive, "the page scrolls again")
        compare(swipe.interactive, data.phone, "and swipes where it did")

        click(page, "panelClose")
        compare(page.panel, "")
        compare(page.selected, data.edit ? 1 : -1, "only edit mode keeps a selection")
    }

    // A press let go before the hold is a tap
    function test_aTapIsNotAHold() {
        const page = pageOf({}, docOf([cellOf("A")]))
        press(find(page, "dashCell_0_0"))
        compare(page.held, -1)
        compare(page.panel, "", "no menu outside edit mode")
    }

    // A finger that moves before the hold completes is a swipe or a
    // scroll; the desktop's sideways move has nothing to take it
    function test_aMoveBeforeTheHoldEndsIt_data() {
        return [{ tag: "phone, up", phone: true, dx: 0, dy: -60 },
                { tag: "phone, sideways", phone: true, dx: 60, dy: 0 },
                { tag: "desktop, down", phone: false, dx: 0, dy: 60 },
                { tag: "desktop, sideways", phone: false, dx: 60, dy: 0 }]
    }
    function test_aMoveBeforeTheHoldEndsIt(data) {
        const page = pageOf(data.phone ? { form: "phone", width: 384, height: 832 } : {},
                            docOf([cellOf("A"), cellOf("B")]))
        page.startEditing()
        const cell = find(page, "dashCell_0_0")
        tryVerify(() => cell.width > 0, 3000, "laid out")
        verify(waitForPolish(page.Window.window), "laid out")
        const at = centerOf(page, cell)
        shortHold(cell)
        mousePress(page, at.x, at.y)
        mouseMove(page, at.x + data.dx, at.y + data.dy, -1, Qt.LeftButton)
        wait(shortHoldMs + 300)
        compare(page.held, -1, "the hold was canceled")
        mouseRelease(page, at.x + data.dx, at.y + data.dy)
        compare(page.panel, "", "and it isn't a tap either")
        compare(labels(page), ["A", "B"])
    }

    // After the hold a move drags: the others reflow as it passes them,
    // the drop is the change of order, and only the drop stores it
    function test_aHeldCellDragsToANewPlace() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B"), cellOf("C"), cellOf("D")]))
        const a = find(page, "dashCell_0_0")
        const c = find(page, "dashCell_0_2")
        hold(page, a)
        const from = centerOf(page, a)
        const to = centerOf(page, c)
        mouseMove(page, (from.x + to.x) / 2, (from.y + to.y) / 2, -1, Qt.LeftButton)
        mouseMove(page, to.x, to.y, -1, Qt.LeftButton)
        tryCompare(page, "dropAt", 2)
        verify(a.carried, "the cell follows the finger")
        fuzzyCompare(centerOf(page, a).x, to.x, 2, "under the finger")
        // Index 1's cell (B) has moved to the first place, C to the second
        compare([find(page, "dashCell_0_1").box.x, find(page, "dashCell_0_1").box.y], [0, 0])
        compare([find(page, "dashCell_0_2").box.x, find(page, "dashCell_0_2").box.y], [2, 0])
        compare(labels(page), ["A", "B", "C", "D"], "the order waits for the drop")
        compare(storedText(page), "", "nothing stored yet")

        mouseRelease(page, to.x, to.y)
        compare(labels(page), ["B", "C", "A", "D"])
        compare(storedText(page), DashboardBook.serialize(page.doc), "stored by the drop")
        compare(page.held, -1)
        compare(page.dropAt, -1)
        compare(page.panel, "", "a drag opens no menu")
        verify(find(page, "dashPage_0").interactive, "the page scrolls again")
    }

    // Dragged away and brought back: nothing changed, nothing stored
    function test_aDragBackToItsPlaceChangesNothing() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B"), cellOf("C")]))
        const a = find(page, "dashCell_0_0")
        const c = find(page, "dashCell_0_2")
        hold(page, a)
        const from = centerOf(page, a)
        const to = centerOf(page, c)
        mouseMove(page, to.x, to.y, -1, Qt.LeftButton)
        tryCompare(page, "dropAt", 2)
        mouseMove(page, from.x, from.y, -1, Qt.LeftButton)
        tryCompare(page, "dropAt", 0)
        mouseRelease(page, from.x, from.y)
        compare(labels(page), ["A", "B", "C"])
        compare(storedText(page), "")
        compare(page.panel, "", "a drag opens no menu even when it comes back")
    }

    // A page change under a held finger ends the hold, and the release
    // opens nothing
    function test_aPageChangeEndsTheHold() {
        const d = docOf([cellOf("A")])
        d.pages.push({ name: "P2", cells: [cellOf("B")] })
        const page = pageOf({}, d)
        const a = find(page, "dashCell_0_0")
        hold(page, a)
        page.goTo(1)
        compare(page.held, -1)
        verify(find(page, "dashPage_0").interactive, "the page scrolls again")
        mouseRelease(a)
        compare(page.panel, "")
    }

    // A label typed into one cell's menu stays with that cell when
    // another cell is held, outside edit mode too
    function test_aTypedLabelStaysWithItsCellOutsideEditMode() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B")]))
        const a = find(page, "dashCell_0_0")
        const b = find(page, "dashCell_0_1")
        hold(page, a)
        mouseRelease(a)
        const field = find(page, "panelLabel")
        field.forceActiveFocus()
        tryVerify(() => field.activeFocus, 3000, "the field takes the focus")
        field.text = "Typed"
        hold(page, b)
        compare(labels(page), ["Typed", "B"], "committed as the cell lifts")
        mouseRelease(b)
        compare(page.selected, 1)
        compare(labels(page), ["Typed", "B"])
    }

    // The reflow places the others by their own spans in the new order
    function test_theOthersReflowByTheirOwnSpans() {
        const wide = cellOf("A")
        wide.w = 4
        const page = pageOf({}, docOf([wide, cellOf("B"), cellOf("C")]))
        const a = find(page, "dashCell_0_0")
        const b = find(page, "dashCell_0_1")
        const view = find(page, "dashPage_0")
        tryCompare(view, "columns", 6, 3000)
        hold(page, a)
        const slot = a.parent.mapToItem(page, view.dropCenters[2].x, view.dropCenters[2].y)
        mouseMove(page, slot.x, slot.y, -1, Qt.LeftButton)
        tryCompare(page, "dropAt", 2)
        compare(b.box.w, 2, "B keeps its own span")
        compare([b.box.x, b.box.y], [0, 0])
        mouseRelease(page, slot.x, slot.y)
        compare(labels(page), ["B", "C", "A"])
    }

    // What changes the page under a held finger ends the hold: nothing
    // stays non-interactive, and the release moves and opens nothing
    function test_aChangeUnderTheFingerEndsTheHold_data() {
        return [{ tag: "a cell removed before it", act: p => p.removeCell(0), left: ["B", "C", "D"] },
                { tag: "another page added", act: p => p.addPage(), left: ["A", "B", "C", "D"] },
                { tag: "Done", act: p => p.stopEditing(), left: ["A", "B", "C", "D"] }]
    }
    function test_aChangeUnderTheFingerEndsTheHold(data) {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B"), cellOf("C"), cellOf("D")]))
        page.startEditing()
        const c = find(page, "dashCell_0_2")
        const view = find(page, "dashPage_0")
        hold(page, c)
        const from = centerOf(page, c)
        mouseMove(page, from.x - 200, from.y, -1, Qt.LeftButton)
        tryVerify(() => page.dropAt >= 0, 3000, "the drag is under way")
        data.act(page)
        compare(page.held, -1)
        compare(page.dropAt, -1)
        verify(!view.reflowing, "no reflow, so the edge timer stops")
        verify(view.interactive, "the page scrolls again")
        mouseRelease(page, from.x - 200, from.y)
        const shown = page.doc.pages[0].cells.map(c => c.label)
        compare(shown, data.left, "the release moves nothing")
        compare(page.panel, "", "and opens nothing")
    }

    // Another grid under a drag ends it: its places were measured on the old one
    function test_anotherGridEndsTheHold_data() {
        return [{ tag: "columns, the phone turned", phone: true, w: 832, h: 384, rows: 8 },
                { tag: "the row height alone", phone: false, w: 1280, h: 300, rows: 8 },
                { tag: "the columns alone, the height's cap the row", phone: false, w: 640, h: 800,
                  rows: 48, samePitch: true }]
    }
    function test_anotherGridEndsTheHold(data) {
        const tall = cellOf("A")
        tall.h = data.rows
        const page = pageOf(data.phone ? { form: "phone", width: 384, height: 832 } : {},
                            docOf([tall, cellOf("B"), cellOf("C")]))
        const a = find(page, "dashCell_0_0")
        const view = find(page, "dashPage_0")
        hold(page, a)
        const from = centerOf(page, a)
        mouseMove(page, from.x + 100, from.y, -1, Qt.LeftButton)
        tryVerify(() => page.dropAt >= 0, 3000, "the drag is under way")
        const columns = view.columns
        const pitch = view.pitch
        page.width = data.w
        page.height = data.h
        tryVerify(() => page.held === -1, 3000, "the hold ends")
        if (data.samePitch) {
            fuzzyCompare(view.pitch, pitch, 1e-6, "the row height held")
            verify(view.columns !== columns, "and the columns didn't")
        }
        compare(page.dropAt, -1)
        verify(view.interactive, "the page scrolls again")
        mouseRelease(page, from.x + 100, from.y)
        compare(labels(page), ["A", "B", "C"], "the release moves nothing")
    }

    // A hold the system takes away changes nothing
    function test_aCanceledHoldChangesNothing() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B"), cellOf("C")]))
        const a = find(page, "dashCell_0_0")
        const c = find(page, "dashCell_0_2")
        hold(page, a)
        const to = centerOf(page, c)
        mouseMove(page, to.x, to.y, -1, Qt.LeftButton)
        tryCompare(page, "dropAt", 2)
        a.holdEnded(true)
        compare(page.held, -1)
        compare(page.dropAt, -1)
        compare(labels(page), ["A", "B", "C"])
        compare(page.panel, "")
        mouseRelease(page, to.x, to.y)
        compare(labels(page), ["A", "B", "C"], "the finger's release does nothing after it")
    }

    // The selection follows its cell through another cell's drag
    function test_aDragKeepsTheSelectionOnItsCell() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B"), cellOf("C")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        const a = find(page, "dashCell_0_0")
        const c = find(page, "dashCell_0_2")
        hold(page, c)
        const to = centerOf(page, a)
        mouseMove(page, to.x, to.y, -1, Qt.LeftButton)
        tryCompare(page, "dropAt", 0)
        mouseRelease(page, to.x, to.y)
        compare(labels(page), ["C", "A", "B"])
        compare(page.selected, 1, "still A")
        compare(page.panel, "cell", "and its panel stays")
    }

    // A drag near the view's top or bottom edge scrolls the page, and
    // carrying on past what a page holds stops at its end
    function test_aDragNearTheEdgeScrollsThePage() {
        const cells = []
        for (let i = 0; i < 6; ++i)
            cells.push({ kind: "digital", w: 6, h: 8, label: "C" + i, refs: ["id:1001"], unit: "RPM",
                         options: { from: 0, to: 8000 } })
        const page = pageOf({ height: 400 }, docOf(cells))
        const view = find(page, "dashPage_0")
        const first = find(page, "dashCell_0_0")
        tryVerify(() => view.contentHeight > view.height + 100, 3000, "it has somewhere to scroll")
        hold(page, first)
        const edge = view.mapToItem(page, view.width / 2, view.height - 4)
        mouseMove(page, edge.x, edge.y - 60, -1, Qt.LeftButton)
        mouseMove(page, edge.x, edge.y, -1, Qt.LeftButton)
        tryVerify(() => view.contentY > 50, 5000, "the page scrolls down")
        tryVerify(() => page.dropAt >= 1, 5000, "and the drag follows it")
        const top = view.mapToItem(page, view.width / 2, 4)
        mouseMove(page, top.x, top.y, -1, Qt.LeftButton)
        tryCompare(view, "contentY", 0, 8000, "and back to the top, no further")
        mouseRelease(page, top.x, top.y)
    }

    function test_aPageThatFitsDoesNotScrollAtTheEdge() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B")]))
        const view = find(page, "dashPage_0")
        const a = find(page, "dashCell_0_0")
        hold(page, a)
        const edge = view.mapToItem(page, view.width / 2, view.height - 4)
        mouseMove(page, edge.x, edge.y, -1, Qt.LeftButton)
        wait(300)
        compare(view.contentY, 0)
        mouseRelease(page, edge.x, edge.y)
    }

    // A dial's tap resets its peak, through the cell's hold area
    function test_aDialsTapStillResetsItsPeak() {
        const c = cellOf("A", "analog")
        c.options.peak = true
        const page = pageOf({}, docOf([c]))
        const cell = find(page, "dashCell_0_0")
        const loader = find(cell, "gauge")
        tryVerify(() => loader.item !== null, 3000, "it loads")
        const dial = loader.item
        dial.peakValue = 1000
        // The one hold at the platform's interval, then a tap on the same cell
        hold(page, cell, true)
        mouseRelease(cell)
        compare(dial.peakValue, 1000, "a hold leaves the peak")
        page.closePanel()
        press(cell)
        tryVerify(() => isNaN(dial.peakValue), 3000, "a tap resets it")
    }

    // ---- Copy and paste (section 5)

    function test_copyAndPasteAfterACell() {
        const a = cellOf("A", "digital")
        const page = pageOf({}, docOf([a, cellOf("B")]))
        page.startEditing()
        verify(!find(page, "dashPaste").visible, "nothing to paste yet")
        press(find(page, "dashCell_0_0"))
        verify(!find(page, "panelPaste").enabled, "nothing to paste yet")
        click(page, "panelCopy")
        verify(find(page, "panelPaste").enabled)
        click(page, "panelPaste")
        compare(labels(page), ["A", "A", "B"])
        compare(page.selected, 0, "the menu stays on the source")
        compare(page.doc.pages[0].cells[1].kind, "digital", "the whole cell")
        compare(storedText(page), DashboardBook.serialize(page.doc), "stored")

        page.select(1)
        type(find(page, "panelLabel"), "Copy")
        compare(labels(page), ["A", "Copy", "B"], "the pasted cell is its own")
    }

    function test_theSlotKeepsWhatWasCopied() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelCopy")
        type(find(page, "panelLabel"), "Changed")
        compare(labels(page), ["Changed", "B"])
        click(page, "dashPaste")
        compare(labels(page), ["Changed", "B", "A"], "Paste appends what was copied")
    }

    function test_pasteOutsideEditModeThroughTheMenu() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B")]))
        hold(page, find(page, "dashCell_0_0"))
        mouseRelease(find(page, "dashCell_0_0"))
        click(page, "panelCopy")
        click(page, "panelPaste")
        compare(labels(page), ["A", "A", "B"])
        compare(page.editing, false)
    }

    function test_theSlotSurvivesAPageChange() {
        const d = docOf([cellOf("A")])
        d.pages.push({ name: "P2", cells: [] })
        const page = pageOf({}, d)
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelCopy")
        page.goTo(1)
        tryCompare(page, "pageIndex", 1)
        click(page, "dashPaste")
        compare(page.doc.pages[1].cells.map(c => c.label), ["A"])
    }

    function test_aFullPageRefusesAPaste() {
        const cells = []
        for (let i = 0; i < DashboardBook.maxCells; ++i)
            cells.push(cellOf("C" + i))
        const page = pageOf({}, docOf(cells))
        page.startEditing()
        page.select(0)
        page.copySelected()
        verify(!find(page, "dashPaste").enabled, "the bar's is off")
        verify(!find(page, "panelPaste").enabled, "the menu's is off")
        page.pasteAfter(0)
        compare(page.doc.pages[0].cells.length, DashboardBook.maxCells)
    }

    function test_theOtherFormsCellCantBePasted() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B")]))
        page.startEditing()
        page.select(0)
        page.copied = { form: "phone", cell: cellOf("Other") }
        verify(!find(page, "dashPaste").enabled, "the bar's is off")
        verify(!find(page, "panelPaste").enabled, "the menu's is off")
        page.pasteAfter(0)
        compare(labels(page), ["A", "B"])
        page.copySelected()
        verify(find(page, "panelPaste").enabled, "a copy on this form replaces it")
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
            first.history = [1e9]
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        page.moveTo(1)
        compare(labels(page), ["B", "A"])
        const now = gaugeAt(0).item
        verify(now.peakValue !== 1e9, "no peak of A's")
        verify(now.lowValue !== -1e9, "no low of A's")
        if (now.history !== undefined)
            verify(now.history.indexOf(1e9) < 0, "no trace of A's")
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
        const cap = (Math.max(160, view.height - page.topGap - page.margin) + gap) / tallest
        const pitch = Math.max(1, Math.min((col + gap) / 4, cap))
        compare(cap < (col + gap) / 4, data.capped, "the case means what it says")
        const was = DashboardBook.place(cells, data.before)
        for (let i = 0; i < cells.length; ++i) {
            const c = find(page, "dashCell_0_" + i)
            const b = was[i]
            tryVerify(() => c.width > 0, 3000, "cell " + i + " laid out")
            fuzzyCompare(c.x, b.x * (col + gap), 1e-6, "cell " + i + " x")
            fuzzyCompare(c.y, b.y * pitch, 1e-6, "cell " + i + " y")
            fuzzyCompare(c.width, b.w * col + (b.w - 1) * gap, 1e-6, "cell " + i + " width")
            fuzzyCompare(c.height, data.heights[i] * pitch - gap, 1e-6, "cell " + i + " height")
        }
        compare(page.problem, "", "it reads")
        compare(storedText(page), text, "a read writes nothing")
        verify(page.edit(DashboardBook.renamePage(page.doc, 0, "Q")), "an edit")
        const written = JSON.parse(storedText(page))
        compare(written.format, 2, "the first edit writes format 2")
        compare(written.pages[0].cells[1].h, 12, "with the scaled spans")
    }

    // A digital tile samples only while its page is the one shown
    function test_aDigitalTileOnAnotherPageTakesNoSamples() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const d = docOf([cellOf("A", "digital")])
        d.pages.push({ name: "P2", cells: [cellOf("B", "digital")] })
        const page = pageOf({}, d)
        const sampler = p => find(find(find(page, "dashCell_" + p + "_0"), "gauge").item, "sampler")
        tryVerify(() => sampler(0).running, 3000, "the page shown samples")
        verify(!sampler(1).running, "the other page's tile does not")
        page.goTo(1)
        tryVerify(() => sampler(1).running && !sampler(0).running, 3000, "and it follows the page")
    }
}
