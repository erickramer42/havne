// File/layer: app/qml/DashboardEdit.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.dashboardbook (test_qml)

// The dashboards' edits (ADR-0057): each takes a document, as
// DashboardBook describes it, and returns a new one, never changing what
// it was given, through DashboardBook.write so the result reads back as
// it is. DashboardBook knows nothing of these; they read its bounds,
// checks and helpers by name.

pragma Singleton
import QtQml

QtObject {
    // A Digits card's count with its option unset: what a value 10 % past
    // either end of the range takes at these decimals (a minus takes a
    // digit), 3 to 8, so a brief overshoot still draws; 4 without a finite range
    function digitsFor(from, to, decimals) {
        if (!isFinite(from) || !isFinite(to))
            return 4
        const past = (to - from) * 0.1
        const length = v => Number(v).toFixed(decimals).replace(".", "").length
        return DashboardBook.clamp(Math.max(length(from - past), length(to + past)), 3, 8)
    }

    // { from, to } for a new gauge: by the unit it decodes to, then the
    // field's range (finite, lo below hi), then 0 to 100. Never NaN, so a
    // layout never stores one
    function defaultRange(unit, lo, hi) {
        const byUnit = DashboardBook.unitRanges[unit]
        if (byUnit !== undefined && Object.prototype.hasOwnProperty.call(DashboardBook.unitRanges, unit))
            return { from: byUnit[0], to: byUnit[1] }
        if (typeof lo === "number" && typeof hi === "number" && isFinite(lo) && isFinite(hi)
                && lo < hi)
            return { from: lo, to: hi }
        return { from: 0, to: 100 }
    }
    // A number typed in the display unit, back in the stored unit
    // (Bridge.decodedValue's), to six decimals, which drop the
    // conversion's noise (212 deg F is 100 deg C): the zones' and the
    // alerts' one rounding
    function roundStored(decoded) {
        return Math.round(decoded * 1e6) / 1e6
    }

    // ---- The built-in sets' refs (Bridge.gaugeSignals): a page copies the
    // map once with copySets and passes the copy, never the bridge's map,
    // since each read of that makes a new JS wrapper

    function copySets(gaugeSignals) {
        const out = {}
        for (const key in gaugeSignals)
            out[key] = Array.from(gaugeSignals[key])
        return out
    }
    // The ref and its counterparts in the other built-in set, the ref first
    function counterparts(ref, sets) {
        for (const key in sets) {
            const refs = sets[key]
            if (refs.indexOf(ref) >= 0)
                return [ref].concat(refs.filter(r => r !== ref))
        }
        return [ref]
    }
    // Refs with each one's counterparts after them, their own first: a cell
    // or an alert made under one profile reads under another
    function lookupRefs(refs, sets) {
        let all = refs ?? []
        for (const ref of refs ?? [])
            all = all.concat(counterparts(ref, sets).filter(r => all.indexOf(r) < 0))
        return all
    }
    // The set's key whose refs hold the ref, "" for none
    function keyOfRef(ref, sets) {
        for (const key in sets) {
            if (sets[key].indexOf(ref) >= 0)
                return key
        }
        return ""
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
        return cells.map((_, to) => DashboardBook.place(reordered(cells, from, to), columns)[to])
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
        return DashboardBook.documentProblem(doc) === "" && doc.pages.length < DashboardBook.maxPages
    }
    function canRemovePage(doc) {
        return DashboardBook.documentProblem(doc) === "" && doc.pages.length > 1
    }
    function canAddCell(doc, page) {
        return DashboardBook.documentProblem(doc) === "" && DashboardBook.hasPage(doc, page)
               && doc.pages[page].cells.length < DashboardBook.maxCells
    }

    function addCell(doc, page, cell) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasPage(d, page) || !DashboardBook.plain(cell))
                return false
            d.pages[page].cells.push(cell)
            return true
        })
    }
    // Cells added at the page's end in one write: none of them past the
    // page's room or not plain, else the write is refused whole
    function addCells(doc, page, cells) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasPage(d, page) || !Array.isArray(cells) || !cells.every(c => DashboardBook.plain(c)))
                return false
            d.pages[page].cells = d.pages[page].cells.concat(cells)
            return true
        })
    }

    function removeCell(doc, page, index) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasCell(d, page, index))
                return false
            d.pages[page].cells.splice(index, 1)
            return true
        })
    }

    // The cell at from goes to place to (0 the start, the count less one
    // the end): earlier, later, to the start and to the end are each one
    function moveCell(doc, page, from, to) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasCell(d, page, from) || !DashboardBook.hasCell(d, page, to))
                return false
            const cells = d.pages[page].cells
            cells.splice(to, 0, cells.splice(from, 1)[0])
            return true
        })
    }

    // The cell at index as a copy of its own, null when there's none
    function copyCell(doc, page, index) {
        return DashboardBook.documentProblem(doc) === "" && DashboardBook.hasCell(doc, page, index)
               ? DashboardBook.copy(doc.pages[page].cells[index]) : null
    }

    // A copy of cell, after place index (-1 for the page's start); a full
    // page refuses it, as every write refuses a 25th cell
    function pasteCell(doc, page, index, cell) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasPage(d, page) || !DashboardBook.plain(cell) || !Number.isInteger(index) || index < -1
                    || index >= d.pages[page].cells.length)
                return false
            d.pages[page].cells.splice(index + 1, 0, cell)
            return true
        })
    }

    // The span, rounded and clamped to 1 to 12 columns and 1 to 48 rows
    function resizeCell(doc, page, index, w, h) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasCell(d, page, index) || typeof w !== "number" || typeof h !== "number"
                    || !isFinite(w) || !isFinite(h))
                return false
            const c = d.pages[page].cells[index]
            c.w = DashboardBook.clamp(Math.round(w), 1, DashboardBook.maxColumns)
            c.h = DashboardBook.clamp(Math.round(h), 1, DashboardBook.maxRows)
            return true
        })
    }

    // A height in rows as the columns it spans: the phone's row is a quarter
    // of a column, so 18 reads "4.5" and 3 reads "0.75"
    function heightInColumns(h) {
        return String(Math.round(h * 25) / 100)
    }

    // The phone's height control: the next multiple of the scale up or
    // down, and no lower than one scale's rows (a stored odd h stays until
    // a step moves it)
    function stepRows(h, up) {
        if (up)
            return Math.min(DashboardBook.maxRows, (Math.floor(h / DashboardBook.phoneSpanScale) + 1) * DashboardBook.phoneSpanScale)
        const down = (Math.ceil(h / DashboardBook.phoneSpanScale) - 1) * DashboardBook.phoneSpanScale
        return down >= DashboardBook.phoneSpanScale ? down : h
    }

    // label, refs and unit, merged; null removes label or unit, and a key
    // holding undefined is left out of the change. The kind
    // and the span aren't changed here (a new kind is a new cell)
    function setCell(doc, page, index, fields) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasCell(d, page, index) || !DashboardBook.isObject(fields) || !DashboardBook.plain(fields))
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

    // The cell drawn as another kind, everything else kept: refs, unit,
    // label, span and every option (a kind's own options stay stored for
    // the way back). A blank has no signal, so neither side is blank
    function setKind(doc, page, index, kind) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasCell(d, page, index) || !DashboardBook.knownKind(kind) || kind === "blank")
                return false
            const c = d.pages[page].cells[index]
            if (c.kind === "blank" || c.kind === kind)
                return false
            c.kind = kind
            return true
        })
    }

    // Merged into the cell's options; null removes a key, and a key
    // holding undefined is left out of the change. Keys this version
    // doesn't know are kept, as read
    function setOptions(doc, page, index, changes) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasCell(d, page, index) || !DashboardBook.isObject(changes) || !DashboardBook.plain(changes))
                return false
            const c = d.pages[page].cells[index]
            const o = c.options === undefined ? {} : c.options
            if (!DashboardBook.isObject(o))
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
        return DashboardBook.write(doc, d => {
            if (DashboardBook.pageNameProblem(name) !== "")
                return false
            d.pages.push({ name: name.trim(), cells: [] })
            return true
        })
    }

    function renamePage(doc, page, name) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasPage(d, page) || DashboardBook.pageNameProblem(name) !== "")
                return false
            d.pages[page].name = name.trim()
            return true
        })
    }

    function movePage(doc, from, to) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasPage(d, from) || !DashboardBook.hasPage(d, to))
                return false
            d.pages.splice(to, 0, d.pages.splice(from, 1)[0])
            return true
        })
    }

    // Never the last page: a form holds 1 to 10
    function removePage(doc, page) {
        return DashboardBook.write(doc, d => {
            if (!DashboardBook.hasPage(d, page))
                return false
            d.pages.splice(page, 1)
            return true
        })
    }
}
