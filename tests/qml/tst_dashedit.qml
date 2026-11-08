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
    Component {
        id: sessionComponent
        Settings {
            category: "session"
            property string uri: ""
        }
    }

    property string keptUri: ""

    function initTestCase() {
        keptUri = String(createTemporaryObject(sessionComponent, testCase).value("uri", ""))
    }
    function cleanupTestCase() {
        createTemporaryObject(sessionComponent, testCase).setValue("uri", keptUri)
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
        createTemporaryObject(sessionComponent, testCase).setValue("uri", "")
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
    function cellOf(label, kind) {
        return { kind: kind || "analog", w: 2, h: 4, label: label, refs: ["id:1001"],
                 unit: "RPM", options: { from: 0, to: 8000 } }
    }
    function labels(page) {
        return page.doc.pages[page.pageIndex].cells.map(c => c.label)
    }
    function storedText(page) {
        return find(page, "dashStore")[page.form === "phone" ? "phone" : "desktop"]
    }
    // A press where the item is, once it has stopped moving (a page
    // change slides the view)
    function click(page, name) {
        const item = find(page, name)
        verify(item.visible && item.enabled, name + " can be pressed")
        press(item)
    }
    function press(item) {
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
        const peaked = cellOf("B")
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
        const page = pageOf({}, docOf([cellOf("A")]))
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
        const page = pageOf({}, docOf([cellOf("A")]))
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
    function test_withNoSessionThePickerReadsTheLastUri() {
        const none = pageOf({}, docOf([cellOf("A")]))
        none.startEditing()
        none.pickKind("ring")
        find(none, "dashSignalPanel")
        verify(find(none, "signalNone").visible, "nothing to pick")
        none.destroy()
        wait(0)

        createTemporaryObject(sessionComponent, testCase).setValue("uri", "mock://demo")
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        page.pickKind("ring")
        verify(!find(page, "signalNone").visible)
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
        const zoned = cellOf("A")
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
        const page = pageOf({}, docOf([cellOf("A"), cellOf("D", "digital"),
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
        const two = docOf([cellOf("A")])
        two.pages.push({ name: "More", cells: [cellOf("B")] })
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

    // Peak hold shows only for a kind that draws it
    function test_peakHoldOnlyWhereItDraws() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("R", "ring"), cellOf("D", "digital")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        verify(find(page, "panelPeak").visible, "a dial's")
        press(find(page, "dashCell_0_1"))
        verify(!find(page, "panelPeak").visible, "not a ring's")
        press(find(page, "dashCell_0_2"))
        verify(!find(page, "panelPeak").visible, "not a digital tile's")
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
        const t = cellOf("T")
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
                { tag: "newer", text: JSON.stringify({ format: 2, pages: [] }) },
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

    // The phone: a long press starts edit mode at the cell; the desktop's
    // does nothing
    function test_aLongPressOnThePhoneStartsEditing() {
        const desk = pageOf({}, docOf([cellOf("A"), cellOf("B")]))
        const deskCell = find(desk, "dashCell_0_1")
        mousePress(deskCell)
        wait(1200)
        mouseRelease(deskCell)
        verify(!desk.editing, "not on the desktop")
        desk.destroy()

        const page = pageOf({ form: "phone", width: 384, height: 832 },
                            docOf([cellOf("A"), cellOf("B")]))
        const cell = find(page, "dashCell_0_1")
        tryVerify(() => cell.width > 0, 3000, "laid out")
        mousePress(cell)
        tryVerify(() => page.editing, 3000, "edit mode")
        mouseRelease(cell)
        compare(page.selected, 1)
        compare(page.panel, "cell")
    }
}
