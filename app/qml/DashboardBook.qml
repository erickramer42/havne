// File/layer: app/qml/DashboardBook.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboardbook (test_qml)

pragma Singleton
import QtQml

// The dashboards (ADR-0057) as rules on one form's document,
// { format: 2, pages: [{ name, cells: [{ kind, w, h, label, refs, unit,
// options }] }] }: a form is "phone" or "desktop", each stored as its
// own text, and a page's cells are in order, placed by place().
//
// Format 1 is read and upgraded in memory (ADR-0057's appendix on the
// six columns): its bounds are checked first, then a phone document's
// spans are scaled to the 6 and 12 column grid and a desktop document
// only takes the new number. The stored text is rewritten by the first
// edit, never by a read.
//
// Reading keeps what it can (section 7): a key it doesn't know, in the
// document, a page, a cell or its options, and a kind or an accent it
// doesn't know, are kept as read and written back. A known field of the
// wrong type or out of its bounds drops its cell (or its page, for the
// page's name and cells); the document's own bounds (pages, cells on a
// page, the text's size) and an unreadable or newer text give the
// built-in layout and a problem instead, the stored text left alone. A
// read that drops a page or cell names that as its problem too, since
// the next write stores the document without it.
//
// Every function takes the document and returns a new one, never
// changing what it was given. A write that would leave a document that
// doesn't read back as it is (an 11th page, a 25th cell, the last page
// removed, an option of the wrong type) returns an unchanged copy, so
// whatever the page stores reads back equal; canAddPage, canAddCell,
// canRemovePage and cellProblem give its controls their state.
QtObject {
    readonly property int format: 2
    readonly property int maxPages: 10
    readonly property int maxCells: 24
    readonly property int maxRefs: 8
    readonly property int maxLabel: 40
    readonly property int maxColumns: 12
    readonly property int maxRows: 48
    readonly property int format1MaxRows: 16
    readonly property int format1MaxBytes: 65536
    // Format 1's bound plus what its upgrade can add: two bytes per cell
    readonly property int maxBytes: format1MaxBytes + 2 * maxPages * maxCells
    // A format 1 phone span times this is the same shape on the 6 and 12
    // column grid; the phone's size control steps by it
    readonly property int phoneSpanScale: 3

    // Append-only: a name is never reused for another drawing (section 1)
    readonly property var kinds: ["analog", "ring", "bar", "segments", "digital",
                                  "seven-segment", "sparkline", "shift-light", "blank"]
    // The Theme roles a cell's accent names; any other string is kept and
    // draws as "accent", the gauges' default
    readonly property var accents: ["accent", "info", "success"]

    // Each known option's rule, one table for every kind: a name means
    // one thing in each, and a kind ignores the ones it has no use for
    readonly property var optionRules: ({
        from: { type: "number" }, to: { type: "number" },
        coldBelow: { type: "number" }, warnFrom: { type: "number" },
        dangerFrom: { type: "number" }, rise: { type: "number" },
        majorStep: { type: "positive" }, minorStep: { type: "positive" },
        labelDivisor: { type: "positive" },
        decimals: { type: "whole", min: 0, max: 6 },
        count: { type: "whole", min: 1, max: 32 },
        cells: { type: "whole", min: 2, max: 64 },
        digits: { type: "whole", min: 1, max: 8 },
        window: { type: "whole", min: 10, max: 120 },
        peak: { type: "bool" }, progress: { type: "bool" },
        vertical: { type: "bool" }, marks: { type: "bool" },
        caption: { type: "text", max: 40 },
        description: { type: "text", max: 80 },
        peakLabel: { type: "text", max: 8 },
        accent: { type: "string" }
    })

    // A new gauge's range by the unit it decodes to (section 6)
    readonly property var unitRanges: ({
        "RPM": [0, 8000], "km/h": [0, 260], "\u00B0C": [0, 150], "%": [0, 100],
        "kPa": [0, 300], "V": [0, 16], "g/s": [0, 300], "L/h": [0, 60]
    })

    function knownKind(kind) {
        return kinds.indexOf(kind) >= 0
    }

    function knownAccent(name) {
        return accents.indexOf(name) >= 0
    }

    // ---- Reading and writing the stored text

    // { doc, problem, dropped }: problem "" (read), "dropped" (read,
    // with pages or cells left out: dropped holds their counts, { pages,
    // cells }), "none" (no stored text), "damaged" or "newer"; doc is the
    // built-in layout of the form for the last three, so the page always
    // has pages to show
    function parse(text, form) {
        if (typeof text !== "string" || text === "")
            return { doc: builtIn(form), problem: "none" }
        if (utf8Bytes(text) > maxBytes)
            return { doc: builtIn(form), problem: "damaged" }
        let d
        try {
            d = JSON.parse(text)
        } catch (e) {
            return { doc: builtIn(form), problem: "damaged" }
        }
        if (!isObject(d) || !Number.isInteger(d.format) || d.format < 1)
            return { doc: builtIn(form), problem: "damaged" }
        if (d.format > format)
            return { doc: builtIn(form), problem: "newer" }
        if (!Array.isArray(d.pages) || d.pages.length > maxPages
                || d.pages.some(p => isObject(p) && Array.isArray(p.cells)
                                     && p.cells.length > maxCells))
            return { doc: builtIn(form), problem: "damaged" }
        const pagesRead = d.pages.length
        const rows = d.format === 1 ? format1MaxRows : maxRows
        d.pages = d.pages.filter(p => pageProblem(p) === "")
        let cellsDropped = 0
        for (const p of d.pages) {
            const cellsRead = p.cells.length
            p.cells = p.cells.filter(c => cellProblem(c, rows) === "")
            cellsDropped += cellsRead - p.cells.length
        }
        if (d.format === 1) {
            if (utf8Bytes(text) > format1MaxBytes || utf8Bytes(serialize(d)) > format1MaxBytes)
                return { doc: builtIn(form), problem: "damaged" }
            upgrade(d, form)
        }
        // What's left must be what a write stores: with no page left, or
        // past the bound once written back (1e5 is written 100000), it isn't
        if (documentProblem(d) !== "")
            return { doc: builtIn(form), problem: "damaged" }
        const pagesDropped = pagesRead - d.pages.length
        if (pagesDropped > 0 || cellsDropped > 0)
            return { doc: d, problem: "dropped",
                     dropped: { pages: pagesDropped, cells: cellsDropped } }
        return { doc: d, problem: "" }
    }

    // A format 1 document, its bounds already checked, as format 2. The
    // form is the key the text came from, never guessed from the content
    function upgrade(d, form) {
        d.format = format
        if (form !== "phone")
            return
        for (const p of d.pages) {
            for (const c of p.cells) {
                c.w = Math.min(maxColumns, c.w * phoneSpanScale)
                c.h = c.h * phoneSpanScale
            }
        }
    }

    function serialize(doc) {
        return JSON.stringify(doc)
    }

    // What the page stores on its first edit after a read: the new text,
    // and the stored text to keep in <form>Unread ("" for none), which
    // is the stored text only when it couldn't be read, was newer or
    // lost a page or cell to the read
    function saveTexts(problem, stored, doc) {
        const keep = problem === "damaged" || problem === "newer" || problem === "dropped"
        return { text: serialize(doc), unread: keep ? String(stored) : "" }
    }

    // ---- The fields' rules

    // "" when a page's own fields may be kept, else why not
    function pageProblem(p) {
        if (!isObject(p))
            return qsTr("A page isn't an object")
        if (pageNameProblem(p.name) !== "")
            return pageNameProblem(p.name)
        if (!Array.isArray(p.cells))
            return qsTr("A page needs a list of cells")
        return ""
    }

    function pageNameProblem(name) {
        if (typeof name !== "string" || name.trim() === "")
            return qsTr("A page's name needs at least one character")
        if (codePoints(name.trim()) > maxLabel)
            return qsTr("At most %1 characters").arg(maxLabel)
        return ""
    }

    // "" when a cell may be kept, else why not (its own fields: the
    // count of cells is the page's, judged where the page is). rows is
    // the bound on h, the current format's unless the caller gives another
    function cellProblem(c, rows) {
        const rowBound = rows === undefined ? maxRows : rows
        if (!isObject(c))
            return qsTr("A gauge isn't an object")
        if (typeof c.kind !== "string" || c.kind === "")
            return qsTr("A gauge needs a kind")
        if (!Number.isInteger(c.w) || c.w < 1 || c.w > maxColumns)
            return qsTr("A gauge is 1 to %1 columns wide").arg(maxColumns)
        if (!Number.isInteger(c.h) || c.h < 1 || c.h > rowBound)
            return qsTr("A gauge is 1 to %1 rows high").arg(rowBound)
        if (!Array.isArray(c.refs) || !c.refs.every(r => typeof r === "string"))
            return qsTr("A gauge's signals are a list of references")
        if (c.refs.length > maxRefs)
            return qsTr("At most %1 signals").arg(maxRefs)
        if (c.label !== undefined
                && (typeof c.label !== "string" || codePoints(c.label) > maxLabel))
            return qsTr("A label of at most %1 characters").arg(maxLabel)
        if (c.unit !== undefined && typeof c.unit !== "string")
            return qsTr("A unit is text")
        if (c.options === undefined)
            return ""
        if (!isObject(c.options))
            return qsTr("A gauge's options are an object")
        for (const name in optionRules) {
            if (c.options[name] !== undefined && !optionFits(optionRules[name], c.options[name]))
                return qsTr("%1 isn't a value it can take").arg(name)
        }
        const o = c.options
        if (o.from !== undefined && o.to !== undefined && o.from >= o.to)
            return qsTr("The scale's start is below its end")
        return ""
    }

    function optionFits(rule, v) {
        switch (rule.type) {
        case "number":   return typeof v === "number" && isFinite(v)
        case "positive": return typeof v === "number" && isFinite(v) && v > 0
        case "whole":    return Number.isInteger(v) && v >= rule.min && v <= rule.max
        case "bool":     return typeof v === "boolean"
        case "text":     return typeof v === "string" && codePoints(v) <= rule.max
        default:         return typeof v === "string"
        }
    }

    // "" when the whole document reads back as it is
    function documentProblem(doc) {
        if (!isObject(doc) || doc.format !== format || !Array.isArray(doc.pages)
                || doc.pages.length < 1 || doc.pages.length > maxPages)
            return "pages"
        for (const p of doc.pages) {
            if (pageProblem(p) !== "" || p.cells.length > maxCells
                    || p.cells.some(c => cellProblem(c) !== ""))
                return "page"
        }
        return utf8Bytes(serialize(doc)) > maxBytes ? "size" : ""
    }

    // The fields a stored cell may leave out, read with their defaults
    function labelOf(cell) {
        return cell.label === undefined ? "" : cell.label
    }
    function unitOf(cell) {
        return cell.unit === undefined ? "" : cell.unit
    }
    function optionsOf(cell) {
        return cell.options === undefined ? {} : cell.options
    }

    // ---- The built-in layout

    function builtIn(form) {
        return { format: format, pages: [{ name: qsTr("Engine"),
                                           cells: form === "phone" ? phoneCells() : desktopCells() }] }
    }

    function gaugeCell(kind, w, h, label, refs, unit, options) {
        return { kind: kind, w: w, h: h, label: label, refs: refs, unit: unit, options: options }
    }

    // On 6 columns: the dial 4 wide with three readouts stacked beside it,
    // a trace across, then three readouts across
    function phoneCells() {
        const rpm = ["id:1001", "id:7003"]
        const degC = "\u00B0C"
        return [
            gaugeCell("shift-light", 12, 3, qsTr("Shift lights"), rpm.slice(), "RPM",
                      { from: 4500, to: 6500, count: 11 }),
            gaugeCell("analog", 4, 18, qsTr("Engine speed"), rpm.slice(), "RPM",
                      { from: 0, to: 8000, majorStep: 1000, minorStep: 250,
                        labelDivisor: 1000, dangerFrom: 6800,
                        caption: qsTr("\u00D71000 r/min"), peak: true, progress: true }),
            gaugeCell("digital", 2, 6, qsTr("Speed"), ["id:3001", "id:7004"], "km/h",
                      { from: 0, to: 260 }),
            gaugeCell("digital", 2, 6, qsTr("Coolant"), ["id:2001", "id:7002"], degC,
                      { from: 0, to: 130, coldBelow: 60, warnFrom: 105, dangerFrom: 115 }),
            gaugeCell("digital", 2, 6, qsTr("Oil temp"), ["id:9001", "id:7010"], degC,
                      { from: 0, to: 160, coldBelow: 70, warnFrom: 125, dangerFrom: 140 }),
            gaugeCell("digital", 6, 8, qsTr("MAF"), ["id:8001", "id:7009"], "g/s",
                      { from: 0, to: 300, description: qsTr("Mass air flow"), accent: "info" }),
            gaugeCell("digital", 2, 7, qsTr("IAT"), ["id:5001", "id:7005"], degC,
                      { from: 0, to: 120, warnFrom: 55, dangerFrom: 70,
                        description: qsTr("Intake air temperature") }),
            gaugeCell("digital", 2, 7, qsTr("Load"), ["id:6001", "id:7001"], "%",
                      { from: 0, to: 100 }),
            gaugeCell("digital", 2, 7, qsTr("Throttle"), ["id:4001", "id:7006"], "%",
                      { from: 0, to: 100 })
        ]
    }

    function desktopCells() {
        const rpm = ["id:1001", "id:7003"]
        const degC = "\u00B0C"
        return [
            gaugeCell("shift-light", 12, 1, qsTr("Shift lights"), rpm.slice(), "RPM",
                      { from: 4500, to: 6500, count: 15 }),
            gaugeCell("analog", 2, 8, qsTr("Engine speed"), rpm.slice(), "RPM",
                      { from: 0, to: 8000, majorStep: 1000, minorStep: 250,
                        labelDivisor: 1000, dangerFrom: 6800,
                        caption: qsTr("\u00D71000 r/min"), peak: true, progress: true }),
            // No caption: the dial shows its unit
            gaugeCell("analog", 2, 8, qsTr("Vehicle speed"), ["id:3001", "id:7004"], "km/h",
                      { from: 0, to: 260, majorStep: 20, minorStep: 10, peak: true,
                        peakLabel: qsTr("TOP") }),
            gaugeCell("ring", 1, 4, qsTr("Coolant"), ["id:2001", "id:7002"], degC,
                      { from: 0, to: 130, coldBelow: 60, warnFrom: 105, dangerFrom: 115 }),
            gaugeCell("ring", 1, 4, qsTr("Oil temp"), ["id:9001", "id:7010"], degC,
                      { from: 0, to: 160, coldBelow: 70, warnFrom: 125, dangerFrom: 140 }),
            gaugeCell("digital", 1, 4, qsTr("IAT"), ["id:5001", "id:7005"], degC,
                      { from: 0, to: 120, warnFrom: 55, dangerFrom: 70,
                        description: qsTr("Intake air temperature") }),
            gaugeCell("digital", 1, 4, qsTr("MAF"), ["id:8001", "id:7009"], "g/s",
                      { from: 0, to: 300, description: qsTr("Mass air flow"), accent: "info" }),
            gaugeCell("bar", 2, 2, qsTr("Throttle"), ["id:4001", "id:7006"], "%",
                      { from: 0, to: 100, accent: "success" }),
            gaugeCell("bar", 2, 2, qsTr("Engine load"), ["id:6001", "id:7001"], "%",
                      { from: 0, to: 100 })
        ]
    }

    function reset(form) {
        return builtIn(form)
    }

    // ---- Placement (section 4)

    // [{ x, y, w, h }] in the cells' order: each at the first free block
    // of its span, rows top to bottom, columns left to right; a span
    // wider than the grid takes the whole row
    function place(cells, columns) {
        // A width not yet laid out (0 / 0) gives no count: one column
        const cols = isFinite(columns) ? Math.max(1, Math.floor(columns)) : 1
        const taken = []   // taken[y][x]
        const free = (x, y, w, h) => {
            for (let r = y; r < y + h; ++r) {
                for (let c = x; c < x + w; ++c) {
                    if (taken[r] && taken[r][c])
                        return false
                }
            }
            return true
        }
        const out = []
        for (const cell of cells) {
            const w = Math.min(cell.w, cols)
            const h = cell.h
            let spot = null
            for (let y = 0; spot === null; ++y) {
                for (let x = 0; x + w <= cols; ++x) {
                    if (free(x, y, w, h)) {
                        spot = { x: x, y: y, w: w, h: h }
                        break
                    }
                }
            }
            for (let r = spot.y; r < spot.y + h; ++r) {
                taken[r] = taken[r] || []
                for (let c = spot.x; c < spot.x + w; ++c)
                    taken[r][c] = true
            }
            out.push(spot)
        }
        return out
    }

    // { from, to } for a new gauge: by the unit it decodes to, then the
    // field's range (finite, lo below hi), then 0 to 100. Never NaN, so a
    // layout never stores one
    function defaultRange(unit, lo, hi) {
        const byUnit = unitRanges[unit]
        if (byUnit !== undefined && Object.prototype.hasOwnProperty.call(unitRanges, unit))
            return { from: byUnit[0], to: byUnit[1] }
        if (typeof lo === "number" && typeof hi === "number" && isFinite(lo) && isFinite(hi)
                && lo < hi)
            return { from: lo, to: hi }
        return { from: 0, to: 100 }
    }

    // ---- A drag (section 4): one cell's order while it's held

    // The list with the item at from moved to place to
    function reordered(list, from, to) {
        const out = list.slice()
        out.splice(to, 0, out.splice(from, 1)[0])
        return out
    }

    // Where the item at i lands when the item at from goes to place to
    function movedIndex(i, from, to) {
        if (i === from)
            return to
        if (from < to && i > from && i <= to)
            return i - 1
        if (to < from && i >= to && i < from)
            return i + 1
        return i
    }

    // The box the cell at from takes at each place it could be dropped
    function dropBoxes(cells, from, columns) {
        return cells.map((_, to) => place(reordered(cells, from, to), columns)[to])
    }

    // The index of the point nearest (x, y), the lowest on a tie, -1 for
    // none. It reads only the finger, so a place can't flip back and forth
    function nearest(points, x, y) {
        let best = -1, bestDistance = Infinity
        points.forEach((p, i) => {
            const d = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y)
            if (d < bestDistance) {
                best = i
                bestDistance = d
            }
        })
        return best
    }

    // ---- Writes: each returns a new document, or an unchanged copy

    function canAddPage(doc) {
        return documentProblem(doc) === "" && doc.pages.length < maxPages
    }
    function canRemovePage(doc) {
        return documentProblem(doc) === "" && doc.pages.length > 1
    }
    function canAddCell(doc, page) {
        return documentProblem(doc) === "" && hasPage(doc, page)
               && doc.pages[page].cells.length < maxCells
    }

    function addCell(doc, page, cell) {
        return write(doc, d => {
            if (!hasPage(d, page) || !plain(cell))
                return false
            d.pages[page].cells.push(cell)
            return true
        })
    }

    function removeCell(doc, page, index) {
        return write(doc, d => {
            if (!hasCell(d, page, index))
                return false
            d.pages[page].cells.splice(index, 1)
            return true
        })
    }

    // The cell at from goes to place to (0 the start, the count less one
    // the end): earlier, later, to the start and to the end are each one
    function moveCell(doc, page, from, to) {
        return write(doc, d => {
            if (!hasCell(d, page, from) || !hasCell(d, page, to))
                return false
            const cells = d.pages[page].cells
            cells.splice(to, 0, cells.splice(from, 1)[0])
            return true
        })
    }

    // The cell at index as a copy of its own, null when there's none
    function copyCell(doc, page, index) {
        return documentProblem(doc) === "" && hasCell(doc, page, index)
               ? copy(doc.pages[page].cells[index]) : null
    }

    // A copy of cell, after place index (-1 for the page's start); a full
    // page refuses it, as every write refuses a 25th cell
    function pasteCell(doc, page, index, cell) {
        return write(doc, d => {
            if (!hasPage(d, page) || !plain(cell) || !Number.isInteger(index) || index < -1
                    || index >= d.pages[page].cells.length)
                return false
            d.pages[page].cells.splice(index + 1, 0, cell)
            return true
        })
    }

    // The span, rounded and clamped to 1 to 12 columns and 1 to 48 rows
    function resizeCell(doc, page, index, w, h) {
        return write(doc, d => {
            if (!hasCell(d, page, index) || typeof w !== "number" || typeof h !== "number"
                    || !isFinite(w) || !isFinite(h))
                return false
            const c = d.pages[page].cells[index]
            c.w = clamp(Math.round(w), 1, maxColumns)
            c.h = clamp(Math.round(h), 1, maxRows)
            return true
        })
    }

    // The phone's height control: the next multiple of the scale up or
    // down, and no lower than one scale's rows (a stored odd h stays until
    // a step moves it)
    function stepRows(h, up) {
        if (up)
            return Math.min(maxRows, (Math.floor(h / phoneSpanScale) + 1) * phoneSpanScale)
        const down = (Math.ceil(h / phoneSpanScale) - 1) * phoneSpanScale
        return down >= phoneSpanScale ? down : h
    }

    // label, refs and unit, merged; null removes label or unit, and a key
    // holding undefined is left out of the change. The kind
    // and the span aren't changed here (a new kind is a new cell)
    function setCell(doc, page, index, fields) {
        return write(doc, d => {
            if (!hasCell(d, page, index) || !isObject(fields) || !plain(fields))
                return false
            const c = d.pages[page].cells[index]
            for (const k in fields) {
                if (k !== "label" && k !== "refs" && k !== "unit")
                    return false
                if (fields[k] === undefined)
                    continue
                if (fields[k] === null)
                    delete c[k]
                else
                    c[k] = fields[k]
            }
            return true
        })
    }

    // Merged into the cell's options; null removes a key, and a key
    // holding undefined is left out of the change. Keys this version
    // doesn't know are kept, as read
    function setOptions(doc, page, index, changes) {
        return write(doc, d => {
            if (!hasCell(d, page, index) || !isObject(changes) || !plain(changes))
                return false
            const c = d.pages[page].cells[index]
            const o = c.options === undefined ? {} : c.options
            if (!isObject(o))
                return false
            for (const k in changes) {
                if (changes[k] === undefined)
                    continue
                if (changes[k] === null)
                    delete o[k]
                else
                    o[k] = changes[k]
            }
            c.options = o
            return true
        })
    }

    function addPage(doc, name) {
        return write(doc, d => {
            if (pageNameProblem(name) !== "")
                return false
            d.pages.push({ name: name.trim(), cells: [] })
            return true
        })
    }

    function renamePage(doc, page, name) {
        return write(doc, d => {
            if (!hasPage(d, page) || pageNameProblem(name) !== "")
                return false
            d.pages[page].name = name.trim()
            return true
        })
    }

    function movePage(doc, from, to) {
        return write(doc, d => {
            if (!hasPage(d, from) || !hasPage(d, to))
                return false
            d.pages.splice(to, 0, d.pages.splice(from, 1)[0])
            return true
        })
    }

    // Never the last page: a form holds 1 to 10
    function removePage(doc, page) {
        return write(doc, d => {
            if (!hasPage(d, page))
                return false
            d.pages.splice(page, 1)
            return true
        })
    }

    // ---- Helpers

    // change(copy) edits a copy and says whether it applies. The result is
    // the copy as stored text reads it back (a key holding undefined left
    // out, nothing shared with the caller's arguments), kept only when the
    // change applies and that reads back as it is
    function write(doc, change) {
        if (documentProblem(doc) !== "")
            return copy(doc)
        const d = copy(doc)
        if (!change(d))
            return copy(doc)
        const out = copy(d)
        return documentProblem(out) === "" ? out : copy(doc)
    }

    // Whether a value a write is given is one stored text holds as it is:
    // null, a boolean, a finite number, text, or a list or a plain object
    // of those (an object's key holding undefined is left out). A
    // function, NaN, a list holding undefined or an object of a class (a
    // Date, which stored text holds as a string) isn't, and its write is
    // refused
    function plain(v) {
        if (v === null || typeof v === "boolean" || typeof v === "string")
            return true
        if (typeof v === "number")
            return isFinite(v)
        if (Array.isArray(v))
            return v.every(e => plain(e))
        if (isObject(v) && Object.getPrototypeOf(v) === Object.prototype)
            return Object.keys(v).every(k => v[k] === undefined || plain(v[k]))
        return false
    }

    function copy(v) {
        return v === undefined ? undefined : JSON.parse(JSON.stringify(v))
    }

    function hasPage(doc, page) {
        return Number.isInteger(page) && page >= 0 && page < doc.pages.length
    }

    function hasCell(doc, page, index) {
        return hasPage(doc, page) && Number.isInteger(index) && index >= 0
               && index < doc.pages[page].cells.length
    }

    function isObject(v) {
        return v !== null && typeof v === "object" && !Array.isArray(v)
    }

    function clamp(v, lo, hi) {
        return Math.min(hi, Math.max(lo, v))
    }

    // A label's length as a reader counts it: a character outside the
    // BMP (an emoji) is one, not its two UTF-16 units
    function codePoints(s) {
        return s.replace(/[\uD800-\uDBFF][\uDC00-\uDFFF]/g, "_").length
    }

    // The text's size as stored, in UTF-8 bytes (a lone surrogate as the
    // three of its replacement character)
    function utf8Bytes(s) {
        let n = 0
        for (let i = 0; i < s.length; ++i) {
            const u = s.charCodeAt(i)
            if (u < 0x80) {
                n += 1
            } else if (u < 0x800) {
                n += 2
            } else if (u >= 0xD800 && u <= 0xDBFF && i + 1 < s.length
                       && s.charCodeAt(i + 1) >= 0xDC00 && s.charCodeAt(i + 1) <= 0xDFFF) {
                n += 4
                ++i
            } else {
                n += 3
            }
        }
        return n
    }
}
