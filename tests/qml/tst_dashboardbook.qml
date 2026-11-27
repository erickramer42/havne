// File/layer: tests/qml/tst_dashboardbook.qml | Qt Quick Test, QML tier
// Testing: QML: QmlTest.dashboardbook (test_qml)

import QtQuick
import QtTest
import App
import AppTest

DashboardBookKit {
    name: "DashboardBook"

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
        compare(c[7].options, { from: 0, to: 500, description: "Mass air flow" })
        compare(c[8].options, { from: 0.5, to: 2, description: "Commanded equivalence ratio" })
        compare(c[9].options, { from: 10, to: 16, dangerBelow: 11.8, warnBelow: 12.4, warnFrom: 14.8, dangerFrom: 15.2 })
    }

    // The built-in layouts read the zone table: a cell on a table signal has
    // exactly the table's zones, and the shift lights the band
    function test_theBuiltInLayoutsTakeTheirZonesFromTheTable() {
        for (const form of ["desktop", "phone"]) {
            for (const c of DashboardBook.builtIn(form).pages[0].cells) {
                const key = c.refs && c.refs.length ? Object.keys(Bridge.gaugeSignals).find(
                    k => Bridge.gaugeSignals[k][0] === c.refs[0]) : undefined
                if (key === undefined || c.kind === "shift-light")
                    continue
                const zones = DashboardBook.zonesFor(key)
                for (const name of DashboardBook.zoneOptions)
                    compare(c.options[name], zones[name], form + " " + c.label + " " + name)
            }
            const shift = DashboardBook.builtIn(form).pages[0].cells[0]
            compare([shift.kind, shift.options.from, shift.options.to],
                    ["shift-light", DashboardBook.shiftBand.from, DashboardBook.shiftBand.to])
        }
        const battery = DashboardBook.zonesFor("battery")
        verify(battery.dangerBelow < battery.warnBelow && battery.warnBelow < battery.warnFrom
               && battery.warnFrom < battery.dangerFrom, "the battery's four levels are ordered")
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
        const edited = DashboardEdit.renamePage(r.doc, 0, "Q")
        compare(edited.pages[0].name, "Q", "the first edit is kept")
        const again = DashboardBook.parse(DashboardBook.serialize(edited), "phone")
        compare(again.problem, "")
        compare(again.doc, edited)
        // A byte past format 1's own limit is damaged, as it always was
        compare(DashboardBook.parse(fullFormat1(65537), "phone").problem, "damaged")
    }

    // ---- The phone's height control

    function test_aRowStepMovesToTheNextMultipleOfThree() {
        compare(DashboardEdit.heightInColumns(18), "4.5", "a square dial's 18 rows")
        compare(DashboardEdit.heightInColumns(12), "3")
        compare(DashboardEdit.heightInColumns(3), "0.75", "one step")
        compare(DashboardEdit.heightInColumns(7), "1.75", "a stored odd height")
        compare(DashboardEdit.stepRows(12, true), 15)
        compare(DashboardEdit.stepRows(12, false), 9)
        compare(DashboardEdit.stepRows(4, true), 6, "a stored odd h, up")
        compare(DashboardEdit.stepRows(4, false), 3, "and down")
        compare(DashboardEdit.stepRows(1, true), 3)
        compare(DashboardEdit.stepRows(3, false), 3, "no lower than one step")
        compare(DashboardEdit.stepRows(2, false), 2, "an odd h under it stays")
        compare(DashboardEdit.stepRows(47, true), 48)
        compare(DashboardEdit.stepRows(48, true), 48, "the bound")
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
        const w = DashboardEdit.renamePage(r.doc, 1, "Laps")
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
            const edited = DashboardEdit.renamePage(r.doc, 0, "Street")
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

    readonly property var beside: [{ baseDef: 7, baseRef: "id:23", baseTick: "7E8:4133:2.0:8", standing: 40, standingKey: "boost", name: "Boost", part: "Example import" }]
    function carryDoc(refsList) {
        return { format: 2, pages: [{ name: "One", cells: refsList.map(
            refs => DashboardBook.gaugeCell("digital", 3, 3, "", refs, "", {})) }] }
    }

    // The first ref the set has wins: a set lacking the first passes to the next
    function test_carryOverPutsTheKeyFirst() {
        const has = r => r !== "key:other"
        const doc = carryDoc([["id:23"], ["key:other", "id:23"], ["id:12"]])
        const r = DashboardBook.carryOver(doc, beside, has)
        compare(r.count, 2)
        compare(r.doc.pages[0].cells.map(c => c.refs), [["key:boost", "id:23"], ["key:boost", "key:other", "id:23"], ["id:12"]])
        compare(doc.pages[0].cells[0].refs, ["id:23"], "the argument is unchanged")
        compare(DashboardBook.carryOver(doc, beside, r => false).count, 0, "a set that has none")
    }

    function test_carryOverLeavesAListAlreadyOnTheStandingSignal() {
        const doc = carryDoc([["key:boost", "id:23"]])
        const r = DashboardBook.carryOver(doc, beside, r => true)
        compare(r.count, 0)
        verify(r.doc === doc)
    }

    function test_carryOverDropsTheLastRefAtTheCap() {
        const full = ["id:23"].concat(["a", "b", "c", "d", "e", "f", "g"].map(k => "key:" + k))
        compare(full.length, DashboardBook.maxRefs)
        const r = DashboardBook.carryOver(carryDoc([full]), beside, r => true)
        const refs = r.doc.pages[0].cells[0].refs
        compare(refs.length, DashboardBook.maxRefs)
        compare(refs[0], "key:boost")
        compare(refs[refs.length - 1], "key:f", "the last one goes")
    }
}
