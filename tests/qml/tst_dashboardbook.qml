// File/layer: tests/qml/tst_dashboardbook.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboardbook (test_qml)

import QtQuick
import QtTest
import App
TestCase {
    name: "DashboardBook"

    readonly property string degC: "\u00B0C"
    // One character outside the BMP: two UTF-16 units, one code point
    readonly property string emoji: "\uD83D\uDE97"

    function cell(label, extra) {
        const c = { kind: "digital", w: 1, h: 4, label: label, refs: ["id:1005", "id:7005"],
                    unit: degC, options: { from: 0, to: 120 } }
        for (const k in extra)
            c[k] = extra[k]
        return c
    }

    // Two pages, the first with two cells
    function book() {
        return {
            format: 1,
            pages: [
                { name: "Engine", cells: [cell("IAT"), cell("Oil")] },
                { name: "Track", cells: [] }
            ]
        }
    }

    // A page of n cells labeled c0, c1, ...
    function pageOf(n) {
        const cells = []
        for (let i = 0; i < n; ++i)
            cells.push(cell("c" + i))
        return { format: 1, pages: [{ name: "P", cells: cells }] }
    }

    function read(d) {
        return DashboardBook.parse(JSON.stringify(d), "desktop")
    }

    function labels(d, page) {
        return d.pages[page].cells.map(c => c.label)
    }

    // The text's size in UTF-8 bytes, counted apart from DashboardBook's
    function bytes(s) {
        const enc = encodeURIComponent(s)
        return enc.length - 2 * (enc.split("%").length - 1)
    }

    // A document whose text is exactly n bytes, padded in an unknown key
    // with characters of one to four bytes
    function docOfBytes(n) {
        const d = { format: 1, pages: [{ name: "P", cells: [] }], pad: "\u20AC" + emoji }
        let pad = n - bytes(JSON.stringify(d))
        d.pad += "\u00E9".repeat(Math.floor(pad / 2)) + (pad % 2 ? "a" : "")
        return d
    }

    function refsOf(key) {
        const out = []
        for (const r of Bridge.gaugeSignals[key])
            out.push(String(r))
        return out
    }

    // ---- The built-in layout

    function test_eachFormsBuiltInLayoutReadsBack() {
        for (const form of ["phone", "desktop"]) {
            const b = DashboardBook.builtIn(form)
            compare(DashboardBook.documentProblem(b), "", form)
            const r = DashboardBook.parse(DashboardBook.serialize(b), form)
            compare(r.problem, "", form)
            compare(r.doc, b, form)
            compare(DashboardBook.reset(form), b, form)
        }
    }

    // The built-in gauges in their order, each bound to the refs the
    // bridge pairs for it, at spans that keep their proportions
    function test_theBuiltInLayoutsGauges() {
        const want = [
            ["Shift lights", "shift-light", "rpm", "RPM", [12, 1], [12, 1]],
            ["Engine speed", "analog", "rpm", "RPM", [1, 4], [2, 8]],
            ["Vehicle speed", "analog", "speed", "km/h", [1, 4], [2, 8]],
            ["Coolant", "ring", "coolant", degC, [1, 4], [1, 4]],
            ["Oil temp", "ring", "oil", degC, [1, 4], [1, 4]],
            ["IAT", "digital", "iat", degC, [1, 4], [1, 4]],
            ["MAF", "digital", "maf", "g/s", [1, 4], [1, 4]],
            ["Throttle", "bar", "throttle", "%", [1, 2], [2, 2]],
            ["Engine load", "bar", "load", "%", [1, 2], [2, 2]]
        ]
        for (const form of ["phone", "desktop"]) {
            const b = DashboardBook.builtIn(form)
            compare(b.pages.length, 1)
            compare(b.pages[0].name, "Engine")
            const cells = b.pages[0].cells
            compare(cells.length, want.length)
            for (let i = 0; i < want.length; ++i) {
                const [label, kind, key, unit, phone, desktop] = want[i]
                const c = cells[i]
                compare(c.label, label)
                compare(c.kind, kind, label)
                verify(DashboardBook.knownKind(c.kind), label)
                compare(c.refs, refsOf(key), label)
                compare(c.unit, unit, label)
                const span = form === "phone" ? phone : desktop
                compare([c.w, c.h], span, label + " " + form)
                const accent = c.options.accent
                verify(accent === undefined || DashboardBook.knownAccent(accent), label)
            }
            compare(cells[0].options, { from: 4500, to: 6500, count: form === "phone" ? 11 : 15 })
            // The two RPM cells' lists are each their own
            verify(cells[0].refs !== cells[1].refs, form)
        }
        const c = DashboardBook.builtIn("desktop").pages[0].cells
        compare(c[1].options, { from: 0, to: 8000, majorStep: 1000, minorStep: 250,
                                labelDivisor: 1000, dangerFrom: 6800,
                                caption: "\u00D71000 r/min", peak: true, progress: true })
        compare(c[2].options, { from: 0, to: 260, majorStep: 20, minorStep: 10, peak: true,
                                peakLabel: "TOP" })
        compare(c[3].options, { from: 0, to: 130, coldBelow: 60, warnFrom: 105, dangerFrom: 115 })
        compare(c[4].options, { from: 0, to: 160, coldBelow: 70, warnFrom: 125, dangerFrom: 140 })
        compare(c[5].options, { from: 0, to: 120, warnFrom: 55, dangerFrom: 70,
                                description: "Intake air temperature" })
        compare(c[6].options, { from: 0, to: 300, description: "Mass air flow", accent: "info" })
        compare(c[7].options, { from: 0, to: 100, accent: "success" })
        compare(c[8].options, { from: 0, to: 100 })
    }

    // ---- Reading

    function test_aStoredDocumentReadsBack() {
        const r = read(book())
        compare(r.problem, "")
        compare(r.doc, book())
    }

    // A later version's kinds, ref forms, accents and keys read in this one
    // as they were written, and go back out with the next save
    function test_whatThisVersionDoesntKnowIsKept() {
        const d = book()
        d.theme = { night: true }
        d.pages[0].icon = "gauge"
        d.pages[0].cells.push({ kind: "needle", w: 3, h: 2, refs: ["did:7E9:1E1C", "id:1001"],
                                glow: 2, options: { sweep: "slow", accent: "amber" } })
        d.pages[0].cells[0].options.flash = [1, 2]
        const r = read(d)
        compare(r.problem, "")
        compare(r.doc, d)
        compare(DashboardBook.serialize(r.doc), JSON.stringify(d))
        verify(!DashboardBook.knownKind("needle"))
        verify(DashboardBook.knownKind("seven-segment"))
        verify(!DashboardBook.knownAccent("amber"))
        verify(DashboardBook.knownAccent("info"))
        // A write elsewhere keeps them
        const w = DashboardBook.renamePage(r.doc, 1, "Laps")
        compare(w.theme, { night: true })
        compare(w.pages[0].cells[2], d.pages[0].cells[2])
    }

    // A cell or page may leave out label, unit and options; they read with
    // their defaults and the stored object keeps them out
    function test_aCellsOptionalFieldsReadWithDefaults() {
        const d = book()
        d.pages[0].cells = [{ kind: "ring", w: 1, h: 4, refs: [] }]
        const r = read(d)
        compare(r.problem, "")
        const c = r.doc.pages[0].cells[0]
        compare(c, { kind: "ring", w: 1, h: 4, refs: [] })
        compare(DashboardBook.labelOf(c), "")
        compare(DashboardBook.unitOf(c), "")
        compare(DashboardBook.optionsOf(c), {})
        compare(DashboardBook.labelOf(cell("IAT")), "IAT")
        compare(DashboardBook.unitOf(cell("IAT")), degC)
        compare(DashboardBook.optionsOf(cell("IAT")), { from: 0, to: 120 })
    }

    function test_noStoredTextIsTheBuiltInLayout() {
        for (const t of ["", undefined, null]) {
            const r = DashboardBook.parse(t, "phone")
            compare(r.problem, "none")
            compare(r.doc, DashboardBook.builtIn("phone"))
        }
        compare(DashboardBook.parse("", "desktop").doc, DashboardBook.builtIn("desktop"))
    }

    function test_unreadableTextIsDamaged() {
        const pages = book().pages
        const texts = [
            "{", "[]", "null", "7", "\"x\"", "{}",
            JSON.stringify({ pages: pages }),
            JSON.stringify({ format: "1", pages: pages }),
            JSON.stringify({ format: 1.5, pages: pages }),
            JSON.stringify({ format: 0, pages: pages }),
            JSON.stringify({ format: -1, pages: pages }),
            JSON.stringify({ format: 1 }),
            JSON.stringify({ format: 1, pages: {} }),
            // A form holds 1 to 10 pages: none, or none left once dropped
            JSON.stringify({ format: 1, pages: [] }),
            JSON.stringify({ format: 1, pages: [{ name: "" , cells: [] }, "x"] })
        ]
        for (const t of texts) {
            const r = DashboardBook.parse(t, "phone")
            compare(r.problem, "damaged", t)
            compare(r.doc, DashboardBook.builtIn("phone"), t)
        }
    }

    // A newer format may change anything, so nothing of it is read
    function test_aNewerFormatGivesTheBuiltInLayout() {
        for (const d of [{ format: 2, pages: book().pages }, { format: 2, pages: "x" },
                         { format: 9 }]) {
            const r = DashboardBook.parse(JSON.stringify(d), "desktop")
            compare(r.problem, "newer", JSON.stringify(d))
            compare(r.doc, DashboardBook.builtIn("desktop"))
        }
    }

    // One bad field costs its cell; the page and the other cell read
    function test_aWrongCellFieldDropsThatCellOnly() {
        const long = "x".repeat(41)
        const nine = ["a", "b", "c", "d", "e", "f", "g", "h", "i"]
        const bad = {
            "not an object": "x", "null": null, "an array": [],
            "no kind": { kind: undefined }, "an empty kind": { kind: "" }, "a kind not text": { kind: 7 },
            "w 0": { w: 0 }, "w 13": { w: 13 }, "w 1.5": { w: 1.5 }, "w text": { w: "2" },
            "no w": { w: undefined },
            "h 0": { h: 0 }, "h 17": { h: 17 }, "no h": { h: undefined },
            "no refs": { refs: undefined }, "refs text": { refs: "id:1" }, "a ref not text": { refs: [1] },
            "9 refs": { refs: nine },
            "a label not text": { label: 7 }, "a label of 41": { label: long },
            "a label of 41 code points": { label: emoji.repeat(41) },
            "a unit not text": { unit: 7 },
            "options an array": { options: [] }, "options null": { options: null },
            "options text": { options: "x" }
        }
        const badOptions = {
            from: "0", to: null, coldBelow: "x", warnFrom: true, dangerFrom: [], rise: "x",
            majorStep: 0, minorStep: -1, labelDivisor: 0,
            decimals: 7, count: 33, cells: 1, digits: 9, window: 121,
            peak: "yes", progress: 1, vertical: 0, marks: "true",
            caption: "x".repeat(41), description: "x".repeat(81), peakLabel: "x".repeat(9),
            accent: 7
        }
        for (const k in badOptions) {
            const o = { from: 0, to: 120 }
            o[k] = badOptions[k]
            bad["option " + k] = { options: o }
        }
        const lowOptions = { decimals: -1, count: 0, cells: 65, digits: 0, window: 9 }
        for (const k in lowOptions) {
            const o = {}
            o[k] = lowOptions[k]
            bad["option " + k + " " + lowOptions[k]] = { options: o }
        }
        bad["decimals 1.5"] = { options: { decimals: 1.5 } }
        bad["from equal to to"] = { options: { from: 5, to: 5 } }
        bad["from above to"] = { options: { from: 6, to: 5 } }

        for (const name in bad) {
            const d = book()
            const change = bad[name]
            if (change === null || typeof change !== "object" || Array.isArray(change)) {
                d.pages[0].cells[0] = change
            } else {
                for (const k in change) {
                    if (change[k] === undefined)
                        delete d.pages[0].cells[0][k]
                    else
                        d.pages[0].cells[0][k] = change[k]
                }
            }
            const r = read(d)
            compare(r.problem, "dropped", name)
            compare(r.dropped, { pages: 0, cells: 1 }, name)
            compare(r.doc.pages.length, 2, name)
            compare(labels(r.doc, 0), ["Oil"], name)
            verify(DashboardBook.cellProblem(d.pages[0].cells[0]) !== "", name)
        }
    }

    // Each bound's edge reads, and what the rules leave unchecked reads
    function test_eachBoundsEdgeReads() {
        const eight = ["a", "b", "c", "d", "e", "f", "g", "h"]
        const ok = {
            "w 1": { w: 1 }, "w 12": { w: 12 }, "h 1": { h: 1 }, "h 16": { h: 16 },
            "8 refs": { refs: eight }, "no refs": { refs: [] }, "an empty label": { label: "" },
            "a label of 40": { label: "x".repeat(40) },
            "a label of 40 code points": { label: emoji.repeat(40) },
            "no options": { options: {} },
            "options at their edges": { options: {
                decimals: 0, count: 1, cells: 2, digits: 1, window: 10,
                caption: "x".repeat(40), description: "x".repeat(80), peakLabel: emoji.repeat(8),
                majorStep: 0.5, rise: -3, peak: false, marks: true } },
            "options at their other edges": { options: {
                decimals: 6, count: 32, cells: 64, digits: 8, window: 120 } },
            // Zones aren't checked against the range: one outside it
            // doesn't draw
            "zones outside the range": { options: { from: 0, to: 100, coldBelow: -50,
                                                    warnFrom: 150, dangerFrom: 999 } },
            "only a from": { options: { from: 500 } },
            "an accent of a later version": { options: { accent: "amber" } }
        }
        for (const name in ok) {
            const d = book()
            for (const k in ok[name])
                d.pages[0].cells[0][k] = ok[name][k]
            const r = read(d)
            compare(r.problem, "", name)
            compare(r.doc, d, name)
            compare(DashboardBook.cellProblem(d.pages[0].cells[0]), "", name)
        }
    }

    // A page's own fields cost that page; its cells' count is the
    // document's (below)
    function test_aWrongPageFieldDropsThatPage() {
        const bad = ["x", null, [], { cells: [] }, { name: "", cells: [] },
                     { name: "   ", cells: [] }, { name: 7, cells: [] },
                     { name: "x".repeat(41), cells: [] }, { name: "P" }, { name: "P", cells: {} }]
        for (const p of bad) {
            const d = book()
            d.pages.splice(1, 0, p)
            const r = read(d)
            compare(r.problem, "dropped", JSON.stringify(p))
            compare(r.dropped, { pages: 1, cells: 0 }, JSON.stringify(p))
            compare(r.doc, book(), JSON.stringify(p))
        }
        // The name's 40 is counted after trimming
        const d = book()
        d.pages[1].name = "  " + emoji.repeat(40) + "  "
        compare(read(d).doc, d)
    }

    // Past the document's own bounds the whole text is damaged
    function test_theDocumentsBounds() {
        const ten = book()
        while (ten.pages.length < 10)
            ten.pages.push({ name: "P" + ten.pages.length, cells: [] })
        compare(read(ten).problem, "")
        const eleven = JSON.parse(JSON.stringify(ten))
        eleven.pages.push({ name: "P10", cells: [] })
        compare(read(eleven).problem, "damaged")
        // A bad page still counts toward the 10
        const elevenWithBad = JSON.parse(JSON.stringify(ten))
        elevenWithBad.pages.push("x")
        compare(read(elevenWithBad).problem, "damaged")

        compare(read(pageOf(24)).problem, "")
        compare(read(pageOf(25)).problem, "damaged")
        // So do bad cells toward the 24, even on a page that's dropped
        const badCells = pageOf(24)
        badCells.pages[0].cells.push("x")
        compare(read(badCells).problem, "damaged")
        const droppedPage = pageOf(25)
        droppedPage.pages[0].name = ""
        droppedPage.pages.push({ name: "Q", cells: [] })
        compare(read(droppedPage).problem, "damaged")

        // 64 KiB, as UTF-8 bytes of the text
        const atBound = docOfBytes(65536)
        compare(bytes(JSON.stringify(atBound)), 65536)
        verify(JSON.stringify(atBound).length < 65536)
        compare(read(atBound).problem, "")
        const pastBound = docOfBytes(65537)
        compare(bytes(JSON.stringify(pastBound)), 65537)
        compare(read(pastBound).problem, "damaged")
        // The text as stored counts, spaces and all, though it would be
        // written back shorter
        compare(DashboardBook.parse(JSON.stringify(atBound) + " ", "phone").problem, "damaged")

        // The bound is on the text a write stores: a text under it that
        // is written back longer is past it
        const head = '{"format":1,"pages":[{"name":"P","cells":[]}],"n":['
                     + Array(100).fill("1e5").join(",") + '],"pad":"'
        const longer = head + "a".repeat(65400 - head.length - 2) + '"}'
        compare(bytes(longer), 65400)
        verify(bytes(DashboardBook.serialize(JSON.parse(longer))) > 65536)
        compare(DashboardBook.parse(longer, "phone").problem, "damaged")
        compare(DashboardBook.parse(longer.replace(/1e5/g, "1e1"), "phone").problem, "")
    }

    // The first edit after a text that couldn't be read keeps it in
    // <form>Unread, and what's saved reads as it is
    function test_theFirstEditKeepsAnUnreadText() {
        const doc = DashboardBook.builtIn("phone")
        const part = book()
        part.pages[0].cells[0].w = "x"
        const cases = [["damaged", "{"], ["newer", JSON.stringify({ format: 2 })],
                       ["dropped", JSON.stringify(part)], ["none", ""],
                       ["", JSON.stringify(book())]]
        for (const [problem, stored] of cases) {
            const r = DashboardBook.parse(stored, "phone")
            compare(r.problem, problem, stored)
            const edited = DashboardBook.renamePage(r.doc, 0, "Street")
            const s = DashboardBook.saveTexts(r.problem, stored, edited)
            compare(s.text, DashboardBook.serialize(edited), problem)
            compare(s.unread, problem === "damaged" || problem === "newer" || problem === "dropped"
                              ? stored : "", problem)
            const again = DashboardBook.parse(s.text, "phone")
            compare(again.problem, "", problem)
            compare(again.doc, edited, problem)
        }
        // A setting never written may read as undefined: nothing to keep
        compare(DashboardBook.saveTexts("none", undefined, doc).unread, "")
    }

    // ---- Placement

    function spots(list) {
        return list.map(s => [s.x, s.y, s.w, s.h])
    }

    function test_placementOfTheBuiltInLayouts() {
        const phone = DashboardBook.builtIn("phone").pages[0].cells
        const desktop = DashboardBook.builtIn("desktop").pages[0].cells
        compare(spots(DashboardBook.place(phone, 2)), [
            [0, 0, 2, 1], [0, 1, 1, 4], [1, 1, 1, 4], [0, 5, 1, 4], [1, 5, 1, 4],
            [0, 9, 1, 4], [1, 9, 1, 4], [0, 13, 1, 2], [1, 13, 1, 2]])
        compare(spots(DashboardBook.place(phone, 4)), [
            [0, 0, 4, 1], [0, 1, 1, 4], [1, 1, 1, 4], [2, 1, 1, 4], [3, 1, 1, 4],
            [0, 5, 1, 4], [1, 5, 1, 4], [2, 5, 1, 2], [3, 5, 1, 2]])
        compare(spots(DashboardBook.place(desktop, 12)), [
            [0, 0, 12, 1], [0, 1, 2, 8], [2, 1, 2, 8], [4, 1, 1, 4], [5, 1, 1, 4],
            [6, 1, 1, 4], [7, 1, 1, 4], [8, 1, 2, 2], [10, 1, 2, 2]])
        // A desktop window narrowed keeps the desktop pages at fewer columns
        compare(spots(DashboardBook.place(desktop, 4)), [
            [0, 0, 4, 1], [0, 1, 2, 8], [2, 1, 2, 8], [0, 9, 1, 4], [1, 9, 1, 4],
            [2, 9, 1, 4], [3, 9, 1, 4], [0, 13, 2, 2], [2, 13, 2, 2]])
        // The rings and tiles beside the dials from 5 columns up
        compare(spots(DashboardBook.place(desktop, 6)).slice(3, 7), [
            [4, 1, 1, 4], [5, 1, 1, 4], [4, 5, 1, 4], [5, 5, 1, 4]])
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
            compare(DashboardBook.defaultRange(unit, -1e6, 1e6), { from: from, to: to }, unit)
        compare(DashboardBook.defaultRange("N\u00B7m", -10, 50), { from: -10, to: 50 })
        compare(DashboardBook.defaultRange("", -40, 215), { from: -40, to: 215 })
        // A name an object already has is no unit in the table
        compare(DashboardBook.defaultRange("toString", 1, 2), { from: 1, to: 2 })
        const fallback = [[NaN, NaN], [NaN, 10], [0, Infinity], [5, 5], [6, 5],
                          [undefined, undefined], ["0", "10"]]
        for (const [lo, hi] of fallback)
            compare(DashboardBook.defaultRange("lb", lo, hi), { from: 0, to: 100 }, lo + " " + hi)
    }

    // ---- Writes

    function test_aCellMovesWithinItsPage() {
        const d = pageOf(4)
        compare(labels(DashboardBook.moveCell(d, 0, 0, 3), 0), ["c1", "c2", "c3", "c0"])
        compare(labels(DashboardBook.moveCell(d, 0, 3, 0), 0), ["c3", "c0", "c1", "c2"])
        compare(labels(DashboardBook.moveCell(d, 0, 1, 2), 0), ["c0", "c2", "c1", "c3"])
        compare(labels(DashboardBook.moveCell(d, 0, 2, 1), 0), ["c0", "c2", "c1", "c3"])
        compare(labels(DashboardBook.moveCell(d, 0, 2, 2), 0), ["c0", "c1", "c2", "c3"])
    }

    function test_aResizeIsClampedToTheBounds() {
        const size = (w, h) => {
            const c = DashboardBook.resizeCell(book(), 0, 0, w, h).pages[0].cells[0]
            return [c.w, c.h]
        }
        compare(size(0, 0), [1, 1])
        compare(size(-5, -5), [1, 1])
        compare(size(99, 99), [12, 16])
        compare(size(12, 16), [12, 16])
        compare(size(2.4, 3.6), [2, 4])
    }

    function test_cellsAreAddedAndRemoved() {
        const added = DashboardBook.addCell(book(), 1, cell("Boost"))
        compare(labels(added, 1), ["Boost"])
        const more = DashboardBook.addCell(added, 1, cell("MAP"))
        compare(labels(more, 1), ["Boost", "MAP"])
        compare(labels(DashboardBook.removeCell(more, 1, 0), 1), ["MAP"])
        compare(labels(DashboardBook.removeCell(book(), 0, 1), 0), ["IAT"])
    }

    function test_setOptionsMergesAndNullRemoves() {
        const d = DashboardBook.setOptions(book(), 0, 0, { warnFrom: 55, from: -40, flash: true })
        compare(d.pages[0].cells[0].options, { from: -40, to: 120, warnFrom: 55, flash: true })
        const e = DashboardBook.setOptions(d, 0, 0, { warnFrom: null, flash: null })
        compare(e.pages[0].cells[0].options, { from: -40, to: 120 })
        // A cell stored without options gets them
        const bare = book()
        delete bare.pages[0].cells[0].options
        compare(DashboardBook.setOptions(bare, 0, 0, { peak: true }).pages[0].cells[0].options,
                { peak: true })
    }

    function test_setCellChangesLabelRefsAndUnit() {
        const d = DashboardBook.setCell(book(), 0, 0, { label: "Intake", refs: ["key:iat2"],
                                                        unit: "\u00B0F" })
        const c = d.pages[0].cells[0]
        compare([c.label, c.refs, c.unit, c.kind, c.w, c.h], ["Intake", ["key:iat2"], "\u00B0F",
                                                               "digital", 1, 4])
        const e = DashboardBook.setCell(d, 0, 0, { label: null, unit: null })
        compare(e.pages[0].cells[0].label, undefined)
        compare(e.pages[0].cells[0].unit, undefined)
        compare(DashboardBook.labelOf(e.pages[0].cells[0]), "")
    }

    function test_thePageFunctions() {
        const added = DashboardBook.addPage(book(), "  Laps  ")
        compare(added.pages.map(p => p.name), ["Engine", "Track", "Laps"])
        compare(added.pages[2].cells, [])
        compare(DashboardBook.renamePage(added, 1, " Street ").pages[1].name, "Street")
        compare(DashboardBook.movePage(added, 2, 0).pages.map(p => p.name),
                ["Laps", "Engine", "Track"])
        compare(DashboardBook.movePage(added, 0, 1).pages.map(p => p.name),
                ["Track", "Engine", "Laps"])
        compare(DashboardBook.removePage(added, 0).pages.map(p => p.name), ["Track", "Laps"])
        compare(DashboardBook.removePage(added, 0).pages[0].cells, [])
    }

    function test_theControlsState() {
        const nine = book()
        while (nine.pages.length < 9)
            nine.pages.push({ name: "P", cells: [] })
        verify(DashboardBook.canAddPage(nine))
        verify(!DashboardBook.canAddPage(DashboardBook.addPage(nine, "Last")))
        verify(DashboardBook.canRemovePage(book()))
        verify(!DashboardBook.canRemovePage(DashboardBook.removePage(book(), 1)))
        verify(DashboardBook.canAddCell(pageOf(23), 0))
        verify(!DashboardBook.canAddCell(pageOf(24), 0))
        verify(!DashboardBook.canAddCell(book(), 2))
        verify(!DashboardBook.canAddCell(book(), -1))
        verify(!DashboardBook.canAddPage({ format: 1, pages: "x" }))
        compare(DashboardBook.cellProblem(cell("IAT")), "")
        verify(DashboardBook.cellProblem(cell("IAT", { w: 0 })) !== "")
        compare(DashboardBook.pageNameProblem("Track"), "")
        verify(DashboardBook.pageNameProblem(" ") !== "")
    }

    // A write that would leave a document that doesn't read back as it is
    // returns a copy equal to its input, and never changes the input
    function test_aRefusedWriteReturnsAnEqualCopy() {
        const near = docOfBytes(65400)
        const tenPages = book()
        while (tenPages.pages.length < 10)
            tenPages.pages.push({ name: "P", cells: [] })
        const refused = [
            ["a 25th cell", pageOf(24), d => DashboardBook.addCell(d, 0, cell("x"))],
            ["a bad cell", book(), d => DashboardBook.addCell(d, 0, cell("x", { w: 0 }))],
            ["a cell not an object", book(), d => DashboardBook.addCell(d, 0, "x")],
            ["a cell on no page", book(), d => DashboardBook.addCell(d, 2, cell("x"))],
            ["a cell past 64 KiB", near, d => DashboardBook.addCell(d, 0,
                                                 cell("x", { more: "y".repeat(200) }))],
            ["removing no cell", book(), d => DashboardBook.removeCell(d, 0, 2)],
            ["removing a cell of no page", book(), d => DashboardBook.removeCell(d, 5, 0)],
            ["moving from no cell", book(), d => DashboardBook.moveCell(d, 0, 2, 0)],
            ["moving to no cell", book(), d => DashboardBook.moveCell(d, 0, 0, -1)],
            ["moving past the end", book(), d => DashboardBook.moveCell(d, 0, 0, 2)],
            ["moving a fraction", book(), d => DashboardBook.moveCell(d, 0, 0.5, 1)],
            ["a resize to NaN", book(), d => DashboardBook.resizeCell(d, 0, 0, NaN, 2)],
            ["a resize by text", book(), d => DashboardBook.resizeCell(d, 0, 0, "2", 2)],
            ["a resize of no cell", book(), d => DashboardBook.resizeCell(d, 1, 0, 2, 2)],
            ["setCell's kind", book(), d => DashboardBook.setCell(d, 0, 0, { kind: "ring" })],
            ["setCell's span", book(), d => DashboardBook.setCell(d, 0, 0, { w: 2 })],
            ["setCell's options", book(), d => DashboardBook.setCell(d, 0, 0, { options: {} })],
            ["9 refs", book(), d => DashboardBook.setCell(d, 0, 0,
                                     { refs: ["a", "b", "c", "d", "e", "f", "g", "h", "i"] })],
            ["refs removed", book(), d => DashboardBook.setCell(d, 0, 0, { refs: null })],
            ["a label of 41", book(), d => DashboardBook.setCell(d, 0, 0,
                                                                 { label: "x".repeat(41) })],
            ["setCell with no fields", book(), d => DashboardBook.setCell(d, 0, 0, null)],
            ["from at to", book(), d => DashboardBook.setOptions(d, 0, 0, { from: 120 })],
            ["decimals 7", book(), d => DashboardBook.setOptions(d, 0, 0, { decimals: 7 })],
            ["accent not text", book(), d => DashboardBook.setOptions(d, 0, 0, { accent: 1 })],
            ["options not an object", book(), d => DashboardBook.setOptions(d, 0, 0, [1])],
            ["options of no cell", book(), d => DashboardBook.setOptions(d, 1, 0, { peak: true })],
            ["an 11th page", tenPages, d => DashboardBook.addPage(d, "More")],
            ["a page with no name", book(), d => DashboardBook.addPage(d, "  ")],
            ["a page named by a number", book(), d => DashboardBook.addPage(d, 7)],
            ["a page's name of 41", book(), d => DashboardBook.renamePage(d, 0, "x".repeat(41))],
            ["renaming no page", book(), d => DashboardBook.renamePage(d, 2, "x")],
            ["moving no page", book(), d => DashboardBook.movePage(d, 0, 2)],
            ["removing the last page", DashboardBook.removePage(book(), 1),
             d => DashboardBook.removePage(d, 0)],
            ["removing no page", book(), d => DashboardBook.removePage(d, 2)],
            ["writing to a document that doesn't read", { format: 1, pages: [] },
             d => DashboardBook.addPage(d, "P")],
            // Values stored text doesn't hold as they are
            ["a cell holding a function", book(),
             d => DashboardBook.addCell(d, 0, cell("x", { onTap: () => 1 }))],
            ["a cell's option a date", book(),
             d => DashboardBook.addCell(d, 0, cell("x", { options: { caption: new Date(0) } }))],
            ["a label a date", book(), d => DashboardBook.setCell(d, 0, 0, { label: new Date(0) })],
            ["refs holding a date", book(),
             d => DashboardBook.setCell(d, 0, 0, { refs: ["key:a", new Date(0)] })],
            ["refs holding undefined", book(),
             d => DashboardBook.setCell(d, 0, 0, { refs: ["key:a", undefined] })],
            ["an option a date", book(),
             d => DashboardBook.setOptions(d, 0, 0, { caption: new Date(0) })],
            ["an option a function", book(),
             d => DashboardBook.setOptions(d, 0, 0, { flash: () => 1 })],
            ["an unknown option NaN", book(), d => DashboardBook.setOptions(d, 0, 0, { glow: NaN })],
            ["an unknown option holding Infinity", book(),
             d => DashboardBook.setOptions(d, 0, 0, { extra: { a: Infinity } })]
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
        const added = DashboardBook.addCell(book(), 1, c)
        c.refs.push("id:9")
        c.options.from = -5
        compare(added.pages[1].cells[0].refs, ["id:1005", "id:7005"])
        compare(added.pages[1].cells[0].options.from, 0)

        const refs = ["key:a"]
        const set = DashboardBook.setCell(book(), 0, 0, { refs: refs })
        refs.push("key:b")
        compare(set.pages[0].cells[0].refs, ["key:a"])

        const extra = { a: 1 }
        const opt = DashboardBook.setOptions(book(), 0, 0, { extra: extra })
        extra.a = 2
        compare(opt.pages[0].cells[0].options.extra, { a: 1 })
    }

    // A key holding undefined is left out: of a change, of a cell added,
    // and of the result, which reads back with the same keys
    function test_aKeyHoldingUndefinedIsLeftOut() {
        const d = DashboardBook.setOptions(book(), 0, 0, { from: undefined, warnFrom: 50 })
        compare(d.pages[0].cells[0].options, { from: 0, to: 120, warnFrom: 50 })
        const e = DashboardBook.setCell(book(), 0, 0, { label: undefined, unit: "x" })
        compare([e.pages[0].cells[0].label, e.pages[0].cells[0].unit], ["IAT", "x"])
        const f = DashboardBook.addCell(book(), 1, cell("x", {
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
            ["addCell", d => DashboardBook.addCell(d, 1, cell("Boost"))],
            ["removeCell", d => DashboardBook.removeCell(d, 0, 0)],
            ["moveCell", d => DashboardBook.moveCell(d, 0, 0, 1)],
            ["resizeCell", d => DashboardBook.resizeCell(d, 0, 1, 3, 9)],
            ["setCell", d => DashboardBook.setCell(d, 0, 1, { label: emoji + " Oil",
                                                              refs: ["key:oil"] })],
            ["setOptions", d => DashboardBook.setOptions(d, 0, 1, { peak: true, to: null,
                                                                    accent: "amber" })],
            ["addPage", d => DashboardBook.addPage(d, "Laps")],
            ["renamePage", d => DashboardBook.renamePage(d, 1, "Laps")],
            ["movePage", d => DashboardBook.movePage(d, 1, 0)],
            ["removePage", d => DashboardBook.removePage(d, 0)]
        ]
        for (const form of ["", "phone", "desktop"]) {
            for (const [name, write] of writes) {
                const input = form === "" ? book() : DashboardBook.addPage(
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
}
