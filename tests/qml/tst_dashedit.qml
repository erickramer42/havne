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
        verify(!find(find(page, "dashCell_0_0"), "cellRemove").visible)
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
        verify(!find(find(page, "dashCell_0_0"), "cellSelected").visible)
        press(find(page, "dashCell_0_0"))
        compare(page.selected, 0)
        click(page, "panelClose")
        compare(page.panel, "")
        click(page, "dashDone")
        verify(!page.editing)
        verify(!bar.visible)
        compare(page.selected, -1)
        compare(storedText(page), "", "no edit, nothing stored")
        // Outside edit mode a tap selects nothing
        press(find(page, "dashCell_0_0"))
        compare(page.selected, -1)
        compare(page.panel, "")
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
        click(page, "moreRemovePage")
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

    // A stored text that can't be read: the first edit keeps it in
    // <form>Unread and stores the edited document; a later one leaves it
    function test_theFirstEditKeepsATextThatCantBeRead_data() {
        return [{ tag: "damaged", text: "{" },
                { tag: "newer", text: JSON.stringify({ format: 2, pages: [] }) }]
    }
    function test_theFirstEditKeepsATextThatCantBeRead(data) {
        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", data.text)
        const page = pageOf({}, null)
        compare(page.problem, data.tag)
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
