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
    // The kinds that draw a square face: the built-in layout's drawing
    // never stretches them (stretched's `keepSquare`)
    readonly property var squareKinds: ["analog", "ring"]

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
        warnBelow: { type: "number" }, dangerBelow: { type: "number" },
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
        accent: { type: "string" },
        color: { type: "string" }
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

    // Reading and writing the stored text

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

    // The layout file (ADR-0057's appendix on the layout file)
    //
    // A layout file is the form's stored document with two more
    // document-level keys, which a stored text never holds: "form" (the
    // form it was written from) and "files" (each signal file its
    // references need, { name, sha256 }). havne-cli's `layout check` is a
    // C++ port of importProblem, held equal to it by tests/data/layouts.

    readonly property int maxFiles: 32
    readonly property int maxFileName: 80
    // What the two keys may add to a stored document's own bound
    readonly property int fileKeysBytes: 16384

    // A file's text for a layout of `form`: "" when doc doesn't read back
    // as it is. `files` is [{ name, sha256 }], already narrowed to the
    // ones the layout needs (neededFiles); key order: format, form, files,
    // the document's other keys, then the pages
    function exportText(doc, form, files) {
        if (documentProblem(doc) !== "" || (form !== "phone" && form !== "desktop")
                || !Array.isArray(files) || files.length > maxFiles
                || !files.every(fileFits))
            return ""
        const listed = files.map(f => ({ name: f.name, sha256: f.sha256 }))
        // JSON escapes a control character to 6 bytes, so names that fit
        // fileFits can still overrun the keys' room
        if (utf8Bytes(serialize(listed)) > fileKeysBytes)
            return ""
        const out = { format: doc.format, form: form, files: listed }
        for (const k in doc) {
            if (!(k in out) && k !== "pages")
                out[k] = doc[k]
        }
        out.pages = doc.pages
        const text = serialize(out)
        // importProblem's bound: the document's own plus the two keys' room
        return utf8Bytes(text) > maxBytes + fileKeysBytes ? "" : text
    }

    // A file's name within maxFileName code points. A longer one gives
    // way before its last ": " (a plugin's name before its variant),
    // ending "...", so two variants of one plugin keep their own names
    function fittedFileName(name) {
        if (codePoints(name) <= maxFileName)
            return name
        const dots = "..."
        const at = name.lastIndexOf(": ")
        const keep = at >= 0 && codePoints(name.substring(at)) + dots.length < maxFileName
                   ? name.substring(at) : ""
        const head = name.substring(0, name.length - keep.length)
                         .match(/[\uD800-\uDBFF][\uDC00-\uDFFF]|[\s\S]/g)
        return head.slice(0, maxFileName - codePoints(keep) - dots.length).join("") + dots + keep
    }

    // Of a load's missing files, those a file of `vehicleFiles` is named
    // as (its name as a layout writes it, or the missing name with
    // ": <variant>" after it): the pick has changed since the layout was
    // saved, so its file is another
    function changedFiles(missing, vehicleFiles) {
        return missing.filter(m => vehicleFiles.some(
            f => fittedFileName(f.name) === m.name || f.name.startsWith(m.name + ": ")))
    }

    // The files, of `vehicleFiles` ([{ name, sha256, refs }], refs the
    // "key:" and "id:" references the file's profile gives), that a cell
    // of doc reads by a "key:" reference: [{ name, sha256 }], each name
    // within the file's bound (fittedFileName)
    function neededFiles(doc, vehicleFiles) {
        const used = {}
        for (const p of doc.pages) {
            for (const c of p.cells) {
                for (const r of c.refs)
                    used[r] = true
            }
        }
        const out = []
        for (const f of vehicleFiles) {
            if (f.refs.some(r => r.startsWith("key:") && used[r] === true))
                out.push({ name: fittedFileName(f.name), sha256: f.sha256 })
        }
        return out
    }

    // How many cells of doc read a signal by a "key:" reference that no
    // file of `vehicleFiles` gives, so a layout written now can't name the
    // plugin they need (the vehicle picks none that has the key, or none
    // is picked): a cell counts once however many keys it holds
    function unboundKeyCells(doc, vehicleFiles) {
        const given = {}
        for (const f of vehicleFiles) {
            for (const r of f.refs)
                given[r] = true
        }
        let n = 0
        for (const p of doc.pages) {
            for (const c of p.cells) {
                const keys = c.refs.filter(r => r.startsWith("key:"))
                if (keys.length > 0 && !keys.some(r => given[r] === true))
                    ++n
            }
        }
        return n
    }

    function fileFits(f) {
        return isObject(f) && typeof f.name === "string" && f.name.trim() !== ""
               && codePoints(f.name) <= maxFileName
               && typeof f.sha256 === "string" && /^[0-9a-f]{64}$/.test(f.sha256)
    }

    // Reads a layout file for `form` (the form loading it):
    // { doc, problem, dropped, form, files, missing, notes }. problem is
    // parse's ("none", "damaged", "newer", "dropped", "") or "otherform"
    // (the file's form isn't the loading one: no scaling between forms,
    // doc is the built-in layout). form is the file's own ("" for none),
    // files its list, missing those of files whose sha256 isn't among
    // `vehicleSha256s` (every file is missing when that isn't an array),
    // and notes holds "no-form" when the file named none, so it was read
    // as `form`. doc never holds the form or files keys
    function importProblem(text, form, vehicleSha256s) {
        const refused = problem => ({ doc: builtIn(form), problem: problem, form: "",
                                      files: [], missing: [], notes: [] })
        if (typeof text !== "string" || text === "")
            return refused("none")
        if (utf8Bytes(text) > maxBytes + fileKeysBytes)
            return refused("damaged")
        let d
        try {
            d = JSON.parse(text)
        } catch (e) {
            return refused("damaged")
        }
        if (!isObject(d))
            return refused("damaged")
        const declared = d.form
        const listed = d.files
        delete d.form
        delete d.files
        const r = parse(serialize(d), form)
        if (r.problem === "damaged" || r.problem === "newer")
            return refused(r.problem)
        if ((declared !== undefined && declared !== "phone" && declared !== "desktop")
                || (listed !== undefined
                    && (!Array.isArray(listed) || listed.length > maxFiles
                        || !listed.every(fileFits))))
            return refused("damaged")
        if (declared !== undefined && declared !== form)
            return { doc: builtIn(form), problem: "otherform", form: declared,
                     files: [], missing: [], notes: [] }
        const files = (listed ?? []).map(f => ({ name: f.name, sha256: f.sha256 }))
        const have = Array.isArray(vehicleSha256s) ? vehicleSha256s : []
        const out = { doc: r.doc, problem: r.problem, form: declared ?? "", files: files,
                      missing: files.filter(f => have.indexOf(f.sha256) < 0),
                      notes: declared === undefined ? ["no-form"] : [] }
        if (r.dropped !== undefined)
            out.dropped = r.dropped
        return out
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

    // The fields' rules

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

    // The built-in layout

    function builtIn(form) {
        return { format: format, pages: [{ name: qsTr("Engine"),
                                           cells: form === "phone" ? phoneCells() : desktopCells() }] }
    }

    function gaugeCell(kind, w, h, label, refs, unit, options) {
        return { kind: kind, w: w, h: h, label: label, refs: refs, unit: unit, options: options }
    }

    // The zones a new gauge on a common signal starts with, and the built-in
    // layouts use: the keys are Bridge.gaugeSignals', the values are in the
    // unit a cell stores. Generic starting points for a petrol engine, not a
    // diagnosis; a cell's own values are the user's from then on. Lambda,
    // boost, speed, MAF, throttle and load are car-specific: none
    readonly property var zoneTable: ({
        coolant: { coldBelow: 60, warnFrom: 105, dangerFrom: 115 },
        oil: { coldBelow: 70, warnFrom: 125, dangerFrom: 140 },
        iat: { warnFrom: 55, dangerFrom: 70 },
        battery: { dangerBelow: 11.8, warnBelow: 12.4, warnFrom: 14.8, dangerFrom: 15.2 },
        rpm: { dangerFrom: 6800 }
    })
    // The zone options of a gaugeSignals key: a copy, {} for none
    function zonesFor(key) {
        return Object.assign({}, zoneTable[key] ?? {})
    }
    // The shift lights' band on engine speed: the first light and the shift
    readonly property var shiftBand: ({ from: 4500, to: 6500 })
    // Every zone option, for clearing a cell's when its signal changes
    readonly property var zoneOptions: ["coldBelow", "warnFrom", "dangerFrom", "warnBelow", "dangerBelow"]

    // On 6 columns: the dial 4 wide with three readouts stacked beside it,
    // two traces across, then three readouts across. Every height is a
    // multiple of phoneSpanScale, the step the Height control takes
    function phoneCells() {
        const rpm = ["id:1001", "id:7003"]
        const degC = "\u00B0C"
        const { boost, battery, lambda, boostOptions, lambdaOptions } = watched()
        return [
            gaugeCell("shift-light", 12, 3, qsTr("Shift lights"), rpm.slice(), "RPM",
                      Object.assign({ count: 11 }, shiftBand)),
            gaugeCell("analog", 4, 18, qsTr("Engine speed"), rpm.slice(), "RPM",
                      Object.assign({ from: 0, to: 8000, majorStep: 1000, minorStep: 250,
                                     labelDivisor: 1000,
                                     caption: qsTr("\u00D71000 r/min"), peak: true,
                                     progress: true }, zonesFor("rpm"))),
            gaugeCell("digital", 2, 6, qsTr("Speed"), ["id:3001", "id:7004"], "km/h",
                      { from: 0, to: 260 }),
            gaugeCell("digital", 2, 6, qsTr("Coolant"), ["id:2001", "id:7002"], degC,
                      Object.assign({ from: 0, to: 130 }, zonesFor("coolant"))),
            gaugeCell("digital", 2, 6, qsTr("Oil temp"), ["id:9001", "id:7010"], degC,
                      Object.assign({ from: 0, to: 160 }, zonesFor("oil"))),
            gaugeCell("digital", 3, 9, qsTr("MAF"), ["id:8001", "id:7009"], "g/s",
                      { from: 0, to: 500, description: qsTr("Mass air flow") }),
            gaugeCell("digital", 3, 9, qsTr("Boost"), boost.slice(), "kPa", boostOptions),
            gaugeCell("digital", 2, 6, qsTr("IAT"), ["id:5001", "id:7005"], degC,
                      Object.assign({ from: 0, to: 120,
                                     description: qsTr("Intake air temperature") }, zonesFor("iat"))),
            gaugeCell("digital", 2, 6, qsTr("Battery"), battery.slice(), "V",
                      Object.assign({ from: 10, to: 16 }, zonesFor("battery"))),
            gaugeCell("digital", 2, 6, qsTr("Lambda"), lambda.slice(), "\u03BB",
                      lambdaOptions)
        ]
    }

    // What a driver watches beyond the dials, shared by both forms. Boost's refs
    // are the mock's and obd2's computed one (MAP minus baro). Its range runs from the
    // vacuum an idle pulls to what a turbo makes (about -15 to 25 psi), and
    // lambda's from rich to the lean a fuel cut reads. obd2's lambda is the
    // commanded one (PID 44), so the cell says so
    function watched() {
        return {
            boost: ["id:13001", "id:7011"],
            battery: ["id:14001", "id:7008"],
            lambda: ["id:10001", "id:11544"],
            boostOptions: { from: -100, to: 200 },
            lambdaOptions: { from: 0.5, to: 2, description: qsTr("Commanded equivalence ratio") }
        }
    }

    // On a grid of 5 to 8 columns the cells after the two dials fill the
    // squares beside them and then whole rows, so none is left empty
    function desktopCells() {
        const rpm = ["id:1001", "id:7003"]
        const degC = "\u00B0C"
        const { boost, battery, lambda, boostOptions, lambdaOptions } = watched()
        return [
            gaugeCell("shift-light", 12, 1, qsTr("Shift lights"), rpm.slice(), "RPM",
                      Object.assign({ count: 15 }, shiftBand)),
            gaugeCell("analog", 2, 8, qsTr("Engine speed"), rpm.slice(), "RPM",
                      Object.assign({ from: 0, to: 8000, majorStep: 1000, minorStep: 250,
                                     labelDivisor: 1000,
                                     caption: qsTr("\u00D71000 r/min"), peak: true,
                                     progress: true }, zonesFor("rpm"))),
            // No caption: the dial shows its unit
            gaugeCell("analog", 2, 8, qsTr("Vehicle speed"), ["id:3001", "id:7004"], "km/h",
                      { from: 0, to: 260, majorStep: 20, minorStep: 10, peak: true }),
            gaugeCell("ring", 1, 4, qsTr("Coolant"), ["id:2001", "id:7002"], degC,
                      Object.assign({ from: 0, to: 130 }, zonesFor("coolant"))),
            gaugeCell("ring", 1, 4, qsTr("Oil temp"), ["id:9001", "id:7010"], degC,
                      Object.assign({ from: 0, to: 160 }, zonesFor("oil"))),
            // A dial this small has no room for a label: its caption names it
            gaugeCell("analog", 1, 4, qsTr("Boost"), boost.slice(), "kPa",
                      Object.assign({ caption: qsTr("Boost") }, boostOptions)),
            gaugeCell("digital", 1, 4, qsTr("IAT"), ["id:5001", "id:7005"], degC,
                      Object.assign({ from: 0, to: 120,
                                     description: qsTr("Intake air temperature") }, zonesFor("iat"))),
            gaugeCell("digital", 1, 4, qsTr("MAF"), ["id:8001", "id:7009"], "g/s",
                      { from: 0, to: 500, description: qsTr("Mass air flow") }),
            gaugeCell("digital", 1, 4, qsTr("Lambda"), lambda.slice(), "\u03BB", lambdaOptions),
            gaugeCell("digital", 1, 4, qsTr("Battery"), battery.slice(), "V",
                      Object.assign({ from: 10, to: 16 }, zonesFor("battery")))
        ]
    }

    function reset(form) {
        return builtIn(form)
    }

    // Presets: layout files (format 2) of one page each, compiled into
    // the app as app/layouts/<form>/<key>.json and held valid by `havne-cli
    // layout check` in CI. Standard is the built-in page's cells

    readonly property var presets: [
        { key: "standard", label: qsTr("Standard") },
        { key: "temperatures", label: qsTr("Temperatures") },
        { key: "showcase", label: qsTr("Showcase") }
    ]

    function presetText(key, form) {
        if (!presets.some(p => p.key === key))
            return ""
        return Bridge.presetLayoutText(key, form)
    }

    // The preset's page ({ name, cells }), null when the file can't be read
    function presetPage(key, form) {
        const r = importProblem(presetText(key, form), form, [])
        return r.problem === "" && r.doc.pages.length === 1 ? r.doc.pages[0] : null
    }

    // A value as text with object keys in order, so two cells compare
    // whatever order their keys were written in
    function canonical(v) {
        if (Array.isArray(v))
            return "[" + v.map(canonical).join(",") + "]"
        if (isObject(v))
            return "{" + Object.keys(v).sort().filter(k => v[k] !== undefined)
                       .map(k => JSON.stringify(k) + ":" + canonical(v[k])).join(",") + "}"
        return JSON.stringify(v)
    }

    // Whether the page's cells are one preset's, whatever its name
    function matchesPreset(page, form) {
        if (!isObject(page) || !Array.isArray(page.cells))
            return false
        const mine = canonical(page.cells)
        return presets.some(p => {
            const q = presetPage(p.key, form)
            return q !== null && canonical(q.cells) === mine
        })
    }

    // Whether replacing the page's gauges with a preset's loses nothing a
    // preset doesn't give back: a page holding none, or one that is a preset
    function losesNothing(page, form) {
        return isObject(page) && Array.isArray(page.cells)
               && (page.cells.length === 0 || matchesPreset(page, form))
    }

    // pages added after place `after` (-1 the start), in order; refused
    // whole past the form's room
    function insertPages(doc, after, pages) {
        return write(doc, d => {
            if (!Number.isInteger(after) || after < -1 || after >= d.pages.length
                    || !Array.isArray(pages) || pages.length === 0 || !plain(pages))
                return false
            d.pages.splice(after + 1, 0, ...copy(pages))
            return true
        })
    }

    // The page's gauges replaced by a page's, its name kept
    function replacePage(doc, page, with_) {
        return write(doc, d => {
            if (!hasPage(d, page) || !isObject(with_) || !Array.isArray(with_.cells)
                    || !plain(with_.cells))
                return false
            d.pages[page].cells = copy(with_.cells)
            return true
        })
    }

    // Placement (section 4)

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

    // The lines of a joined panel (ADR-0057's appendix, section 3), as
    // { vertical, x, y, length } in grid squares: wherever two neighboring
    // squares belong to different cells and at least one of them is drawn.
    // A blank cell and an empty square draw nothing, so no line runs
    // between two of those. `placed` holds the cells in `boxes`' order;
    // lines that continue along a boundary are one
    function seams(placed, boxes, columns) {
        const cols = isFinite(columns) ? Math.max(1, Math.floor(columns)) : 1
        const owner = []   // owner[y][x], the box drawn there
        let rows = 0
        boxes.forEach((b, i) => {
            rows = Math.max(rows, b.y + b.h)
            if (placed[i].kind === "blank")
                return
            for (let y = b.y; y < b.y + b.h; ++y) {
                owner[y] = owner[y] || []
                for (let x = b.x; x < b.x + b.w; ++x)
                    owner[y][x] = i
            }
        })
        const at = (x, y) => owner[y] && owner[y][x] !== undefined ? owner[y][x] : -1
        const out = []
        const runs = (vertical, line, span, differs) => {
            let start = -1
            for (let i = 0; i <= span; ++i) {
                if (i < span && differs(i)) {
                    if (start < 0)
                        start = i
                } else if (start >= 0) {
                    out.push({ vertical: vertical, x: vertical ? line : start,
                               y: vertical ? start : line, length: i - start })
                    start = -1
                }
            }
        }
        for (let c = 1; c < cols; ++c)
            runs(true, c, rows, y => at(c - 1, y) !== at(c, y))
        for (let r = 1; r < rows; ++r)
            runs(false, r, cols, x => at(x, r - 1) !== at(x, r))
        return out
    }

    // A joined panel's boxes as drawn outside edit mode: each gauge runs
    // down over the empty squares under it, as far as every column it spans
    // stays empty, within the panel's rows; a gauge alone in its rows then
    // runs right to the panel's edge (the last cell alone in its row takes
    // the row), so no outlined empty square is left beside it. A cell
    // sharing its rows keeps its width: a held cell shows the true sizes,
    // and a lift that moved it sideways would land elsewhere. A blank cell
    // doesn't stretch and nothing stretches over one. For display only:
    // the layout is untouched. `columns` is the grid's; left out, the
    // boxes' right edge stands in. With `keepSquare` (the built-in layout's
    // drawing) a dial or ring keeps its own box, and the other gauges fill
    // what it leaves: down over the empty squares under them, then right
    // over the empty squares beside them, so no square is empty at any
    // column count
    function stretched(placed, boxes, columns, keepSquare) {
        const square = i => keepSquare === true && squareKinds.indexOf(placed[i].kind) >= 0
        const taken = []   // taken[y][x]
        let rows = 0
        let cols = isFinite(columns) ? Math.max(1, Math.floor(columns)) : 0
        for (const b of boxes) {
            rows = Math.max(rows, b.y + b.h)
            if (!isFinite(columns))
                cols = Math.max(cols, b.x + b.w)
            for (let y = b.y; y < b.y + b.h; ++y) {
                taken[y] = taken[y] || []
                for (let x = b.x; x < b.x + b.w; ++x)
                    taken[y][x] = true
            }
        }
        const emptyRow = (b, y) => {
            for (let x = b.x; x < b.x + b.w; ++x) {
                if (taken[y] && taken[y][x])
                    return false
            }
            return true
        }
        const down = boxes.map((b, i) => {
            let h = b.h
            if (placed[i].kind !== "blank" && !square(i)) {
                while (b.y + h < rows && emptyRow(b, b.y + h))
                    ++h
            }
            return { x: b.x, y: b.y, w: b.w, h: h }
        })
        // The squares the gauges then cover: a cell is alone in its rows
        // against what its neighbors draw, not what they were placed at
        const drawn = []   // drawn[y][x]
        for (const b of down) {
            for (let y = b.y; y < b.y + b.h; ++y) {
                drawn[y] = drawn[y] || []
                for (let x = b.x; x < b.x + b.w; ++x)
                    drawn[y][x] = true
            }
        }
        if (keepSquare === true) {
            // `drawn` grows with each widening, so a later cell sees it
            down.forEach((b, i) => {
                if (placed[i].kind === "blank" || square(i))
                    return
                for (let free = true; free && b.x + b.w < cols;) {
                    for (let y = b.y; y < b.y + b.h && free; ++y)
                        free = !(drawn[y] && drawn[y][b.x + b.w])
                    if (free) {
                        for (let y = b.y; y < b.y + b.h; ++y)
                            drawn[y][b.x + b.w] = true
                        ++b.w
                    }
                }
            })
        }
        const aloneInRows = (b) => {
            for (let y = b.y; y < b.y + b.h; ++y) {
                for (let x = 0; x < cols; ++x) {
                    if (drawn[y] && drawn[y][x] && (x < b.x || x >= b.x + b.w))
                        return false
                }
            }
            return true
        }
        return down.map((b, i) => {
            const w = placed[i].kind !== "blank" && !square(i) && aloneInRows(b)
                      ? cols - b.x : b.w
            return { x: b.x, y: b.y, w: w, h: b.h }
        })
    }

    // Helpers

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
