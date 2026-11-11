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
            format: DashboardBook.format,
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
        return { format: DashboardBook.format, pages: [{ name: "P", cells: cells }] }
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
    function docOfBytes(n, format) {
        const d = { format: format === undefined ? DashboardBook.format : format,
                    pages: [{ name: "P", cells: [] }], pad: "\u20AC" + emoji }
        let pad = n - bytes(JSON.stringify(d))
        d.pad += "\u00E9".repeat(Math.floor(pad / 2)) + (pad % 2 ? "a" : "")
        return d
    }

    // Boost has no key in Bridge.gaugeSignals: the demo's signal alone
    function refsOf(key) {
        const out = []
        for (const r of key === "boost" ? ["id:13001"] : Bridge.gaugeSignals[key])
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
    function test_theBuiltInLayoutsGauges_data() {
        return [
            { tag: "phone", form: "phone", count: 11, want: [
                ["Shift lights", "shift-light", "rpm", "RPM", [12, 3]],
                ["Engine speed", "analog", "rpm", "RPM", [4, 18]],
                ["Speed", "digital", "speed", "km/h", [2, 6]],
                ["Coolant", "digital", "coolant", degC, [2, 6]],
                ["Oil temp", "digital", "oil", degC, [2, 6]],
                ["MAF", "digital", "maf", "g/s", [3, 9]],
                ["Boost", "digital", "boost", "kPa", [3, 9]],
                ["IAT", "digital", "iat", degC, [2, 6]],
                ["Battery", "digital", "battery", "V", [2, 6]],
                ["Lambda", "digital", "lambda", "\u03BB", [2, 6]]
            ] },
            { tag: "desktop", form: "desktop", count: 15, want: [
                ["Shift lights", "shift-light", "rpm", "RPM", [12, 1]],
                ["Engine speed", "analog", "rpm", "RPM", [2, 8]],
                ["Vehicle speed", "analog", "speed", "km/h", [2, 8]],
                ["Coolant", "ring", "coolant", degC, [1, 4]],
                ["Oil temp", "ring", "oil", degC, [1, 4]],
                ["Boost", "analog", "boost", "kPa", [1, 4]],
                ["IAT", "digital", "iat", degC, [1, 4]],
                ["MAF", "digital", "maf", "g/s", [1, 4]],
                ["Lambda", "digital", "lambda", "\u03BB", [1, 4]],
                ["Battery", "digital", "battery", "V", [1, 4]]
            ] }
        ]
    }
    function test_theBuiltInLayoutsGauges(data) {
        const b = DashboardBook.builtIn(data.form)
        compare(b.format, 2)
        compare(b.pages.length, 1)
        compare(b.pages[0].name, "Engine")
        const cells = b.pages[0].cells
        compare(cells.length, data.want.length)
        for (let i = 0; i < data.want.length; ++i) {
            const [label, kind, key, unit, span] = data.want[i]
            const c = cells[i]
            compare(c.label, label)
            compare(c.kind, kind, label)
            verify(DashboardBook.knownKind(c.kind), label)
            compare(c.refs, refsOf(key), label)
            compare(c.unit, unit, label)
            compare([c.w, c.h], span, label)
            const accent = c.options.accent
            verify(accent === undefined || DashboardBook.knownAccent(accent), label)
        }
        compare(cells[0].options, { from: 4500, to: 6500, count: data.count })
        // The two RPM cells' lists are each their own
        verify(cells[0].refs !== cells[1].refs)
    }

    function test_theDesktopsBuiltInOptions() {
        const c = DashboardBook.builtIn("desktop").pages[0].cells
        compare(c[1].options, { from: 0, to: 8000, majorStep: 1000, minorStep: 250,
                                labelDivisor: 1000, dangerFrom: 6800,
                                caption: "\u00D71000 r/min", peak: true, progress: true })
        // No peakLabel of its own: both dials say "Peak"
        compare(c[2].options, { from: 0, to: 260, majorStep: 20, minorStep: 10, peak: true })
        compare(c[3].options, { from: 0, to: 130, coldBelow: 60, warnFrom: 105, dangerFrom: 115 })
        compare(c[4].options, { from: 0, to: 160, coldBelow: 70, warnFrom: 125, dangerFrom: 140 })
        // Vacuum to about 25 psi, the ends in kPa
        compare(c[5].options, { caption: "Boost", from: -100, to: 200 })
        compare(c[6].options, { from: 0, to: 120, warnFrom: 55, dangerFrom: 70,
                                description: "Intake air temperature" })
        // No accent: blue means cold, nothing else
        compare(c[7].options, { from: 0, to: 300, description: "Mass air flow" })
        compare(c[8].options, { from: 0.5, to: 2, description: "Equivalence ratio" })
        compare(c[9].options, { from: 10, to: 16 })
    }

    // No empty square on a desktop grid of 5 to 8 columns (1280 x 800 is 5,
    // 1920 x 1080 is 8), or the phone's 6 or 12: every square is covered by
    // a cell as the built-in layout is drawn (`keepSquare`), and no dial or
    // ring is stretched past its square, since it would draw the gauge with
    // blank space around it
    function emptySquares(form, columns) {
        const cells = DashboardBook.builtIn(form).pages[0].cells
        const boxes = DashboardBook.place(cells, columns)
        const drawn = DashboardBook.stretched(cells, boxes, columns, true)
        const covered = {}
        let stretchedRound = 0
        cells.forEach((c, i) => {
            if ((c.kind === "analog" || c.kind === "ring")
                    && (drawn[i].w !== boxes[i].w || drawn[i].h !== boxes[i].h))
                ++stretchedRound
        })
        let rows = 0
        for (const b of drawn) {
            rows = Math.max(rows, b.y + b.h)
            for (let y = b.y; y < b.y + b.h; ++y)
                for (let x = b.x; x < b.x + b.w; ++x)
                    covered[x + "," + y] = true
        }
        let empty = 0
        for (let y = 0; y < rows; ++y)
            for (let x = 0; x < columns; ++x)
                empty += covered[x + "," + y] ? 0 : 1
        return empty + stretchedRound
    }
    function test_theBuiltInLayoutLeavesNoEmptySquare_data() {
        return [
            { tag: "desktop at 1280 x 800", form: "desktop", columns: 5 },
            { tag: "desktop at 6 columns", form: "desktop", columns: 6 },
            { tag: "desktop at 7 columns", form: "desktop", columns: 7 },
            { tag: "desktop at 1920 x 1080", form: "desktop", columns: 8 },
            { tag: "phone", form: "phone", columns: 6 },
            { tag: "phone on its side", form: "phone", columns: 12 }
        ]
    }
    function test_theBuiltInLayoutLeavesNoEmptySquare(data) {
        compare(emptySquares(data.form, data.columns), 0)
    }

    // ---- Format 1, read and upgraded (ADR-0057's appendix on the six columns)

    // ADR-0057 section 7's sample as a format 1 text
    function sampleCells() {
        const rpm = ["id:1001", "id:7003"]
        return [
            { kind: "shift-light", w: 12, h: 1, label: "Shift lights", refs: rpm.slice(), unit: "RPM",
              options: { from: 4500, to: 6500 } },
            { kind: "analog", w: 2, h: 8, label: "Engine speed", refs: rpm.slice(), unit: "RPM",
              options: { from: 0, to: 8000, dangerFrom: 6800, majorStep: 1000,
                         labelDivisor: 1000, peak: true } },
            { kind: "digital", w: 1, h: 4, label: "Boost", refs: ["key:boost"], unit: "kPa",
              options: { from: -100, to: 150 } }
        ]
    }
    function format1Text(cells, extra) {
        const d = { format: 1, pages: [{ name: "Engine", cells: cells }] }
        for (const k in extra)
            d[k] = extra[k]
        return JSON.stringify(d)
    }
    function spans(doc) {
        return doc.pages[0].cells.map(c => [c.w, c.h])
    }

    // A phone document is three times the spans, in the same places
    function test_aFormat1PhoneDocumentIsScaled() {
        const r = DashboardBook.parse(format1Text(sampleCells()), "phone")
        compare(r.problem, "")
        compare(r.doc.format, 2)
        compare(spans(r.doc), [[12, 3], [6, 24], [3, 12]])
        // Everything else as read
        const want = sampleCells()
        for (let i = 0; i < want.length; ++i) {
            for (const k of ["kind", "label", "refs", "unit", "options"])
                compare(r.doc.pages[0].cells[i][k], want[i][k], k)
        }
        for (const [before, after] of [[2, 6], [4, 12]]) {
            const was = DashboardBook.place(sampleCells(), before)
            const is = DashboardBook.place(r.doc.pages[0].cells, after)
            compare(is.map(b => [b.x, b.y, b.h]), was.map(b => [b.x * 3, b.y * 3, b.h * 3]),
                    before + " columns to " + after)
            compare(is.map(b => b.w), was.map(b => b.w * 3), before + " columns' widths")
        }
    }

    function test_aFormat1DesktopDocumentKeepsItsSpans() {
        const r = DashboardBook.parse(format1Text(sampleCells()), "desktop")
        compare(r.problem, "")
        compare(r.doc.format, 2)
        compare(spans(r.doc), [[12, 1], [2, 8], [1, 4]])
    }

    // The clamp changes nothing on screen: past 4 of 4 is the whole row
    // on 12, and h takes its whole range
    function test_theUpgradesClampAndItsLargestH() {
        const r = DashboardBook.parse(format1Text([cell("Wide", { w: 5, h: 16 })]), "phone")
        compare(r.problem, "")
        compare(spans(r.doc), [[12, 48]])
        compare(spots(DashboardBook.place(r.doc.pages[0].cells, 12)), [[0, 0, 12, 48]])
        compare(spans(DashboardBook.parse(format1Text([cell("Most", { w: 4 })]), "phone").doc),
                [[12, 12]])
    }

    // Format 2 is never multiplied; a format 1 text reads the same twice
    function test_theUpgradeRunsOnceAndOnlyOnFormat1() {
        const two = JSON.stringify({ format: 2, pages: [{ name: "P", cells: sampleCells() }] })
        const r = DashboardBook.parse(two, "phone")
        compare(r.problem, "")
        compare(spans(r.doc), [[12, 1], [2, 8], [1, 4]])
        const text = format1Text(sampleCells())
        compare(DashboardBook.parse(text, "phone").doc, DashboardBook.parse(text, "phone").doc)
    }

    // What's stored after the first edit is format 2 and reads back as is
    function test_theFirstWriteIsFormat2AndRoundTrips() {
        const r = DashboardBook.parse(format1Text(sampleCells()), "phone")
        const text = DashboardBook.serialize(r.doc)
        compare(JSON.parse(text).format, 2)
        compare(JSON.parse(text).pages[0].cells[1].h, 24)
        const again = DashboardBook.parse(text, "phone")
        compare(again.problem, "")
        compare(again.doc, r.doc)
    }

    // What this version doesn't know is kept, and an unknown kind scales
    function test_anUnknownCellAndKeysScale() {
        const later = cell("Later", { kind: "hologram", w: 2, h: 3, depth: 9 })
        later.options = { glow: true }
        const r = DashboardBook.parse(format1Text([later], { theme: "x" }), "phone")
        compare(r.problem, "")
        const c = r.doc.pages[0].cells[0]
        compare([c.kind, c.w, c.h, c.depth, c.options.glow], ["hologram", 6, 9, 9, true])
        compare(r.doc.theme, "x")
    }

    function test_aFormat3TextIsNewer() {
        const r = DashboardBook.parse(JSON.stringify({ format: 3, pages: book().pages }), "phone")
        compare(r.problem, "newer")
        compare(r.doc, DashboardBook.builtIn("phone"))
    }

    // Format 1's bounds are checked before the upgrade, on either form
    function test_format1sBoundsAreCheckedFirst_data() {
        return [{ tag: "phone", form: "phone" }, { tag: "desktop", form: "desktop" }]
    }
    function test_format1sBoundsAreCheckedFirst(data) {
        for (const [name, bad] of [["h 17", { h: 17 }], ["w 13", { w: 13 }], ["h 0", { h: 0 }]]) {
            const r = DashboardBook.parse(format1Text([cell("Bad", bad), cell("Good")]), data.form)
            compare(r.problem, "dropped", name)
            compare(r.dropped, { pages: 0, cells: 1 }, name)
            compare(labels(r.doc, 0), ["Good"], name)
        }
        const scale = data.form === "phone" ? 3 : 1
        const edge = DashboardBook.parse(format1Text([cell("Edge", { w: 1, h: 16 })]), data.form)
        compare(edge.problem, "")
        compare(edge.doc.pages[0].cells[0].h, 16 * scale)
    }

    function test_format2sBoundOnH() {
        const of = h => DashboardBook.parse(JSON.stringify(
            { format: 2, pages: [{ name: "P", cells: [cell("A", { h: h })] }] }), "desktop")
        compare(of(48).problem, "")
        compare(of(48).doc.pages[0].cells[0].h, 48)
        compare(of(17).problem, "")
        compare(of(49).problem, "dropped")
        compare(of(49).dropped, { pages: 0, cells: 1 })
    }

    // The fullest format 1 document that grows: ten pages of 24 cells,
    // each w and h a digit longer, padded to exactly n bytes of text
    function fullFormat1(n) {
        const page = () => ({ name: "P", cells: Array.from({ length: 24 },
                                                           () => cell("c", { w: 4, h: 4 })) })
        const d = { format: 1, pages: Array.from({ length: 10 }, page), pad: "" }
        d.pad = "a".repeat(n - bytes(JSON.stringify(d)))
        return JSON.stringify(d)
    }

    // A document at format 1's limit stays readable and survives its first edit
    function test_theUpgradesGrowthIsAllowed() {
        const text = fullFormat1(DashboardBook.format1MaxBytes)
        compare(bytes(text), 65536)
        const r = DashboardBook.parse(text, "phone")
        compare(r.problem, "", "it reads")
        const grown = bytes(DashboardBook.serialize(r.doc))
        compare(grown, 65536 + 480, "two bytes for each of its cells")
        compare(grown, DashboardBook.maxBytes, "exactly the allowance")
        compare(DashboardBook.documentProblem(r.doc), "")
        const edited = DashboardBook.renamePage(r.doc, 0, "Q")
        compare(edited.pages[0].name, "Q", "the first edit is kept")
        const again = DashboardBook.parse(DashboardBook.serialize(edited), "phone")
        compare(again.problem, "")
        compare(again.doc, edited)
        // A byte past format 1's own limit is damaged, as it always was
        compare(DashboardBook.parse(fullFormat1(65537), "phone").problem, "damaged")
    }

    // ---- The phone's height control

    function test_aRowStepMovesToTheNextMultipleOfThree() {
        compare(DashboardBook.stepRows(12, true), 15)
        compare(DashboardBook.stepRows(12, false), 9)
        compare(DashboardBook.stepRows(4, true), 6, "a stored odd h, up")
        compare(DashboardBook.stepRows(4, false), 3, "and down")
        compare(DashboardBook.stepRows(1, true), 3)
        compare(DashboardBook.stepRows(3, false), 3, "no lower than one step")
        compare(DashboardBook.stepRows(2, false), 2, "an odd h under it stays")
        compare(DashboardBook.stepRows(47, true), 48)
        compare(DashboardBook.stepRows(48, true), 48, "the bound")
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
        for (const d of [{ format: 3, pages: book().pages }, { format: 3, pages: "x" },
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
            "h 0": { h: 0 }, "h 49": { h: 49 }, "no h": { h: undefined },
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
            "w 1": { w: 1 }, "w 12": { w: 12 }, "h 1": { h: 1 }, "h 48": { h: 48 },
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

        // 64 KiB for format 1, as UTF-8 bytes of the text
        const atBound = docOfBytes(65536, 1)
        compare(bytes(JSON.stringify(atBound)), 65536)
        verify(JSON.stringify(atBound).length < 65536)
        compare(read(atBound).problem, "")
        const pastBound = docOfBytes(65537, 1)
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

        // Format 2's bound holds what format 1's upgrade can add
        compare(DashboardBook.maxBytes, 65536 + 2 * 10 * 24)
        const twoAtBound = docOfBytes(DashboardBook.maxBytes)
        compare(read(twoAtBound).problem, "")
        compare(read(docOfBytes(DashboardBook.maxBytes + 1)).problem, "damaged")
    }

    // The first edit after a text that couldn't be read keeps it in
    // <form>Unread, and what's saved reads as it is
    function test_theFirstEditKeepsAnUnreadText() {
        const doc = DashboardBook.builtIn("phone")
        const part = book()
        part.pages[0].cells[0].w = "x"
        const cases = [["damaged", "{"], ["newer", JSON.stringify({ format: 3 })],
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
        compare(size(99, 99), [12, 48])
        compare(size(12, 48), [12, 48])
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
        const near = docOfBytes(DashboardBook.maxBytes - 136)
        const tenPages = book()
        while (tenPages.pages.length < 10)
            tenPages.pages.push({ name: "P", cells: [] })
        const refused = [
            ["a 25th cell", pageOf(24), d => DashboardBook.addCell(d, 0, cell("x"))],
            ["a bad cell", book(), d => DashboardBook.addCell(d, 0, cell("x", { w: 0 }))],
            ["a cell not an object", book(), d => DashboardBook.addCell(d, 0, "x")],
            ["a cell on no page", book(), d => DashboardBook.addCell(d, 2, cell("x"))],
            ["a cell past the size bound", near, d => DashboardBook.addCell(d, 0,
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

    // ---- Copy and paste (ADR-0057's appendix, section 5)

    function test_copyCellGivesACellOfItsOwn() {
        const d = book()
        const c = DashboardBook.copyCell(d, 0, 1)
        compare(c, d.pages[0].cells[1])
        c.options.from = 99
        compare(d.pages[0].cells[1].options.from, 0, "the source is its own")
        compare(DashboardBook.copyCell(d, 0, 2), null, "past the end")
        compare(DashboardBook.copyCell(d, 5, 0), null, "no such page")
        compare(DashboardBook.copyCell(d, 0, 0.5), null, "not an index")
        compare(DashboardBook.copyCell({ format: 99 }, 0, 0), null, "not a document")
    }

    function test_pasteCellPutsACopyAfterTheIndex() {
        const d = pageOf(3)
        const paste = DashboardBook.copyCell(d, 0, 0)
        paste.label = "new"
        compare(labels(DashboardBook.pasteCell(d, 0, -1, paste), 0), ["new", "c0", "c1", "c2"])
        compare(labels(DashboardBook.pasteCell(d, 0, 0, paste), 0), ["c0", "new", "c1", "c2"])
        compare(labels(DashboardBook.pasteCell(d, 0, 2, paste), 0), ["c0", "c1", "c2", "new"])
        compare(labels(d, 0), ["c0", "c1", "c2"], "its input unchanged")

        const out = DashboardBook.pasteCell(d, 0, 0, paste)
        out.pages[0].cells[1].options.from = 42
        compare(paste.options.from, 0, "the pasted cell is its own")
    }

    function test_pasteCellRefusesWhatItCantTake() {
        const d = pageOf(2)
        const c = DashboardBook.copyCell(d, 0, 0)
        const same = out => compare(DashboardBook.serialize(out), DashboardBook.serialize(d))
        same(DashboardBook.pasteCell(d, 0, -2, c))
        same(DashboardBook.pasteCell(d, 0, 2, c))
        same(DashboardBook.pasteCell(d, 3, 0, c))
        same(DashboardBook.pasteCell(d, 0, 0.5, c))
        same(DashboardBook.pasteCell(d, 0, 0, null))
        same(DashboardBook.pasteCell(d, 0, 0, { kind: "digital", w: 99, h: 1, refs: [] }))
        same(DashboardBook.pasteCell(d, 0, 0, { kind: "digital", w: 1, h: 1, refs: [],
                                                options: { from: Date } }))
    }

    function test_aFullPageRefusesAPaste() {
        const d = pageOf(DashboardBook.maxCells)
        const c = DashboardBook.copyCell(d, 0, 0)
        const out = DashboardBook.pasteCell(d, 0, 0, c)
        compare(DashboardBook.serialize(out), DashboardBook.serialize(d))
        verify(!DashboardBook.canAddCell(d, 0))
    }

    // ---- A drag's order

    function test_reorderedMovesOneAndLeavesItsInput() {
        const list = ["a", "b", "c", "d"]
        compare(DashboardBook.reordered(list, 0, 2), ["b", "c", "a", "d"])
        compare(DashboardBook.reordered(list, 3, 1), ["a", "d", "b", "c"])
        compare(DashboardBook.reordered(list, 1, 1), list)
        compare(list, ["a", "b", "c", "d"], "its input unchanged")
    }

    // For every from and to, the moved index agrees with the reordered list
    function test_movedIndexAgreesWithReordered() {
        const list = ["a", "b", "c", "d", "e"]
        for (let from = 0; from < list.length; ++from) {
            for (let to = 0; to < list.length; ++to) {
                const out = DashboardBook.reordered(list, from, to)
                for (let i = 0; i < list.length; ++i) {
                    compare(out[DashboardBook.movedIndex(i, from, to)], list[i],
                            from + " to " + to + ", item " + i)
                }
            }
        }
    }

    function test_dropBoxesAreTheBoxesOfEachPlace() {
        const cells = [{ w: 2, h: 2 }, { w: 2, h: 2 }, { w: 2, h: 2 }]
        const boxes = DashboardBook.dropBoxes(cells, 0, 4)
        compare(boxes.length, 3)
        compare([boxes[0].x, boxes[0].y], [0, 0])
        compare([boxes[1].x, boxes[1].y], [2, 0])
        compare([boxes[2].x, boxes[2].y], [0, 2])
    }

    function test_nearestTakesTheClosestAndTheFirstOnATie() {
        const pts = [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 0, y: 10 }]
        compare(DashboardBook.nearest(pts, 9, 1), 1)
        compare(DashboardBook.nearest(pts, 1, 9), 2)
        compare(DashboardBook.nearest(pts, 5, 0), 0, "a tie is the first")
        compare(DashboardBook.nearest([], 0, 0), -1)
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
        const copied = DashboardBook.copyCell(r.doc, 0, 1)
        compare(copied, blank)
        const pasted = DashboardBook.pasteCell(r.doc, 0, 0, copied)
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
        compare(DashboardBook.digitsFor(data.from, data.to, data.d), data.want)
    }
}
