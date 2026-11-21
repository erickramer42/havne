// File/layer: tests/qml/tst_dashedit_panel.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit_panel (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit mode (ADR-0057 section 5), the cell's panel, the pages' controls, Reset and Done:
// each control's write and what's stored
DashEditKit {
    id: testCase
    name: "DashboardEditPanel"

    // Edit shows the bar and each cell's Remove; Done leaves edit mode,
    // and nothing was waiting to be saved
    // Two pages: three cells and a tall one, then one
    function twoPages() {
        const d = docOf([cellOf("A"), cellOf("B"),
                         { kind: "bar", w: 2, h: 2, label: "C", refs: ["id:1001"], unit: "RPM",
                           options: { from: 0, to: 8000 } },
                         { kind: "digital", w: 1, h: 20, label: "T", refs: ["id:1001"], unit: "RPM",
                           options: { from: 0, to: 8000 } }])
        d.pages[0].name = "One"
        d.pages.push({ name: "Two", cells: [cellOf("D")] })
        return d
    }
    // An option the cell leaves unset shows in its box as the cell draws it,
    // in edit mode and out of it; a stored cell taller than the desktop's 16
    // rows shows its own height
    function test_theBoxesShowWhatTheCellDraws() {
        const lights = cellOf("S", "shift-light")
        const tall = Object.assign(cellOf("T"), { h: 20 })
        const page = pageOf({ width: 400 }, docOf([lights, tall]))
        verify(page.compact)
        compare(find(page, "dashCell_0_0").count, 11, "a compact page draws 11")
        page.select(0)
        compare(find(page, "panelCount").value, 11, "out of edit mode")
        page.closePanel()
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        compare(find(page, "panelCount").value, 11, "in edit mode")
        spin(find(page, "panelCount"), 12)
        compare(page.doc.pages[0].cells[0].options.count, 12, "one more than shown")

        page.select(1)
        const rows = find(page, "panelH")
        compare(rows.value, 20, "its own height")
        spin(rows, 19)
        compare(page.doc.pages[0].cells[1].h, 19)
        page.select(0)
        compare(rows.to, 16, "a cell of 16 or fewer stops at 16")
    }

    // The phone's row unit is a quarter of a column: the panel reads the
    // height in columns, as Columns is, and the desktop keeps rows
    function test_thePhonesHeightReadsInColumns() {
        const cell = Object.assign(cellOf("A"), { w: 4, h: 18 })
        const phone = pageOf({ form: "phone", width: 384, height: 832 }, docOf([cell]))
        phone.startEditing()
        phone.select(0)
        const box = find(phone, "panelH")
        compare(box.Accessible.name, "Height")
        compare(box.textFromValue(box.value, box.locale), "4.5", "18 rows is 4.5 columns")
        compare(box.value, 18, "the stored value is rows")
        const desktop = pageOf({}, docOf([Object.assign(cellOf("B"), { h: 6 })]))
        desktop.startEditing()
        desktop.select(0)
        const rows = find(desktop, "panelH")
        compare(rows.Accessible.name, "Rows")
        compare(rows.textFromValue(rows.value, rows.locale), "6")
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
        page.addFlowPart.startAdd()
        verify(find(page, "addKind_" + data.kind), "a tile for it")
        verify(find(page, "preview_" + data.kind).item, "its preview loads")
        compare(page.newSpans[data.kind].join("x"), data.span.join("x"))
        compare(page.drawnKinds.indexOf(data.kind) >= 0, true)
        if (data.kind === "sparkline") {
            const spark = find(page, "preview_sparkline").item
            verify(spark.samples.length > 20, "a still trace to look at")
            wait(300)
            compare(spark.samples.length, 240, "and it reads nothing of its own")
            verify(spark.tracePoints(200, 50)[0][0].x < 2, "the trace spans the preview")
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

    // On the phone with no session the connect list owns the page's top: the
    // empty-page note waits until a session runs, so it never overdraws the list
    function test_theEmptyPageNoteYieldsToTheConnectList() {
        verify(!Bridge.connected, "no session")
        const page = pageOf({ form: "phone", width: 384, height: 832 }, docOf([]))
        verify(page.connectShown, "the list shows")
        verify(!find(page, "dashEmptyPage").visible, "the note stays off while the list shows")
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
        page.addFlowPart.startAdd()
        compare(page.panel, "", "Add opens nothing")
    }

    // The pages' row: rename, move, remove (never the last); on the
    // desktop a page is added from the strip's "+" tab, and the tabs follow
    function test_thePagesControls() {
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        verify(!find(page, "dashMoreRow").visible)
        compare(find(page, "dashMore").text, "Page")
        click(page, "dashMore")
        verify(find(page, "dashMoreRow").visible)
        verify(!find(page, "moreAddPage").visible, "the desktop's row has no Add page")
        verify(!find(page, "moreRemovePage").enabled, "never the last page")
        click(page, "dashPageAdd")
        click(page, "preset_blank")
        compare(page.doc.pages.length, 2)
        compare(page.doc.pages[1].name, "Page 2")
        compare(page.pageIndex, 1, "the new page shows")
        verify(find(page, "dashPageTab_1").checked, "its tab is the shown one")
        type(find(page, "moreRename"), "Temps")
        compare(page.doc.pages[1].name, "Temps")
        compare(find(page, "moreRename").text, "Temps")
        verify(!find(page, "moreRight").enabled)
        click(page, "moreLeft")
        compare(page.doc.pages.map(p => p.name), ["Temps", "Test"])
        compare(page.pageIndex, 0, "the page moved shows")
        compare(find(page, "dashPageTab_0").text, "Temps", "its tab moved with it")
        verify(find(page, "dashPageTab_0").checked)
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
        verify(find(page, "dashPageTab_0").checked, "the tab now at the index is shown")
        verify(!findChild(page, "dashPageTab_1"), "one tab left")
        compare(storedText(page), DashboardBook.serialize(page.doc))
        // A page change drops the selection
        click(page, "dashPageAdd")
        click(page, "preset_blank")
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
            page.addFlowPart.startAdd()
            page.addFlowPart.pickKind("ring")
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

    // Adding or removing a page makes every page again. The cells released
    // live on, their bindings running, until their deferred delete, one turn
    // past their page's: such a cell stays out of edit mode and builds nothing,
    // though the page shown is again the one whose number it holds
    function test_aReleasedCellTakesNoPartInThePage_data() {
        return [{ tag: "in the same turn", turn: false },
                { tag: "once its page is deleted", turn: true }]
    }
    function test_aReleasedCellTakesNoPartInThePage(data) {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B")]))
        page.startEditing()
        const cell = find(page, "dashCell_0_0")
        verify(cell.editable, "the cell is in edit mode")
        verify(find(cell, "cellRemoveSlot").active, "with its Remove button")
        page.editsPart.addPage()
        compare(page.pageIndex, 1, "the new page shows")
        verify(find(page, "dashCell_0_0") !== cell, "the first page's cells are new ones")
        if (data.turn) {
            wait(0)
            verify(cell.parent === null, "the page is gone and the cell waits for its own delete")
        }
        page.editsPart.removePage()
        compare(page.pageIndex, 0, "the first page shows again")
        verify(find(page, "dashCell_0_0").editable, "its own cell is in edit mode")
        verify(!cell.editable, "the released cell stays out of it")
        verify(!find(cell, "cellRemoveSlot").active, "and builds no Remove button")
        verify(!find(cell, "gauge").active, "nor a gauge")
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
        page.addFlowPart.startAdd()
        compare(page.panel, "add")
        page.goTo(0)
        compare(page.panel, "")
        page.addFlowPart.startAdd()
        page.addFlowPart.pickKind("bar")
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
    // The built-in layout an unreadable text gave isn't stored as the user's
    // by an undo: "" is the built-in layout, one a later version can change
    function test_undoOverAnUnreadableTextStoresNothing() {
        const stored = createTemporaryObject(storeComponent, testCase)
        stored.setValue("desktop", "{")
        const page = pageOf({}, null)
        compare(page.problem, "damaged")
        page.startEditing()
        page.editsPart.removeCell(0)
        verify(storedText(page) !== "", "the edit is stored")
        page.historyPart.undo()
        compare(storedText(page), "", "the built-in layout again, as nothing")
        compare(find(page, "dashStore").desktopUnread, "{", "the unread text is kept")
    }
    // The strip's state comes from the pages and edit mode, never from
    // whether the page is on screen: another page in front moves no layout
    function test_theStripDoesntFollowThePagesVisibility() {
        const page = pageOf({}, twoPages())
        verify(page.stripShown, "two pages: the strip")
        const top = page.gridTop
        page.visible = false
        verify(page.stripShown, "still the strip, off screen")
        compare(page.gridTop, top, "and the grid's top is where it was")
        page.visible = true
        compare(page.gridTop, top)
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

    // With no session the stepper reads the listed row's decimals, not 0
    function test_theDecimalsStepperStartsAtTheListedRowsWithNoSession() {
        // A session under raw leaves the hub with no rows, whatever an earlier case read
        compare(Bridge.connectSession("mock://demo?profile=raw"), "", "raw connects")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.connected, 5000, "disconnected")
        const maf = cellOf("MAF")
        maf.refs = Array.from(Bridge.gaugeSignals["maf"])
        maf.unit = "g/s"
        maf.options = { from: 0, to: 300 }
        const page = pageOf({}, docOf([maf]))
        page.offlineUri = "mock://?profile=obd2"
        page.startEditing()
        const rows = Bridge.profileSignals("mock://?profile=obd2")
        const row = rows.find(r => maf.refs.indexOf(r.ref) >= 0)
        verify(row !== undefined && row.decimals > 0, "the listed row has decimals")
        press(find(page, "dashCell_0_0"))
        const box = find(page, "panelDecimals")
        verify(find(page, "dashCellPanel").entry === null, "no session, no hub row")
        compare(box.value, row.decimals)
        verify(page.doc.pages[0].cells[0].options.decimals === undefined, "nothing stored")
        // A stored value wins
        spin(box, 0)
        compare(box.value, 0)
    }

    // A profile with no number to show says so, not "connect"
    function test_aProfileWithNothingToPickSaysSo() {
        compare(Bridge.connectSession("mock://demo?profile=raw"), "", "raw connects")
        const page = pageOf({}, docOf([cellOf("A")]))
        page.startEditing()
        page.addFlowPart.startAdd()
        page.addFlowPart.pickKind("ring")
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
        click(first, "dashPageAdd")
        click(first, "preset_blank")
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
}
