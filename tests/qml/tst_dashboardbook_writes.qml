// File/layer: tests/qml/tst_dashboardbook_writes.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboardbook_writes (test_qml)

import QtQuick
import QtTest
import App
import AppTest

DashboardBookKit {
    name: "DashboardBook"

    function test_placementOfTheBuiltInLayouts() {
        const phone = DashboardBook.builtIn("phone").pages[0].cells
        const desktop = DashboardBook.builtIn("desktop").pages[0].cells
        // Upright: the dial with its three readouts stacked beside it, two
        // traces across, three readouts across
        compare(spots(DashboardBook.place(phone, 6)), [
            [0, 0, 6, 3], [0, 3, 4, 18], [4, 3, 2, 6], [4, 9, 2, 6], [4, 15, 2, 6],
            [0, 21, 3, 9], [3, 21, 3, 9], [0, 30, 2, 6], [2, 30, 2, 6], [4, 30, 2, 6]])
        compare(spots(DashboardBook.place(phone, 12)), [
            [0, 0, 12, 3], [0, 3, 4, 18], [4, 3, 2, 6], [6, 3, 2, 6], [8, 3, 2, 6],
            [4, 9, 3, 9], [7, 9, 3, 9], [10, 3, 2, 6], [10, 9, 2, 6], [10, 15, 2, 6]])
        compare(spots(DashboardBook.place(desktop, 12)), [
            [0, 0, 12, 1], [0, 1, 2, 8], [2, 1, 2, 8], [4, 1, 1, 4], [5, 1, 1, 4],
            [6, 1, 1, 4], [7, 1, 1, 4], [8, 1, 1, 4], [9, 1, 1, 4], [10, 1, 1, 4]])
        // A desktop window narrowed keeps the desktop pages at fewer columns
        compare(spots(DashboardBook.place(desktop, 4)), [
            [0, 0, 4, 1], [0, 1, 2, 8], [2, 1, 2, 8], [0, 9, 1, 4], [1, 9, 1, 4],
            [2, 9, 1, 4], [3, 9, 1, 4], [0, 13, 1, 4], [1, 13, 1, 4], [2, 13, 1, 4]])
        // The rings and the readouts beside the dials from 5 columns up; at 5,
        // Battery, the last cell, lands under Oil temp
        compare(spots(DashboardBook.place(desktop, 6)).slice(3, 7), [
            [4, 1, 1, 4], [5, 1, 1, 4], [4, 5, 1, 4], [5, 5, 1, 4]])
        compare(spots(DashboardBook.place(desktop, 5)).slice(3), [
            [4, 1, 1, 4], [4, 5, 1, 4], [0, 9, 1, 4], [1, 9, 1, 4], [2, 9, 1, 4],
            [3, 9, 1, 4], [4, 9, 1, 4]])
    }

    // Each cell goes to the first free block, so a later small cell fills
    // a gap an earlier wide one left
    function test_placementTakesTheFirstFreeBlock() {
        const c = (w, h) => ({ kind: "ring", w: w, h: h, refs: [] })
        compare(spots(DashboardBook.place([c(2, 2), c(1, 1), c(3, 1), c(1, 1)], 3)),
                [[0, 0, 2, 2], [2, 0, 1, 1], [0, 2, 3, 1], [2, 1, 1, 1]])
        // A span wider than the grid takes the whole row
        compare(spots(DashboardBook.place([c(1, 1), c(12, 2)], 3)), [[0, 0, 1, 1], [0, 1, 3, 2]])
        compare(spots(DashboardBook.place([c(5, 1)], 0)), [[0, 0, 1, 1]])
        compare(DashboardBook.place([], 4), [])
        // A width not yet laid out gives no count: one column, never a hang
        for (const n of [NaN, undefined, Infinity, -Infinity])
            compare(spots(DashboardBook.place([c(2, 1), c(1, 1)], n)),
                    [[0, 0, 1, 1], [0, 1, 1, 1]], String(n))
    }

    // A reflow keeps the order: cells of one span read in their order at
    // every width, and no two cells overlap at any width
    function test_aReflowKeepsTheOrder() {
        const same = []
        for (let i = 0; i < 7; ++i)
            same.push({ kind: "ring", w: 1, h: 4, refs: [] })
        for (let cols = 1; cols <= 12; ++cols) {
            const s = DashboardBook.place(same, cols)
            for (let i = 1; i < s.length; ++i) {
                const before = s[i - 1], now = s[i]
                verify(now.y > before.y || (now.y === before.y && now.x > before.x),
                       cols + " columns, cell " + i)
            }
        }
        for (const form of ["phone", "desktop"]) {
            const cells = DashboardBook.builtIn(form).pages[0].cells
            for (let cols = 1; cols <= 12; ++cols) {
                const s = DashboardBook.place(cells, cols)
                compare(s.length, cells.length)
                for (let i = 0; i < s.length; ++i) {
                    verify(s[i].x + s[i].w <= cols, form + " " + cols + " columns, cell " + i)
                    for (let j = 0; j < i; ++j) {
                        const a = s[i], b = s[j]
                        const apart = a.x + a.w <= b.x || b.x + b.w <= a.x
                                   || a.y + a.h <= b.y || b.y + b.h <= a.y
                        verify(apart, form + " " + cols + " columns, cells " + j + " and " + i)
                    }
                }
            }
        }
    }

    // ---- A new gauge's range

    function test_defaultRangeByUnitThenFieldThenFallback() {
        const table = [["RPM", 0, 8000], ["km/h", 0, 260], [degC, 0, 150], ["%", 0, 100],
                       ["kPa", 0, 300], ["V", 0, 16], ["g/s", 0, 300], ["L/h", 0, 60]]
        for (const [unit, from, to] of table)
            compare(DashboardEdit.defaultRange(unit, -1e6, 1e6), { from: from, to: to }, unit)
        compare(DashboardEdit.defaultRange("N\u00B7m", -10, 50), { from: -10, to: 50 })
        compare(DashboardEdit.defaultRange("", -40, 215), { from: -40, to: 215 })
        // A name an object already has is no unit in the table
        compare(DashboardEdit.defaultRange("toString", 1, 2), { from: 1, to: 2 })
        const fallback = [[NaN, NaN], [NaN, 10], [0, Infinity], [5, 5], [6, 5],
                          [undefined, undefined], ["0", "10"]]
        for (const [lo, hi] of fallback)
            compare(DashboardEdit.defaultRange("lb", lo, hi), { from: 0, to: 100 }, lo + " " + hi)
    }

    // ---- Writes

    function test_aCellMovesWithinItsPage() {
        const d = pageOf(4)
        compare(labels(DashboardEdit.moveCell(d, 0, 0, 3), 0), ["c1", "c2", "c3", "c0"])
        compare(labels(DashboardEdit.moveCell(d, 0, 3, 0), 0), ["c3", "c0", "c1", "c2"])
        compare(labels(DashboardEdit.moveCell(d, 0, 1, 2), 0), ["c0", "c2", "c1", "c3"])
        compare(labels(DashboardEdit.moveCell(d, 0, 2, 1), 0), ["c0", "c2", "c1", "c3"])
        compare(labels(DashboardEdit.moveCell(d, 0, 2, 2), 0), ["c0", "c1", "c2", "c3"])
    }

    function test_aResizeIsClampedToTheBounds() {
        const size = (w, h) => {
            const c = DashboardEdit.resizeCell(book(), 0, 0, w, h).pages[0].cells[0]
            return [c.w, c.h]
        }
        compare(size(0, 0), [1, 1])
        compare(size(-5, -5), [1, 1])
        compare(size(99, 99), [12, 48])
        compare(size(12, 48), [12, 48])
        compare(size(2.4, 3.6), [2, 4])
    }

    function test_cellsAreAddedAndRemoved() {
        const added = DashboardEdit.addCell(book(), 1, cell("Boost"))
        compare(labels(added, 1), ["Boost"])
        const more = DashboardEdit.addCell(added, 1, cell("MAP"))
        compare(labels(more, 1), ["Boost", "MAP"])
        compare(labels(DashboardEdit.removeCell(more, 1, 0), 1), ["MAP"])
        compare(labels(DashboardEdit.removeCell(book(), 0, 1), 0), ["IAT"])
    }

    // Several cells in one write, at the page's end; a list that would
    // pass the page's room, or holds a cell that isn't plain, is refused
    // whole, an empty one changes nothing, and the input is never changed
    function test_cellsAreAddedSeveralAtOnceOrNotAtAll() {
        const d = book()
        const before = DashboardBook.serialize(d)
        const added = DashboardEdit.addCells(d, 0, [cell("Boost"), cell("MAP")])
        compare(labels(added, 0), ["IAT", "Oil", "Boost", "MAP"])
        compare(DashboardBook.serialize(DashboardBook.parse(DashboardBook.serialize(added), "desktop").doc),
                DashboardBook.serialize(added), "reads back as written")
        compare(DashboardBook.serialize(d), before, "the input is unchanged")
        for (const refused of [[], null, [cell("A"), { kind: "digital", w: 1, h: 1, refs: [],
                                                       options: { from: Date } }]])
            compare(DashboardBook.serialize(DashboardEdit.addCells(d, 0, refused)), before)
        compare(DashboardBook.serialize(DashboardEdit.addCells(d, 5, [cell("A")])), before, "no such page")

        const full = pageOf(DashboardBook.maxCells - 1)
        const fullBefore = DashboardBook.serialize(full)
        compare(labels(DashboardEdit.addCells(full, 0, [cell("last")]), 0).length, DashboardBook.maxCells,
                "up to the page's room")
        compare(DashboardBook.serialize(DashboardEdit.addCells(full, 0, [cell("A"), cell("B")])),
                fullBefore, "one past it refuses both")
        compare(DashboardBook.serialize(full), fullBefore)
    }

    function test_setOptionsMergesAndNullRemoves() {
        const d = DashboardEdit.setOptions(book(), 0, 0, { warnFrom: 55, from: -40, flash: true })
        compare(d.pages[0].cells[0].options, { from: -40, to: 120, warnFrom: 55, flash: true })
        const e = DashboardEdit.setOptions(d, 0, 0, { warnFrom: null, flash: null })
        compare(e.pages[0].cells[0].options, { from: -40, to: 120 })
        // A cell stored without options gets them
        const bare = book()
        delete bare.pages[0].cells[0].options
        compare(DashboardEdit.setOptions(bare, 0, 0, { peak: true }).pages[0].cells[0].options,
                { peak: true })
    }

    function test_setCellChangesLabelRefsAndUnit() {
        const d = DashboardEdit.setCell(book(), 0, 0, { label: "Intake", refs: ["key:iat2"],
                                                        unit: "\u00B0F" })
        const c = d.pages[0].cells[0]
        compare([c.label, c.refs, c.unit, c.kind, c.w, c.h], ["Intake", ["key:iat2"], "\u00B0F",
                                                               "digital", 1, 4])
        const e = DashboardEdit.setCell(d, 0, 0, { label: null, unit: null })
        compare(e.pages[0].cells[0].label, undefined)
        compare(e.pages[0].cells[0].unit, undefined)
        compare(DashboardBook.labelOf(e.pages[0].cells[0]), "")
    }

    // A new kind keeps the signal, the label, the span and every option,
    // the old kind's own included, so the way back finds them
    function test_setKindKeepsEverythingElse() {
        const own = cell("IAT", "analog")
        own.options = { from: 0, to: 8000, warnFrom: 6000, majorStep: 1000, caption: "x" }
        const d = DashboardEdit.addCell(book(), 0, own)
        const at = d.pages[0].cells.length - 1
        const ring = DashboardEdit.setKind(d, 0, at, "ring")
        const c = ring.pages[0].cells[at]
        compare(c.kind, "ring")
        compare([c.label, c.refs, c.unit, c.w, c.h], [own.label, own.refs, own.unit, own.w, own.h])
        compare(c.options, own.options, "every option, the dial's own included")
        compare(JSON.stringify(DashboardEdit.setKind(ring, 0, at, d.pages[0].cells[at].kind).pages[0].cells[at]),
                JSON.stringify(d.pages[0].cells[at]), "and back is the cell it was")
    }

    function test_setKindRefusesWhatItCannotDraw() {
        const d = DashboardEdit.addCell(book(), 0, { kind: "blank", w: 1, h: 4, refs: [] })
        const s = JSON.stringify
        for (const [what, doc, i, kind] of [
                ["a blank target", book(), 0, "blank"],
                ["an unknown kind", book(), 0, "gauge9"],
                ["the same kind", book(), 0, "digital"],
                ["a blank cell", d, 2, "ring"],
                ["no cell", book(), 5, "ring"],
                ["no page", book(), -1, "ring"]])
            compare(s(DashboardEdit.setKind(doc, what === "no page" ? 9 : 0, i, kind)), s(doc), what)
    }

    function test_thePageFunctions() {
        const added = DashboardEdit.addPage(book(), "  Laps  ")
        compare(added.pages.map(p => p.name), ["Engine", "Track", "Laps"])
        compare(added.pages[2].cells, [])
        compare(DashboardEdit.renamePage(added, 1, " Street ").pages[1].name, "Street")
        compare(DashboardEdit.movePage(added, 2, 0).pages.map(p => p.name),
                ["Laps", "Engine", "Track"])
        compare(DashboardEdit.movePage(added, 0, 1).pages.map(p => p.name),
                ["Track", "Engine", "Laps"])
        compare(DashboardEdit.removePage(added, 0).pages.map(p => p.name), ["Track", "Laps"])
        compare(DashboardEdit.removePage(added, 0).pages[0].cells, [])
    }

    function test_theControlsState() {
        const nine = book()
        while (nine.pages.length < 9)
            nine.pages.push({ name: "P", cells: [] })
        verify(DashboardEdit.canAddPage(nine))
        verify(!DashboardEdit.canAddPage(DashboardEdit.addPage(nine, "Last")))
        verify(DashboardEdit.canRemovePage(book()))
        verify(!DashboardEdit.canRemovePage(DashboardEdit.removePage(book(), 1)))
        verify(DashboardEdit.canAddCell(pageOf(23), 0))
        verify(!DashboardEdit.canAddCell(pageOf(24), 0))
        verify(!DashboardEdit.canAddCell(book(), 2))
        verify(!DashboardEdit.canAddCell(book(), -1))
        verify(!DashboardEdit.canAddPage({ format: 1, pages: "x" }))
        compare(DashboardBook.cellProblem(cell("IAT")), "")
        verify(DashboardBook.cellProblem(cell("IAT", { w: 0 })) !== "")
        compare(DashboardBook.pageNameProblem("Track"), "")
        verify(DashboardBook.pageNameProblem(" ") !== "")
    }

    // A write that would leave a document that doesn't read back as it is
    // returns a copy equal to its input, and never changes the input
    function test_aRefusedWriteReturnsAnEqualCopy() {
        const near = docOfBytes(DashboardBook.maxBytes - 136)
        const tenPages = book()
        while (tenPages.pages.length < 10)
            tenPages.pages.push({ name: "P", cells: [] })
        const refused = [
            ["a 25th cell", pageOf(24), d => DashboardEdit.addCell(d, 0, cell("x"))],
            ["a bad cell", book(), d => DashboardEdit.addCell(d, 0, cell("x", { w: 0 }))],
            ["a cell not an object", book(), d => DashboardEdit.addCell(d, 0, "x")],
            ["a cell on no page", book(), d => DashboardEdit.addCell(d, 2, cell("x"))],
            ["a cell past the size bound", near, d => DashboardEdit.addCell(d, 0,
                                                 cell("x", { more: "y".repeat(200) }))],
            ["removing no cell", book(), d => DashboardEdit.removeCell(d, 0, 2)],
            ["removing a cell of no page", book(), d => DashboardEdit.removeCell(d, 5, 0)],
            ["moving from no cell", book(), d => DashboardEdit.moveCell(d, 0, 2, 0)],
            ["moving to no cell", book(), d => DashboardEdit.moveCell(d, 0, 0, -1)],
            ["moving past the end", book(), d => DashboardEdit.moveCell(d, 0, 0, 2)],
            ["moving a fraction", book(), d => DashboardEdit.moveCell(d, 0, 0.5, 1)],
            ["a resize to NaN", book(), d => DashboardEdit.resizeCell(d, 0, 0, NaN, 2)],
            ["a resize by text", book(), d => DashboardEdit.resizeCell(d, 0, 0, "2", 2)],
            ["a resize of no cell", book(), d => DashboardEdit.resizeCell(d, 1, 0, 2, 2)],
            ["setCell's kind", book(), d => DashboardEdit.setCell(d, 0, 0, { kind: "ring" })],
            ["setCell's span", book(), d => DashboardEdit.setCell(d, 0, 0, { w: 2 })],
            ["setCell's options", book(), d => DashboardEdit.setCell(d, 0, 0, { options: {} })],
            ["9 refs", book(), d => DashboardEdit.setCell(d, 0, 0,
                                     { refs: ["a", "b", "c", "d", "e", "f", "g", "h", "i"] })],
            ["refs removed", book(), d => DashboardEdit.setCell(d, 0, 0, { refs: null })],
            ["a label of 41", book(), d => DashboardEdit.setCell(d, 0, 0,
                                                                 { label: "x".repeat(41) })],
            ["setCell with no fields", book(), d => DashboardEdit.setCell(d, 0, 0, null)],
            ["from at to", book(), d => DashboardEdit.setOptions(d, 0, 0, { from: 120 })],
            ["decimals 7", book(), d => DashboardEdit.setOptions(d, 0, 0, { decimals: 7 })],
            ["accent not text", book(), d => DashboardEdit.setOptions(d, 0, 0, { accent: 1 })],
            ["options not an object", book(), d => DashboardEdit.setOptions(d, 0, 0, [1])],
            ["options of no cell", book(), d => DashboardEdit.setOptions(d, 1, 0, { peak: true })],
            ["an 11th page", tenPages, d => DashboardEdit.addPage(d, "More")],
            ["a page with no name", book(), d => DashboardEdit.addPage(d, "  ")],
            ["a page named by a number", book(), d => DashboardEdit.addPage(d, 7)],
            ["a page's name of 41", book(), d => DashboardEdit.renamePage(d, 0, "x".repeat(41))],
            ["renaming no page", book(), d => DashboardEdit.renamePage(d, 2, "x")],
            ["moving no page", book(), d => DashboardEdit.movePage(d, 0, 2)],
            ["removing the last page", DashboardEdit.removePage(book(), 1),
             d => DashboardEdit.removePage(d, 0)],
            ["removing no page", book(), d => DashboardEdit.removePage(d, 2)],
            ["writing to a document that doesn't read", { format: 1, pages: [] },
             d => DashboardEdit.addPage(d, "P")],
            // Values stored text doesn't hold as they are
            ["a cell holding a function", book(),
             d => DashboardEdit.addCell(d, 0, cell("x", { onTap: () => 1 }))],
            ["a cell's option a date", book(),
             d => DashboardEdit.addCell(d, 0, cell("x", { options: { caption: new Date(0) } }))],
            ["a label a date", book(), d => DashboardEdit.setCell(d, 0, 0, { label: new Date(0) })],
            ["refs holding a date", book(),
             d => DashboardEdit.setCell(d, 0, 0, { refs: ["key:a", new Date(0)] })],
            ["refs holding undefined", book(),
             d => DashboardEdit.setCell(d, 0, 0, { refs: ["key:a", undefined] })],
            ["an option a date", book(),
             d => DashboardEdit.setOptions(d, 0, 0, { caption: new Date(0) })],
            ["an option a function", book(),
             d => DashboardEdit.setOptions(d, 0, 0, { flash: () => 1 })],
            ["an unknown option NaN", book(), d => DashboardEdit.setOptions(d, 0, 0, { glow: NaN })],
            ["an unknown option holding Infinity", book(),
             d => DashboardEdit.setOptions(d, 0, 0, { extra: { a: Infinity } })]
        ]
        for (const [name, input, write] of refused) {
            const before = JSON.stringify(input)
            const out = write(input)
            compare(out, JSON.parse(before), name)
            verify(out !== input, name + ": a copy")
            compare(JSON.stringify(input), before, name + ": the input unchanged")
        }
    }

    // A write's result shares nothing with its arguments: changing what
    // was passed afterwards leaves the result as it was
    function test_aWriteSharesNothingWithItsArguments() {
        const c = cell("Boost")
        const added = DashboardEdit.addCell(book(), 1, c)
        c.refs.push("id:9")
        c.options.from = -5
        compare(added.pages[1].cells[0].refs, ["id:1005", "id:7005"])
        compare(added.pages[1].cells[0].options.from, 0)

        const refs = ["key:a"]
        const set = DashboardEdit.setCell(book(), 0, 0, { refs: refs })
        refs.push("key:b")
        compare(set.pages[0].cells[0].refs, ["key:a"])

        const extra = { a: 1 }
        const opt = DashboardEdit.setOptions(book(), 0, 0, { extra: extra })
        extra.a = 2
        compare(opt.pages[0].cells[0].options.extra, { a: 1 })
    }

    // A key holding undefined is left out: of a change, of a cell added,
    // and of the result, which reads back with the same keys
    function test_aKeyHoldingUndefinedIsLeftOut() {
        const d = DashboardEdit.setOptions(book(), 0, 0, { from: undefined, warnFrom: 50 })
        compare(d.pages[0].cells[0].options, { from: 0, to: 120, warnFrom: 50 })
        const e = DashboardEdit.setCell(book(), 0, 0, { label: undefined, unit: "x" })
        compare([e.pages[0].cells[0].label, e.pages[0].cells[0].unit], ["IAT", "x"])
        const f = DashboardEdit.addCell(book(), 1, cell("x", {
            unit: undefined, options: { from: 0, to: 1, caption: undefined } }))
        const added = f.pages[1].cells[0]
        verify(!("unit" in added))
        verify(!("caption" in added.options))
        for (const out of [d, e, f])
            compare(DashboardBook.parse(DashboardBook.serialize(out), "desktop").doc, out)
    }

    // Every kind of write gives a document that reads back equal, and
    // never changes its input
    function test_eachWriteReadsBack() {
        const writes = [
            ["addCell", d => DashboardEdit.addCell(d, 1, cell("Boost"))],
            ["removeCell", d => DashboardEdit.removeCell(d, 0, 0)],
            ["moveCell", d => DashboardEdit.moveCell(d, 0, 0, 1)],
            ["resizeCell", d => DashboardEdit.resizeCell(d, 0, 1, 3, 9)],
            ["setCell", d => DashboardEdit.setCell(d, 0, 1, { label: emoji + " Oil",
                                                              refs: ["key:oil"] })],
            ["setOptions", d => DashboardEdit.setOptions(d, 0, 1, { peak: true, to: null,
                                                                    accent: "amber" })],
            ["addPage", d => DashboardEdit.addPage(d, "Laps")],
            ["renamePage", d => DashboardEdit.renamePage(d, 1, "Laps")],
            ["movePage", d => DashboardEdit.movePage(d, 1, 0)],
            ["removePage", d => DashboardEdit.removePage(d, 0)]
        ]
        for (const form of ["", "phone", "desktop"]) {
            for (const [name, write] of writes) {
                const input = form === "" ? book() : DashboardEdit.addPage(
                                                         DashboardBook.builtIn(form), "Track")
                const before = JSON.stringify(input)
                const out = write(input)
                verify(JSON.stringify(out) !== before, name + " " + form + ": a change")
                compare(DashboardBook.documentProblem(out), "", name + " " + form)
                const r = DashboardBook.parse(DashboardBook.serialize(out), "desktop")
                compare(r.problem, "", name + " " + form)
                compare(r.doc, out, name + " " + form)
                compare(JSON.stringify(input), before, name + " " + form + ": the input unchanged")
            }
        }
    }

    // ---- Copy and paste (ADR-0057's appendix, section 5)

    function test_copyCellGivesACellOfItsOwn() {
        const d = book()
        const c = DashboardEdit.copyCell(d, 0, 1)
        compare(c, d.pages[0].cells[1])
        c.options.from = 99
        compare(d.pages[0].cells[1].options.from, 0, "the source is its own")
        compare(DashboardEdit.copyCell(d, 0, 2), null, "past the end")
        compare(DashboardEdit.copyCell(d, 5, 0), null, "no such page")
        compare(DashboardEdit.copyCell(d, 0, 0.5), null, "not an index")
        compare(DashboardEdit.copyCell({ format: 99 }, 0, 0), null, "not a document")
    }

    function test_pasteCellPutsACopyAfterTheIndex() {
        const d = pageOf(3)
        const paste = DashboardEdit.copyCell(d, 0, 0)
        paste.label = "new"
        compare(labels(DashboardEdit.pasteCell(d, 0, -1, paste), 0), ["new", "c0", "c1", "c2"])
        compare(labels(DashboardEdit.pasteCell(d, 0, 0, paste), 0), ["c0", "new", "c1", "c2"])
        compare(labels(DashboardEdit.pasteCell(d, 0, 2, paste), 0), ["c0", "c1", "c2", "new"])
        compare(labels(d, 0), ["c0", "c1", "c2"], "its input unchanged")

        const out = DashboardEdit.pasteCell(d, 0, 0, paste)
        out.pages[0].cells[1].options.from = 42
        compare(paste.options.from, 0, "the pasted cell is its own")
    }

    function test_pasteCellRefusesWhatItCantTake() {
        const d = pageOf(2)
        const c = DashboardEdit.copyCell(d, 0, 0)
        const same = out => compare(DashboardBook.serialize(out), DashboardBook.serialize(d))
        same(DashboardEdit.pasteCell(d, 0, -2, c))
        same(DashboardEdit.pasteCell(d, 0, 2, c))
        same(DashboardEdit.pasteCell(d, 3, 0, c))
        same(DashboardEdit.pasteCell(d, 0, 0.5, c))
        same(DashboardEdit.pasteCell(d, 0, 0, null))
        same(DashboardEdit.pasteCell(d, 0, 0, { kind: "digital", w: 99, h: 1, refs: [] }))
        same(DashboardEdit.pasteCell(d, 0, 0, { kind: "digital", w: 1, h: 1, refs: [],
                                                options: { from: Date } }))
    }

    function test_aFullPageRefusesAPaste() {
        const d = pageOf(DashboardBook.maxCells)
        const c = DashboardEdit.copyCell(d, 0, 0)
        const out = DashboardEdit.pasteCell(d, 0, 0, c)
        compare(DashboardBook.serialize(out), DashboardBook.serialize(d))
        verify(!DashboardEdit.canAddCell(d, 0))
    }

    // ---- A drag's order

    function test_reorderedMovesOneAndLeavesItsInput() {
        const list = ["a", "b", "c", "d"]
        compare(DashboardEdit.reordered(list, 0, 2), ["b", "c", "a", "d"])
        compare(DashboardEdit.reordered(list, 3, 1), ["a", "d", "b", "c"])
        compare(DashboardEdit.reordered(list, 1, 1), list)
        compare(list, ["a", "b", "c", "d"], "its input unchanged")
    }

    // For every from and to, the moved index agrees with the reordered list
    function test_movedIndexAgreesWithReordered() {
        const list = ["a", "b", "c", "d", "e"]
        for (let from = 0; from < list.length; ++from) {
            for (let to = 0; to < list.length; ++to) {
                const out = DashboardEdit.reordered(list, from, to)
                for (let i = 0; i < list.length; ++i) {
                    compare(out[DashboardEdit.movedIndex(i, from, to)], list[i],
                            from + " to " + to + ", item " + i)
                }
            }
        }
    }

    function test_dropBoxesAreTheBoxesOfEachPlace() {
        const cells = [{ w: 2, h: 2 }, { w: 2, h: 2 }, { w: 2, h: 2 }]
        const boxes = DashboardEdit.dropBoxes(cells, 0, 4)
        compare(boxes.length, 3)
        compare([boxes[0].x, boxes[0].y], [0, 0])
        compare([boxes[1].x, boxes[1].y], [2, 0])
        compare([boxes[2].x, boxes[2].y], [0, 2])
    }

    function test_nearestTakesTheClosestAndTheFirstOnATie() {
        const pts = [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 0, y: 10 }]
        compare(DashboardEdit.nearest(pts, 9, 1), 1)
        compare(DashboardEdit.nearest(pts, 1, 9), 2)
        compare(DashboardEdit.nearest(pts, 5, 0), 0, "a tie is the first")
        compare(DashboardEdit.nearest([], 0, 0), -1)
    }

    // ---- The blank kind

    function test_kindsAreAppendOnly() {
        compare(DashboardBook.kinds.slice(0, 8),
                ["analog", "ring", "bar", "segments", "digital", "seven-segment", "sparkline",
                 "shift-light"])
        compare(DashboardBook.kinds[8], "blank")
        verify(DashboardBook.knownKind("blank"))
    }

    function test_aBlankCellNeedsNoSignal() {
        const blank = { kind: "blank", w: 3, h: 12, refs: [] }
        compare(DashboardBook.cellProblem(blank), "")
        const d = book()
        d.pages[0].cells.splice(1, 0, blank)
        const r = read(d)
        compare(r.problem, "")
        compare(r.doc.pages[0].cells[1], blank, "it reads back as written")
        const copied = DashboardEdit.copyCell(r.doc, 0, 1)
        compare(copied, blank)
        const pasted = DashboardEdit.pasteCell(r.doc, 0, 0, copied)
        compare(pasted.pages[0].cells[1], blank)
    }

    // A blank holds its spot: the next gauge goes past it, never into it
    function test_placementKeepsABlankCellsSpotOpen() {
        const a = { w: 2, h: 2 }, blank = { kind: "blank", w: 2, h: 2, refs: [] }, c = { w: 2, h: 2 }
        const boxes = DashboardBook.place([a, blank, c], 4)
        compare([boxes[1].x, boxes[1].y], [2, 0])
        compare([boxes[2].x, boxes[2].y], [0, 2], "wrapped past the blank")
    }

    // The joined panel's lines (ADR-0057's appendix, section 3). The same
    // four columns throughout; a cell's kind is all that matters to a line
    function test_seams_data() {
        const c = (w, h) => ({ w: w, h: h })
        const blank = (w, h) => ({ kind: "blank", w: w, h: h, refs: [] })
        const v = (x, y, length) => ({ vertical: true, x: x, y: y, length: length })
        const h = (x, y, length) => ({ vertical: false, x: x, y: y, length: length })
        return [
            { tag: "one cell across", cells: [c(4, 2)], lines: [] },
            { tag: "two side by side", cells: [c(2, 2), c(2, 2)], lines: [v(2, 0, 2)] },
            { tag: "two stacked", cells: [c(4, 1), c(4, 1)], lines: [h(0, 1, 4)] },
            { tag: "two beside a tall one", cells: [c(2, 2), c(2, 1), c(2, 1)],
              lines: [v(2, 0, 2), h(2, 1, 2)] },
            { tag: "a line past several pairs is one",
              cells: [c(2, 1), c(2, 1), c(2, 1), c(2, 1)], lines: [v(2, 0, 2), h(0, 1, 4)] },
            { tag: "beside an empty square", cells: [c(2, 2)], lines: [v(2, 0, 2)] },
            { tag: "a gauge beside a blank", cells: [c(2, 2), blank(2, 2)], lines: [v(2, 0, 2)] },
            { tag: "two blanks draw nothing", cells: [blank(2, 2), blank(2, 2)], lines: [] },
            { tag: "a blank beside an empty square", cells: [blank(2, 2)], lines: [] },
            { tag: "any other kind is drawn, the + cell's none included",
              cells: [{ kind: "ring", w: 2, h: 2 }, c(2, 2)], lines: [v(2, 0, 2)] }
        ]
    }

    function test_seams(data) {
        const boxes = DashboardBook.place(data.cells, 4)
        compare(DashboardBook.seams(data.cells, boxes, 4), data.lines)
    }

    // A built-in phone cell's height is one the Height control can reach
    function test_thePhonesBuiltInHeightsAreOnTheScale() {
        for (const c of DashboardBook.builtIn("phone").pages[0].cells)
            compare(c.h % DashboardBook.phoneSpanScale, 0, c.label + " at " + c.h)
    }

    // Each cell's drawn height and width on four columns, placed in order:
    // down over the empty squares under it, then, alone in its rows, right
    // to the edge
    function test_stretched_data() {
        const c = (w, h) => ({ w: w, h: h })
        const blank = (w, h) => ({ kind: "blank", w: w, h: h, refs: [] })
        return [
            { tag: "a shorter cell beside a taller one", cells: [c(2, 2), c(2, 3)],
              heights: [3, 3], widths: [2, 2] },
            { tag: "equal heights stay", cells: [c(2, 2), c(2, 2)], heights: [2, 2], widths: [2, 2] },
            { tag: "one cell stays", cells: [c(4, 2)], heights: [2], widths: [4] },
            { tag: "stops at the next cell", cells: [c(2, 1), c(2, 4), c(2, 1)],
              heights: [1, 4, 3], widths: [2, 2, 2] },
            { tag: "beside a blank that runs longer", cells: [c(2, 1), blank(2, 3)],
              heights: [3, 3], widths: [2, 2] },
            { tag: "a blank doesn't stretch", cells: [blank(2, 1), c(2, 3)],
              heights: [1, 3], widths: [2, 2] },
            { tag: "never over a blank",
              cells: [c(2, 1), c(2, 4), blank(2, 1), c(2, 1)], heights: [1, 4, 1, 2],
              widths: [2, 2, 2, 2] },
            { tag: "only as far as every column is empty; beside another it keeps its width",
              cells: [c(4, 1), c(1, 3), c(3, 1), c(1, 1)], heights: [1, 3, 1, 2], widths: [4, 1, 3, 1] },
            { tag: "a cell with no kind is a gauge", cells: [{ w: 2, h: 1 }, c(2, 2)],
              heights: [2, 2], widths: [2, 2] },
            { tag: "a later column taken stops it", cells: [c(2, 1), c(1, 1)], heights: [1, 1],
              widths: [4, 3],
              boxes: [{ x: 0, y: 0, w: 2, h: 1 }, { x: 1, y: 1, w: 1, h: 1 }] },
            { tag: "the last cell alone in its row takes the row", cells: [c(4, 1), c(2, 1)],
              heights: [1, 1], widths: [4, 4] },
            { tag: "a blank beside it stops the row", cells: [c(4, 1), c(2, 1), blank(1, 1)],
              heights: [1, 1, 1], widths: [4, 2, 1] },
            { tag: "a cell running down beside it stops the row",
              cells: [c(4, 1), c(1, 1), c(3, 1), c(1, 1)], heights: [1, 1, 2, 1], widths: [4, 1, 3, 1] },
            { tag: "alone in its row between two full rows, it takes the row",
              cells: [c(4, 1), c(1, 1), c(4, 1)], heights: [1, 1, 1], widths: [4, 4, 4] }
        ]
    }

    function test_stretched(data) {
        const boxes = data.boxes || DashboardBook.place(data.cells, 4)
        const before = JSON.stringify(boxes)
        const out = DashboardBook.stretched(data.cells, boxes, 4)
        compare(JSON.stringify(boxes), before, "the input is left alone")
        compare(out.map(b => b.h), data.heights)
        compare(out.map(b => b.w), data.widths)
        boxes.forEach((b, i) => {
            compare(out[i].x, b.x, "x stays")
            compare(out[i].y, b.y, "y stays")
        })
    }

    // The grid's columns bound the stretch to the right; without them the
    // boxes' own right edge does
    function test_stretchedStopsAtTheGridsColumns() {
        const cells = [{ w: 2, h: 1 }]
        const boxes = DashboardBook.place(cells, 6)
        compare(DashboardBook.stretched(cells, boxes, 6)[0].w, 6)
        compare(DashboardBook.stretched(cells, boxes)[0].w, 2)
    }

    // The built-in layout's drawing (`keepSquare`): a dial or ring keeps its
    // box, and a gauge fills down and then right over what it leaves. Without
    // it every cell stretches down, and only one alone in its rows runs right
    function test_stretchedKeepingTheSquares() {
        const cell = (kind, w, h) => ({ kind: kind, w: w, h: h })
        const cells = [cell("ring", 1, 4), cell("digital", 1, 4), cell("digital", 1, 4)]
        const boxes = [{ x: 0, y: 0, w: 1, h: 4 }, { x: 1, y: 0, w: 1, h: 4 },
                       { x: 0, y: 4, w: 1, h: 4 }]
        compare(DashboardBook.stretched(cells, boxes, 4).map(b => [b.w, b.h]),
                [[1, 4], [1, 8], [1, 4]], "as before")
        compare(DashboardBook.stretched(cells, boxes, 4, true).map(b => [b.w, b.h]),
                [[1, 4], [3, 8], [1, 4]], "the second runs down, then right to the edge")
        compare(DashboardBook.stretched(cells, boxes, 4, false).map(b => [b.w, b.h]),
                [[1, 4], [1, 8], [1, 4]], "false is the same as left out")
        // The dial keeps its box even with empty squares under it
        const dial = [cell("analog", 1, 4)]
        const room = [{ x: 0, y: 0, w: 1, h: 4 }, { x: 1, y: 0, w: 1, h: 8 }]
        compare(DashboardBook.stretched(dial.concat([cell("digital", 1, 8)]), room, 4, true)
                    .map(b => [b.w, b.h]), [[1, 4], [3, 8]], "a dial is never stretched")
        // A blank beside it stops the fill, as one under it does
        const withBlank = [cell("digital", 1, 4), cell("blank", 1, 4)]
        compare(DashboardBook.stretched(withBlank, [{ x: 0, y: 0, w: 1, h: 4 },
                                                     { x: 1, y: 0, w: 1, h: 4 }], 4, true)
                    .map(b => [b.w, b.h]), [[1, 4], [1, 4]], "never over a blank")
    }

    function test_digitsFor_data() {
        return [
            { tag: "rpm", from: 0, to: 8000, d: 0, want: 4 },
            { tag: "percent", from: 0, to: 100, d: 0, want: 3 },
            { tag: "a decimal", from: 0, to: 100, d: 1, want: 4 },
            { tag: "volts", from: 0, to: 16, d: 1, want: 3 },
            { tag: "a minus takes a digit", from: -40, to: 9, d: 0, want: 3 },
            { tag: "the minus's end is the longer", from: -400, to: 215, d: 1, want: 5 },
            { tag: "under one", from: 0, to: 0.5, d: 2, want: 4 },
            { tag: "an overshoot still draws", from: 0, to: 999, d: 0, want: 4 },
            { tag: "a narrow range keeps three", from: 0, to: 10, d: 0, want: 3 },
            { tag: "at most eight", from: 0, to: 1e12, d: 0, want: 8 },
            { tag: "at least three", from: 0, to: 0, d: 0, want: 3 },
            { tag: "no range", from: NaN, to: 100, d: 0, want: 4 },
            { tag: "an infinite end", from: 0, to: Infinity, d: 0, want: 4 }
        ]
    }

    function test_digitsFor(data) {
        compare(DashboardEdit.digitsFor(data.from, data.to, data.d), data.want)
    }
}
