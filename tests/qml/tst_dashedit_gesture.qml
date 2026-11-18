// File/layer: tests/qml/tst_dashedit_gesture.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit_gesture (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit mode (ADR-0057 section 5), the hold and drag, copy and paste, and what each kind's panel shows:
// each control's write and what's stored
TestCase {
    id: testCase
    name: "DashboardEditGesture"
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
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting, 5000, "disconnect ends")
        for (const p of Bridge.plugins())     // the store starts empty
            Bridge.removePlugin(p.sha256)
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

    // A held finger that drifts past the drag distance and lets go over the
    // cell's own place opens the menu, as a still one does: a thumb's tremble
    // on the phone, or a finger on a phone in a holder in a moving car
    function test_aDriftBackOnItsOwnPlaceOpensTheMenu_data() {
        return [{ tag: "phone", phone: true }, { tag: "desktop", phone: false }]
    }
    function test_aDriftBackOnItsOwnPlaceOpensTheMenu(data) {
        const page = pageOf(data.phone ? { form: "phone", width: 384, height: 832 } : {},
                            docOf([cellOf("A"), cellOf("B")]))
        const cell = find(page, "dashCell_0_1")
        hold(page, cell)
        verify(cell.lifted, "the cell lifts")
        const drift = Application.styleHints.startDragDistance * 3
        mouseMove(cell, cell.width / 2 + drift, cell.height / 2 + drift / 2, -1, Qt.LeftButton)
        compare(page.dropAt, 1, "past the drag distance, the drop place is its own")
        mouseRelease(cell)
        compare(page.held, -1)
        compare(page.selected, 1)
        compare(page.panel, "cell", "the menu opens")
        compare(page.doc.pages[0].cells.map(c => c.label), ["A", "B"], "nothing moved")
    }

    // A cell drawn stretched lifts at its true size with the finger on it:
    // a lone cell fills its row on the joined panel, and a hold at its
    // middle is past its true width
    function test_aStretchedCellLiftsUnderTheFinger() {
        const page = pageOf({ form: "phone", width: 384, height: 832 }, docOf([cellOf("A")]))
        const a = find(page, "dashCell_0_0")
        tryVerify(() => a.width > 0, 3000, "laid out")
        verify(waitForPolish(page.Window.window), "laid out")
        const wide = a.width
        const from = centerOf(page, a)   // where the hold presses
        hold(page, a)
        verify(a.width < wide, "the true size while held: " + a.width + " of " + wide)
        const x = from.x + 4 * Application.styleHints.startDragDistance
        mouseMove(page, x, from.y, -1, Qt.LeftButton)
        tryVerify(() => a.carried, 2000, "the cell follows the finger")
        // Its edges as drawn: a lifted cell is scaled up a little
        const left = a.mapToItem(page, 0, 0).x
        const right = a.mapToItem(page, a.width, 0).x
        verify(x >= left - 0.5 && x <= right + 0.5,
               "the finger at " + x + " is on the cell, " + left + " to " + right)
        mouseRelease(page, x, from.y)
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
        // The panel's preview pushes the grid down: bring B into view
        const view = find(page, "dashPage_0")
        view.contentY = Math.max(0, b.mapToItem(view.contentItem, 0, 0).y - 10)
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
        // Tall enough that the cell panel, with its zone fields, leaves cell C on screen (the switch row sits above the grid)
        const page = pageOf({ height: 1100 }, docOf([cellOf("A"), cellOf("B"), cellOf("C")]))
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
        verify(!find(page, "dashPasteCell_0").visible, "nothing to paste yet")
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
        click(page, "dashPasteCell_0")
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
        click(page, "dashPasteCell_1")
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
        verify(!find(page, "dashPasteCell_0").visible, "no + cell to paste into")
        verify(!find(page, "panelPaste").enabled, "the menu's is off")
        page.pasteAfter(0)
        compare(page.doc.pages[0].cells.length, DashboardBook.maxCells)
    }

    function test_theOtherFormsCellCantBePasted() {
        const page = pageOf({}, docOf([cellOf("A"), cellOf("B")]))
        page.startEditing()
        page.select(0)
        page.copied = { form: "phone", cell: cellOf("Other") }
        verify(find(page, "dashPasteCell_0").visible, "the + cell splits")
        verify(!find(page, "dashPasteCell_0").enabled, "its Paste is off")
        verify(!find(page, "panelPaste").enabled, "the menu's is off")
        page.pasteAfter(0)
        compare(labels(page), ["A", "B"])
        page.copySelected()
        verify(find(page, "panelPaste").enabled, "a copy on this form replaces it")
    }

    // ---- What each kind's panel shows

    // Every field the panel makes, as objectNames: the Color swatches and
    // Night's note are a row's parts, so the row (panelColors) stands for them
    function panelFields(page) {
        const found = []
        const walk = item => {
            const n = item.objectName
            if (n.startsWith("panel") && !(n.startsWith("panelColor") && n !== "panelColors"))
                found.push({ name: n, shown: item.visible })
            for (const c of item.children)
                walk(c)
        }
        walk(find(page, "dashCellPanel"))
        return found
    }
    readonly property var anyCell: ["panelClose", "panelCopy", "panelPaste", "panelW", "panelH",
                                    "panelEarlier", "panelLater", "panelStart", "panelEnd", "panelRemove"]
    // panelSignalName shows while the cell's ref has a row: the demo's RPM
    // (id:1001) has one with no session once a session has run, which the field
    // table's case sets up itself
    readonly property var withSignal: ["panelPreview", "panelLabel", "panelSignal", "panelKind", "panelFrom",
                                       "panelFromCaption", "panelTo", "panelToCaption", "panelSignalName"]
    // What the value's text and tone read: every kind that draws a value.
    // panelAlert shows while the cell has a level, which the table's case gives it
    readonly property var valueDrawn: ["panelAlert", "panelColdBelow", "panelWarnBelow", "panelDangerBelow",
                                       "panelWarnFrom", "panelDangerFrom", "panelDecimals", "panelColors",
                                       "panelColdBelowCaption", "panelWarnBelowCaption",
                                       "panelDangerBelowCaption", "panelWarnFromCaption",
                                       "panelDangerFromCaption", "panelDecimalsCaption"]
    // A new kind or field fails test_eachKindsPanelShowsOnlyItsFields until it is listed here
    readonly property var fieldsOfKind: ({
        "analog": anyCell.concat(withSignal, valueDrawn, ["panelPeak", "panelMajorStep", "panelCaption"]),
        "ring": anyCell.concat(withSignal, valueDrawn, ["panelPeak"]),
        "bar": anyCell.concat(withSignal, valueDrawn, ["panelPeak", "panelVertical", "panelMarks"]),
        "segments": anyCell.concat(withSignal, valueDrawn, ["panelPeak", "panelCells", "panelRise"]),
        "digital": anyCell.concat(withSignal, valueDrawn, ["panelDescription"]),
        "seven-segment": anyCell.concat(withSignal, valueDrawn, ["panelDigits"]),
        "sparkline": anyCell.concat(withSignal, valueDrawn, ["panelWindow"]),
        "shift-light": anyCell.concat(withSignal, ["panelShiftHint", "panelCount"]),
        "blank": anyCell
    })

    // Edit mode says how a gauge is reached, until a panel opens
    function test_editModeHintsAtClickAndHoldUntilAPanelOpens() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        const hint = find(page, "dashEditHint")
        verify(!hint.visible, "not outside edit mode")
        page.startEditing()
        verify(hint.visible)
        verify(hint.text.startsWith("Click a gauge to change it"))
        press(find(page, "dashCell_0_0"))
        verify(!hint.visible, "a panel is open")
    }

    // The panel's head draws the cell it edits, live
    function test_thePanelShowsTheCellItEdits() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const preview = find(page, "panelPreview")
        verify(preview.visible)
        compare(preview.kind, "analog")
        compare(preview.to, find(page, "dashCell_0_0").to)
        type(find(page, "panelTo"), "5000")
        compare(preview.to, 5000, "follows an edit")
    }

    // Close takes the page back to the cell the panel edited
    function test_closingThePanelScrollsToItsCell() {
        const cells = []
        for (let i = 0; i < 12; ++i)
            cells.push(cellOf("C" + i))
        const page = pageOf({}, docOf(cells))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        const view = find(page, "dashPage_0")
        view.contentY = view.contentHeight   // away from cell 0
        const away = view.contentY
        verify(away > 0, "the page scrolled away")
        // Not through click(): its own scroll to the button and back would
        // undo the page's
        page.closePanel()
        tryVerify(() => view.contentY < away / 2, 3000, "back near cell 0")
    }

    // An end shows to the signal's decimals plus one, never its stored noise
    function test_anEndShowsToTheDecimalsPlusOne() {
        const own = cellOf("Speed", "analog")
        own.options = { from: 0, to: 161.5565, decimals: 1 }
        const page = pageOf({}, docOf([own]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        compare(find(page, "panelTo").text, "161.56")
    }

    function test_everyDrawnKindIsInTheFieldTable() {
        const page = pageOf({}, docOf([cellOf("A", "analog")]))
        compare(Object.keys(fieldsOfKind).sort(), page.drawnKinds.slice().sort())
    }

    function test_eachKindsPanelShowsOnlyItsFields_data() {
        return Object.keys(fieldsOfKind).map(k => ({ tag: k, kind: k }))
    }
    function test_eachKindsPanelShowsOnlyItsFields(data) {
        // The demo's rows stay in the model after a session ends: put them there
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        Bridge.disconnectSession()
        tryVerify(() => !Bridge.disconnecting && !Bridge.connected, 5000, "no session again")
        wait(0)
        const cell = data.kind === "blank" ? { kind: "blank", w: 2, h: 4, refs: [] }
                                           : cellOf("A", data.kind)
        if (data.kind !== "blank")
            cell.options.dangerFrom = 7000
        const page = pageOf({}, docOf([cell, cellOf("B")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        compare(page.panel, "cell")
        const found = panelFields(page)
        const shown = found.filter(f => f.shown).map(f => f.name).sort()
        compare(shown, fieldsOfKind[data.kind].slice().sort())
        const listed = Object.values(fieldsOfKind).reduce((all, l) => all.concat(l), [])
        for (const f of found)
            verify(listed.indexOf(f.name) >= 0, f.name + " is in the table for some kind")
    }

    function test_theSignalNameIsHiddenWhenTheCellsRowDoesNotExist() {
        const cell = cellOf("A", "analog")
        cell.refs = ["id:987654"]
        const page = pageOf({}, docOf([cell, cellOf("B")]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        verify(!find(page, "panelSignalName").visible, "no row, no name")
        verify(find(page, "panelSignal").visible, "the signal's own row still shows")
    }

    function test_aHiddenFieldKeepsItsStoredValueThroughAKindChange() {
        const own = cellOf("Coolant", "analog")
        own.options = { from: 0, to: 8000, coldBelow: 500, warnBelow: 800, dangerBelow: 400,
                        warnFrom: 6000, dangerFrom: 7000, decimals: 2 }
        const page = pageOf({}, docOf([own]))
        page.startEditing()
        press(find(page, "dashCell_0_0"))
        click(page, "panelKind")
        click(page, "addKind_shift-light")
        compare(page.doc.pages[0].cells[0].kind, "shift-light")
        verify(!find(page, "panelColdBelow").visible && !find(page, "panelDecimals").visible)
        for (const name of ["coldBelow", "warnBelow", "dangerBelow", "warnFrom", "dangerFrom", "decimals"])
            compare(page.doc.pages[0].cells[0].options[name], own.options[name], name + " stays stored")
        click(page, "panelKind")
        click(page, "addKind_analog")
        verify(find(page, "panelColdBelow").visible, "and shows again")
        compare(find(page, "panelDecimals").value, 2)
        compare(find(page, "panelDangerFrom").text, "7000")
        for (const name of ["coldBelow", "warnBelow", "dangerBelow", "warnFrom", "dangerFrom", "decimals"])
            compare(page.doc.pages[0].cells[0].options[name], own.options[name], name + " is back")
    }
}
