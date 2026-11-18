// File/layer: tests/qml/tst_dashedit_undo.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashedit_undo (test_qml)

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import App
import AppTest

// The Dashboard's edit mode (ADR-0057 section 5), undo and the desktop's phone frame:
// each control's write and what's stored
TestCase {
    id: testCase
    name: "DashboardEditUndo"
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
    // A scratch file a layout is written to and read back from
    function layoutUrl(name) {
        return Harness.writeScratchFile(name, "")
    }

    // ---- Undo (ADR-0057's appendix "undo in edit mode as built")

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
    readonly property var speedRow: ({ ref: "id:1001", name: "Speed", decodedUnit: "km/h",
                                       decodedLow: 0, decodedHigh: 255, isText: false, isEnum: false })

    // Each kind of step, made as the page's own controls make it, is undone
    // (the document, the stored text and the page shown) and then redone
    function test_undoRestoresEachStep_data() {
        return [
            { tag: "add a blank", words: "added Blank", make: p => { p.addFlowPart.startAdd(); p.addFlowPart.pickKind("blank") } },
            { tag: "add a signal", words: "added Speed",
              make: p => { p.addKind = "digital"; p.addFlowPart.pickSignal(speedRow) } },
            { tag: "remove", words: "removed B", make: p => p.editsPart.removeCell(1) },
            { tag: "move earlier", words: "moved B", make: p => { p.select(1); p.editsPart.moveTo(0) } },
            { tag: "move later", words: "moved B", make: p => { p.select(1); p.editsPart.moveTo(2) } },
            { tag: "to the start", words: "moved C", make: p => { p.select(2); p.editsPart.moveTo(0) } },
            { tag: "to the end", words: "moved A", make: p => { p.select(0); p.editsPart.moveTo(3) } },
            { tag: "drop", words: "moved A", make: p => {
                p.held = 0
                p.dropAt = 2
                find(p, "dashPage_0").letGo(false)
            } },
            { tag: "resize", words: "resized A", make: p => { p.select(0); p.editsPart.resize(3, 4) } },
            { tag: "resize a tall cell", words: "resized T", make: p => { p.select(3); p.editsPart.resize(1, 19) } },
            { tag: "a stepped option", words: "changed A's decimals",
              make: p => { p.select(0); p.editsPart.setOptions({ decimals: 2 }) } },
            { tag: "a typed option", words: "changed A's range",
              make: p => { p.select(0); p.editsPart.setOptions({ from: 100 }) } },
            { tag: "zones", words: "changed A's zones",
              make: p => { p.select(0); p.editsPart.setOptions({ warnFrom: 6000 }) } },
            { tag: "the label", words: "changed A's label", make: p => { p.select(0); p.editsPart.setLabel("Z") } },
            { tag: "another signal", words: "changed A's signal",
              make: p => { p.select(0); p.addKind = ""; p.addFlowPart.pickSignal(speedRow) } },
            { tag: "paste", words: "pasted A",
              make: p => { p.select(0); p.copySelected(); p.pasteAfter(0) } },
            { tag: "add a page", words: "added a page", pageAfter: 2, make: p => p.editsPart.addPage() },
            { tag: "rename the page", words: "renamed the page", make: p => p.editsPart.renamePage("New") },
            { tag: "move the page", words: "moved the page", pageAfter: 1, make: p => p.editsPart.movePage(1) },
            { tag: "remove a page", words: "removed the page Two", page: 1,
              make: p => { p.goTo(1); p.editsPart.removePage() } },
            { tag: "reset", words: "reset the layout", make: p => p.layoutFilePart.resetForm() }
        ]
    }
    function test_undoRestoresEachStep(data) {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        const startText = storedText(page)
        const startDoc = DashboardBook.serialize(page.doc)
        data.make(page)
        const afterText = storedText(page)
        const afterDoc = DashboardBook.serialize(page.doc)
        verify(afterDoc !== startDoc, "the step changed the document")
        compare(page.historyPart.undoStack.length, 1, "one step")
        compare(page.historyPart.undoWords, data.words)
        verify(page.historyPart.undoLine === "", "edit mode shows no line for a step")

        click(page, "dashUndo")
        compare(DashboardBook.serialize(page.doc), startDoc, "undone")
        compare(storedText(page), startText, "the text from before is stored")
        compare(page.pageIndex, data.page ?? 0, "the step's page shows")
        compare(page.historyPart.undoLine, "Undid: " + data.words)
        verify(!page.historyPart.canUndo)

        click(page, "dashRedo")
        compare(DashboardBook.serialize(page.doc), afterDoc, "redone")
        compare(storedText(page), afterText, "the text after it is stored")
        compare(page.pageIndex, data.pageAfter ?? 0, "the page after it shows")
        compare(page.historyPart.undoLine, "Redid: " + data.words)
        verify(page.historyPart.canUndo && !page.historyPart.canRedo)
    }

    // A field still open commits as its own step, which Undo then takes
    // back: the step before it stays
    function test_anOpenFieldCommitsBeforeAnUndo() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        page.select(0)
        page.editsPart.resize(3, 4)
        const field = find(page, "panelLabel")
        field.forceActiveFocus()
        field.text = "Typed"
        page.historyPart.undo()
        compare(labels(page), ["A", "B", "C", "T"], "the label committed, then undone")
        compare(page.doc.pages[0].cells[0].w, 3, "the resize stays")
        compare(page.historyPart.undoWords, "resized A")
        compare(page.historyPart.redoWords, "changed A's label")
    }

    function test_aNewStepClearsRedo() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        page.editsPart.removeCell(0)
        page.historyPart.undo()
        verify(page.historyPart.canRedo)
        page.editsPart.removeCell(1)
        verify(!page.historyPart.canRedo, "a new step after an undo")
        compare(labels(page), ["A", "C", "T"])
    }

    // A SpinBox stepped is one undo; another cell, quiet past the group's
    // time or a typed field's commits end the run
    function test_aRunOfStepsIsOneUndo() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        page.select(0)
        const start = storedText(page)
        for (let v = 1; v <= 5; ++v)
            spin(find(page, "panelDecimals"), v)
        compare(page.historyPart.undoStack.length, 1, "five steps, one run")
        compare(page.doc.pages[0].cells[0].options.decimals, 5)
        page.historyPart.undo()
        compare(storedText(page), start, "one Undo back to the start")
        compare(page.selected, 0, "the panel stays on its cell")

        spin(find(page, "panelDecimals"), 1)
        page.select(1)
        spin(find(page, "panelDecimals"), 1)
        page.select(0)
        spin(find(page, "panelDecimals"), 2)
        compare(page.historyPart.undoStack.length, 3, "a step on another cell between them")

        page.stopEditing()
        page.startEditing()
        page.select(0)
        // From 3: the cell's decimals are 2 here, and a run back to them leaves nothing
        for (let v = 3; v <= 7; ++v) {
            if (v === 5)
                page.historyPart.lastAt -= page.historyPart.groupMs + 1
            spin(find(page, "panelDecimals"), v)
        }
        compare(page.historyPart.undoStack.length, 2, "quiet past the group's time")

        page.stopEditing()
        page.startEditing()
        page.select(0)
        type(find(page, "panelFrom"), "100")
        type(find(page, "panelFrom"), "200")
        compare(page.historyPart.undoStack.length, 2, "a typed field commits once each time")
    }

    // A run back where it began leaves no step, since its Undo would change nothing
    function test_aRunThatEndsWhereItBeganLeavesNothing() {
        const a = cellOf("A")
        a.options.decimals = 2
        const page = storedPageOf({}, docOf([a, cellOf("B")]))
        page.startEditing()
        page.select(0)
        spin(find(page, "panelDecimals"), 3)
        verify(page.historyPart.canUndo)
        spin(find(page, "panelDecimals"), 2)
        verify(!page.historyPart.canUndo, "back where it began")
        spin(find(page, "panelDecimals"), 3)
        compare(page.historyPart.undoStack.length, 1, "a later step starts a run of its own")
    }

    // Standing up changes the span and the option in one write: one Undo
    function test_standingUpIsOneStep() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        press(find(page, "dashCell_0_2"))
        const start = storedText(page)
        click(page, "panelVertical")
        const c = page.doc.pages[0].cells[2]
        compare([c.w, c.h, c.options.vertical], [1, 4, true])
        compare(page.historyPart.undoStack.length, 1)
        compare(page.historyPart.undoWords, "changed C's standing up")
        page.historyPart.undo()
        compare(storedText(page), start, "the span and the option both back")
    }

    // Done ends the history; a change from the menu outside edit mode is
    // a step, which Edit then Undo takes back
    function test_doneEndsTheHistory() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        page.editsPart.removeCell(0)
        page.editsPart.removeCell(0)
        page.historyPart.undo()
        verify(page.historyPart.canUndo && page.historyPart.canRedo)
        click(page, "dashDone")
        verify(!page.historyPart.canUndo && !page.historyPart.canRedo, "Done empties both")
        compare(page.historyPart.undoLine, "", "and the line")

        page.select(0)
        click(page, "panelLater")
        compare(labels(page), ["C", "B", "T"])
        click(page, "panelClose")
        click(page, "dashEdit")
        click(page, "dashUndo")
        compare(labels(page), ["B", "C", "T"], "the menu's step undone after Edit")
    }

    // Leaving the Dashboard ends the history as Done does, so a step made
    // outside edit mode (a panel's Remove) isn't undone much later
    function test_leavingTheDashboardEndsTheHistory() {
        const page = storedPageOf({}, twoPages())
        page.select(0)
        click(page, "panelLater")
        verify(page.historyPart.canUndo, "the menu's step is undoable")
        page.visible = false
        verify(!page.historyPart.canUndo && !page.historyPart.canRedo, "hidden: nothing to undo")
        compare(page.historyPart.undoLine, "")
        page.visible = true
        verify(!page.historyPart.canUndo, "and still none when it comes back")
    }

    // Edit mode stays open while the page is hidden, so its history does too
    function test_leavingTheDashboardMidEditKeepsTheHistory() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        page.select(0)
        click(page, "panelLater")
        verify(page.historyPart.canUndo, "an edit step")
        page.visible = false
        verify(page.editing, "edit mode stays open")
        verify(page.historyPart.canUndo, "and its history")
        page.visible = true
        verify(page.editing && page.historyPart.canUndo, "back to the bar with Undo enabled")
        verify(findChild(page, "dashUndo").enabled, "Undo's button is on")
        page.stopEditing()
    }

    function test_theHistoryKeepsFifty() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        for (let i = 1; i <= 51; ++i)
            page.editsPart.renamePage("N" + i)
        compare(page.historyPart.undoStack.length, page.historyPart.undoLimit)
        for (let i = 0; i < 50; ++i)
            page.historyPart.undo()
        compare(page.doc.pages[0].name, "N1", "back to the first rename, not before it")
        verify(!page.historyPart.canUndo)
    }

    // On a fresh install nothing is stored: undoing the first change stores
    // nothing again, so a later version's built-in layout still reaches it
    function test_undoingTheFirstChangeStoresNothing() {
        const page = pageOf({})
        compare(storedText(page), "")
        const builtIn = DashboardBook.serialize(page.doc)
        page.startEditing()
        page.editsPart.removeCell(0)
        verify(storedText(page) !== "")
        page.historyPart.undo()
        compare(storedText(page), "", "nothing stored")
        compare(DashboardBook.serialize(page.doc), builtIn, "the built-in layout shows")
        page.historyPart.redo()
        compare(storedText(page), DashboardBook.serialize(page.doc), "the change stored again")
    }

    // The cell panel follows its cell through an undo, and closes rather
    // than show another cell at its old index
    function test_theSelectionFollowsAnUndo() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        page.select(1)
        page.editsPart.resize(3, 4)
        page.historyPart.undo()
        compare([page.panel, page.selected], ["cell", 1], "an undone resize keeps the panel")

        page.select(0)
        page.editsPart.moveTo(2)
        compare(page.selected, 2)
        page.historyPart.undo()
        compare([page.panel, page.selected], ["cell", 0], "an undone move follows its cell")
        page.historyPart.redo()
        compare([page.panel, page.selected], ["cell", 2], "and a redone one")

        page.select(2)
        page.editsPart.removeCell(0)
        compare(page.selected, 1)
        page.historyPart.undo()
        compare([page.panel, page.selected], ["", -1], "an undone Remove of another cell closes it")

        page.addFlowPart.startAdd()
        page.addFlowPart.pickKind("blank")
        compare(page.panel, "cell")
        page.historyPart.undo()
        compare([page.panel, page.selected], ["", -1], "an undone Add closes it")

        page.addFlowPart.startAdd()
        page.historyPart.undo()
        compare(page.panel, "", "an undo closes Add's panel")
    }

    // Outside edit mode a step shows the line with Undo, which a press
    // takes; it hides after its time. In edit mode only an undo or a redo
    // shows it, with the keys on the desktop
    function test_theLineOutsideEditMode() {
        // A long time while the presses land, short only before the step that must hide
        const page = storedPageOf({ undoLineMs: 60000 }, twoPages())
        const line = find(page, "dashUndoLine")
        page.select(0)
        click(page, "panelLater")
        verify(line.visible)
        compare(find(page, "dashUndoLineText").text, "Moved A")
        verify(!find(page, "dashUndoLineKeys").visible, "no keys outside edit mode")
        compare(find(page, "dashUndoLineUndo").Accessible.name, "Undo: moved A")
        click(page, "dashUndoLineUndo")
        compare(labels(page), ["A", "B", "C", "T"])
        compare(find(page, "dashUndoLineText").text, "Undid: moved A")
        page.historyPart.undoLineMs = 300
        tryVerify(() => !line.visible, 3000, "the line hides after its time")

        page.startEditing()
        page.editsPart.removeCell(0)
        verify(!line.visible, "a step in edit mode shows none")
        click(page, "dashUndo")
        verify(line.visible)
        verify(!find(page, "dashUndoLineUndo").visible, "the bar has Undo")
        const keys = find(page, "dashUndoLineKeys")
        verify(keys.visible && keys.text.indexOf("Z") >= 0, "the keys: " + keys.text)
    }

    // The line floats over the pages' foot: the grid sits where it sat
    // before the line came and after it goes
    function test_theLineMovesNothing() {
        const page = storedPageOf({ form: "phone", width: 384, height: 832, undoLineMs: 60000 },
                                  twoPages())
        const cell = find(page, "dashCell_0_0")
        tryVerify(() => cell.width > 0, 3000, "laid out")
        verify(waitForPolish(page.Window.window), "laid out")
        const line = find(page, "dashUndoLine")
        const at = () => cell.mapToItem(page, 0, 0).y
        const before = at()
        verify(!line.visible)
        page.historyPart.showLine("Undid: removed A")
        verify(line.visible)
        verify(waitForPolish(page.Window.window), "laid out")
        compare(at(), before, "no jump with the line")
        page.historyPart.undoLine = ""
        verify(waitForPolish(page.Window.window), "laid out")
        compare(at(), before, "no jump without it")
    }

    // A cell whose signal the session lacks says why, inside a narrow cell
    // at the title's width too, and a cell made under obd2 reads under mock
    // through its counterpart
    function test_aCellWithoutItsSignalSaysWhy() {
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const battery = Bridge.gaugeSignals["battery"]
        const page = pageOf({ form: "phone", width: 384, height: 832 }, docOf([
            { kind: "seven-segment", w: 2, h: 6, label: "Battery Voltage", refs: [battery[1]],
              unit: "V", options: {} },
            { kind: "seven-segment", w: 2, h: 6, label: "Engine oil pressure",
              refs: ["key:user_oil_pressure"], unit: "kPa", options: {} }]))
        const battCell = find(page, "dashCell_0_0")
        const oilCell = find(page, "dashCell_0_1")
        tryVerify(() => battCell.hasValue, 5000, "the counterpart gives the value")
        verify(!find(battCell, "gaugeNote").visible, "no note with a value")
        const note = find(oilCell, "gaugeNote")
        tryVerify(() => oilCell.width > 0, 3000, "laid out")
        verify(note.visible, "says why")
        verify(note.text.indexOf("not in mock") >= 0, "names the session's profile: " + note.text)
        const right = note.mapToItem(oilCell, note.width, 0).x
        verify(right <= oilCell.width + 0.5, "inside its cell: " + right + " of " + oilCell.width)
        verify(note.contentWidth <= note.width + 0.5, "whole: " + note.contentWidth + " in " + note.width)
        verify(!note.truncated || note.lineCount === 2, "two lines at most")
    }

    // In Edit a title stops short of its cell's Remove button, cut with an
    // ellipsis, in each kind that has a title line
    function test_aTitleElidesBeforeItsRemoveButton_data() {
        return [{ tag: "digits", kind: "seven-segment" }, { tag: "bar", kind: "bar" },
                { tag: "cells", kind: "segments" }, { tag: "sparkline", kind: "sparkline" }]
    }
    function test_aTitleElidesBeforeItsRemoveButton(data) {
        // With a value the title has its line to itself
        compare(Bridge.connectSession("mock://demo"), "", "mock://demo connects")
        const cells = ["Engine oil pressure", "Battery Voltage", "Transmission fluid temp"].map(
            l => ({ kind: data.kind, w: 2, h: 6, label: l, refs: Bridge.gaugeSignals["rpm"],
                    unit: "RPM", options: { from: 0, to: 8000 } }))
        const page = pageOf({ form: "phone", width: 384, height: 832 }, docOf(cells))
        page.startEditing()
        for (let i = 0; i < 3; ++i) {
            const card = find(page, "dashCell_0_" + i)
            tryVerify(() => card.width > 0 && card.hasValue, 5000, "laid out with a value")
            const remove = find(card, "cellRemove")
            const title = findChild(card, "readoutTitle") || find(card, "gaugeTitle")
            verify(title.visible, "the title shows")
            const edge = title.mapToItem(page, title.width, 0).x
            const button = remove.mapToItem(page, 0, 0).x
            verify(edge <= button, cells[i].label + ": the title ends at " + edge
                   + ", the button begins at " + button)
            verify(title.truncated, cells[i].label + " is cut with an ellipsis")
        }
    }

    // Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y on the desktop form, in edit mode or
    // out of it; never on the phone form or in a field with the focus
    function test_theKeys() {
        const page = storedPageOf({}, twoPages())
        page.forceActiveFocus()
        page.startEditing()
        page.editsPart.removeCell(0)
        keyClick("z", Qt.ControlModifier)
        compare(labels(page), ["A", "B", "C", "T"], "Ctrl+Z")
        keyClick("z", Qt.ControlModifier | Qt.ShiftModifier)
        compare(labels(page), ["B", "C", "T"], "Ctrl+Shift+Z")
        keyClick("z", Qt.ControlModifier)
        keyClick("y", Qt.ControlModifier)
        compare(labels(page), ["B", "C", "T"], "Ctrl+Y")
        page.stopEditing()

        page.select(0)
        page.editsPart.moveTo(1)
        page.closePanel()
        keyClick("z", Qt.ControlModifier)
        compare(labels(page), ["B", "C", "T"], "outside edit mode")

        page.startEditing()
        page.editsPart.removeCell(0)
        page.select(0)
        const field = find(page, "panelLabel")
        field.forceActiveFocus()
        verify(page.typing)
        keyClick("z", Qt.ControlModifier)
        compare(labels(page), ["C", "T"], "the field keeps its own")
        field.focus = false
        page.stopEditing()

        const phone = storedPageOf({ form: "phone", width: 384, height: 832 }, twoPages())
        page.visible = false
        phone.forceActiveFocus()
        phone.editsPart.removeCell(0)
        verify(phone.historyPart.canUndo)
        keyClick("z", Qt.ControlModifier)
        compare(labels(phone), ["B", "C", "T"], "no keys on the phone form")
    }

    // The bar: Undo, Redo, Add, Page and Done; Undo and Redo shown but off
    // with nothing to take, named for their step; "Add" on a narrow page
    function test_theBarsOrder() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        const bar = find(page, "dashEditBar")
        compare(Array.from(bar.children).filter(c => c.visible && c.objectName !== "").map(c => c.objectName),
                ["dashUndo", "dashRedo", "dashAdd", "dashMore", "dashDone"])
        const undo = find(page, "dashUndo")
        const redo = find(page, "dashRedo")
        verify(undo.visible && !undo.enabled && redo.visible && !redo.enabled, "shown and off")
        compare(redo.opacity, 0.45)
        compare(undo.Accessible.name, "Undo")
        page.editsPart.removeCell(0)
        compare(undo.Accessible.name, "Undo: removed A")
        click(page, "dashUndo")
        compare(redo.Accessible.name, "Redo: removed A")
        compare(find(page, "dashAdd").text, "Add gauge")

        const narrow = storedPageOf({ form: "phone", width: 384, height: 832 }, twoPages())
        narrow.startEditing()
        const add = find(narrow, "dashAdd")
        compare(add.text, "Add")
        compare(add.Accessible.name, "Add a gauge")
    }

    // Paste is the "+" cell's second half while a copy exists, and appends
    function test_pasteIsInThePlusCell() {
        const page = storedPageOf({}, twoPages())
        page.startEditing()
        verify(!findChild(page, "dashPaste"), "not on the bar")
        const box = find(page, "dashAddCellBox_0")
        const plus = find(page, "dashAddCell_0")
        const paste = find(page, "dashPasteCell_0")
        verify(waitForPolish(page.Window.window), "laid out")
        verify(!paste.visible, "nothing copied")
        compare(plus.width, box.width, "the + fills the cell")
        page.select(0)
        page.copySelected()
        page.closePanel()
        verify(paste.visible && paste.enabled)
        verify(plus.width < box.width && paste.x >= plus.width, "split in two")
        click(page, "dashPasteCell_0")
        compare(labels(page), ["A", "B", "C", "T", "A"], "appended")
        compare(page.historyPart.undoWords, "pasted A")
    }

    // ---- The desktop edits the phone's layout (B1)
    function bothFormsPage(props) {
        const phoneDoc = DashboardBook.builtIn("phone")
        phoneDoc.pages = [{ name: "P", cells: [cellOf("PhoneCell")] }]
        const page = pageOf(props || {}, docOf([cellOf("DesktopCell")]))
        const store = find(page, "dashStore")
        store.desktop = DashboardBook.serialize(docOf([cellOf("DesktopCell")]))
        store.phone = DashboardBook.serialize(phoneDoc)
        return page
    }
    function phoneSwitchOn(page) {
        page.startEditing()
        page.historyPart.showStoredLayout()
        verify(page.phoneSwitchShown)
        click(page, "dashPhoneSwitch")
        verify(page.phoneEdit)
    }

    function test_thePhoneSwitchShowsOnlyOnAWideEnoughDesktopInEditMode() {
        const page = bothFormsPage()
        verify(!find(page, "dashPhoneRow").visible, "not outside edit mode")
        page.startEditing()
        verify(find(page, "dashPhoneRow").visible)
        page.width = 431
        verify(!page.phoneSwitchShown, "too narrow for the frame")
        page.width = 432
        verify(page.phoneSwitchShown)
        page.stopEditing()
        verify(!page.phoneSwitchShown)
        const phone = bothFormsPage({ form: "phone" })
        phone.startEditing()
        verify(phone.phoneSwitchShown, "a test's phone form on a desktop still has it")
    }

    function test_theSwitchShowsThePhoneDocumentAndOffReturnsTheDesktopsUntouched() {
        const page = bothFormsPage()
        page.startEditing()
        page.historyPart.showStoredLayout()
        const desktopText = find(page, "dashStore").desktop
        compare(labels(page), ["DesktopCell"])
        phoneSwitchOn(page)
        compare(page.form, "phone")
        compare(labels(page), ["PhoneCell"])
        click(page, "dashPhoneSwitch")
        verify(!page.phoneEdit)
        compare(page.form, "desktop")
        compare(labels(page), ["DesktopCell"])
        compare(find(page, "dashStore").desktop, desktopText)
    }

    function test_anEditInTheFrameWritesThePhonesStoreAlone() {
        const page = bothFormsPage()
        phoneSwitchOn(page)
        const store = find(page, "dashStore")
        const desktopText = store.desktop
        const phoneText = store.phone
        page.editsPart.renamePage("Renamed")
        verify(store.phone !== phoneText, "the phone's text changed")
        compare(store.desktop, desktopText)
    }

    function test_doneLeavesTheFrameAndEditStartsOnTheDesktop() {
        const page = bothFormsPage()
        phoneSwitchOn(page)
        page.stopEditing()
        verify(!page.phoneEdit)
        compare(page.form, "desktop")
        compare(labels(page), ["DesktopCell"])
        page.startEditing()
        verify(!page.phoneEdit)
        compare(page.form, "desktop")
    }

    function test_switchingDropsTheHistoryTheSelectionTheListAndTheQuestions() {
        const page = bothFormsPage()
        page.startEditing()
        page.historyPart.showStoredLayout()
        page.editsPart.renamePage("Changed")
        verify(page.historyPart.canUndo)
        page.selected = 0
        page.panel = "cell"
        page.confirmReset = true
        click(page, "dashPhoneSwitch")
        verify(!page.historyPart.canUndo, "the history is the form's own")
        compare(page.selected, -1)
        compare(page.panel, "")
        verify(!page.confirmReset)
        page.editsPart.openPresets("add")
        click(page, "dashPhoneSwitch")
        compare(page.presetList, "")
    }

    function test_thePhonesPageIndexIsKeptAcrossTheSwitch() {
        const page = bothFormsPage()
        const store = find(page, "dashStore")
        const phoneDoc = DashboardBook.builtIn("phone")
        phoneDoc.pages = [{ name: "A", cells: [cellOf("a")] }, { name: "B", cells: [cellOf("b")] }]
        store.phone = DashboardBook.serialize(phoneDoc)
        store.phonePage = 1
        page.startEditing()
        click(page, "dashPhoneSwitch")
        compare(page.pageIndex, 1)
        click(page, "dashPhoneSwitch")
        compare(page.pageIndex, 0)
        compare(store.phonePage, 1, "the desktop's page didn't overwrite the phone's")
    }

    // A page index the new form's document can't hold is clamped while the
    // form already is the new one: the clamp must not become its remembered page
    function test_aPageIndexTheOtherFormCantHoldIsNotWrittenAsItsOwn() {
        const page = bothFormsPage()
        const store = find(page, "dashStore")
        const desktopDoc = docOf([cellOf("a")])
        desktopDoc.pages = [{ name: "A", cells: [cellOf("a")] }, { name: "B", cells: [cellOf("b")] },
                            { name: "C", cells: [cellOf("c")] }]
        store.desktop = DashboardBook.serialize(desktopDoc)
        const phoneDoc = DashboardBook.builtIn("phone")
        phoneDoc.pages = [{ name: "A", cells: [cellOf("a")] }, { name: "B", cells: [cellOf("b")] }]
        store.phone = DashboardBook.serialize(phoneDoc)
        store.phonePage = 0
        page.startEditing()
        page.historyPart.showStoredLayout()
        page.goTo(2)
        compare(page.pageIndex, 2)
        click(page, "dashPhoneSwitch")
        compare(page.pageIndex, 0)
        compare(store.phonePage, 0)
        click(page, "dashPhoneSwitch")
        compare(page.pageIndex, 2)
        compare(store.desktopPage, 2)
    }

    function test_theExportInTheFrameIsAPhoneFileAndALoadRefusesADesktopOne() {
        const page = bothFormsPage()
        phoneSwitchOn(page)
        click(page, "dashMore")
        const url = layoutUrl("layout-frame-phone.json")
        verify(page.layoutFilePart.exportLayoutTo(url), page.layoutFilePart.layoutNote)
        verify(page.layoutFilePart.layoutNote.indexOf("It's a phone layout") >= 0, page.layoutFilePart.layoutNote)
        const text = Bridge.layoutFileText(url).text
        compare(JSON.parse(text).form, "phone")
        page.editsPart.renamePage("Other")
        verify(page.layoutFilePart.loadLayoutFrom(url), page.layoutFilePart.layoutNote)
        const desktopUrl = layoutUrl("layout-frame-desktop.json")
        const other = bothFormsPage()
        other.startEditing()
        verify(other.layoutFilePart.exportLayoutTo(desktopUrl))
        other.destroy()
        verify(!page.layoutFilePart.loadLayoutFrom(desktopUrl))
        verify(page.layoutFilePart.layoutNote.indexOf("is a desktop layout") >= 0, page.layoutFilePart.layoutNote)
    }

    function test_aConnectedDesktopInTheFrameNeverEntersTheDriveView() {
        const page = bothFormsPage()
        page.startEditing()
        click(page, "dashPhoneSwitch")
        compare(Bridge.connectSession("mock://demo"), "")
        tryVerify(() => Bridge.connected, 5000)
        wait(100)
        page.drivePart.enterDrive()
        verify(!page.driving, "editing is an editor")
        // The gate's own term: the frame still on with editing already off
        page.editing = false
        verify(page.phoneEdit)
        page.drivePart.enterDrive()
        verify(!page.driving, "the frame is an editor")
        page.editing = true
        page.stopEditing()
    }

    function test_theSwitchSaysWhatItDoes() {
        const page = bothFormsPage()
        page.startEditing()
        const sw = find(page, "dashPhoneSwitch")
        compare(sw.text, "Phone layout")
        compare(sw.Accessible.name, "Edit the phone's layout in a phone-sized frame")
        verify(sw.Accessible.description.indexOf("undo history") >= 0)
        for (const w of [720, 1100]) {
            page.width = w
            verify(waitForPolish(page))
            const row = find(page, "dashPhoneRow")
            verify(sw.mapToItem(page, sw.width, 0).x <= page.width, "the switch fits at " + w)
            verify(row.width <= page.width)
        }
    }
}
