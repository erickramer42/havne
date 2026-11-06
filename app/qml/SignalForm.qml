// File/layer: app/qml/SignalForm.qml | Qt6 Quick singleton, UI layer
// Testing: test_qml (tests/qml/tst_signalform.qml): a signal's key from its
//          name, an enum's values read and written, the form's fields
//          into a signal (each kind's keys, defaults left out) and back,
//          a vehicle's own file with a signal added, replaced by its key
//          or removed, its modules kept to the ones its signals read; the
//          page's use: tst_settings.qml. Core's loader judges the result.
//
// The vehicle's own signals (ADR-0051's form) as rules on documents: the
// form's fields are text, as typed; draft() makes them a signal as a
// signal file holds it (docs/signal-files.md), which Read once asks for
// and Save adds to the vehicle's own file. Nothing here checks a value:
// core's loader does, in its own words, so the form and a file written by
// hand meet the same rules. Every function returns a new value and
// changes nothing it was given.

pragma Singleton
import QtQml

QtObject {
    // The groups a file names (ADR-0050), "" for none (Other)
    readonly property var groups: ["", "engine", "air", "fuel", "oxygen sensors", "emissions",
                                   "vehicle", "electrical", "diagnostics"]
    readonly property var kinds: ["number", "enum", "text", "hex"]

    // A key from a name: lower case, every run of other characters one
    // "_", at most 31; "signal" for a name with none
    function keyOf(name) {
        const key = String(name).toLowerCase().replace(/[^a-z0-9]+/g, "_")
                        .replace(/^_+|_+$/g, "").substring(0, 31).replace(/_+$/, "")
        return key === "" ? "signal" : key
    }

    // "0=Park, 1=Drive" as a file's values, { "0": "Park", "1": "Drive" }:
    // pairs split by commas or new lines, each number=label. An entry with
    // no "=" is kept with an empty label, which the loader refuses by name
    function parseValues(text) {
        const values = {}
        for (const part of String(text).split(/[,\n]/)) {
            const t = part.trim()
            if (t === "")
                continue
            const at = t.indexOf("=")
            values[(at < 0 ? t : t.substring(0, at)).trim()] = at < 0 ? "" : t.substring(at + 1).trim()
        }
        return values
    }

    function valuesText(values) {
        return Object.keys(values || {}).map(k => k + "=" + values[k]).join(", ")
    }

    // The form as it opens: the engine ECU, a byte from the first
    function emptyFields() {
        return { name: "", rx: "7E8", tx: "", service: "22", number: "", byte: "0", bits: "8",
                 bit: "", order: "be", signed: false, bytes: "", scale: "", offset: "",
                 unit: "", decimals: "", group: "", kind: "number", values: "" }
    }

    // A number typed, or the text as it is, so the loader names what's wrong
    function numberOf(text) {
        const t = String(text).trim()
        const n = Number(t)
        return t !== "" && isFinite(n) ? n : t
    }

    // The fields as a signal: the keys its kind takes, a default left out
    function draft(f) {
        const s = { key: keyOf(f.name), name: String(f.name).trim(),
                    read: { service: f.service, number: String(f.number).trim().toUpperCase() },
                    field: { byte: numberOf(f.byte) } }
        if (f.kind !== "number")
            s.kind = f.kind
        if (f.kind === "text" || f.kind === "hex") {
            if (String(f.bytes).trim() !== "")
                s.field.bytes = numberOf(f.bytes)
        } else {
            s.field.bits = numberOf(f.bits)
            if (String(f.bit).trim() !== "")
                s.field.bit = numberOf(f.bit)
            if (f.order === "le")
                s.field.order = "le"
            if (f.signed && f.kind === "number")
                s.field.signed = true
        }
        if (f.kind === "number") {
            if (String(f.scale).trim() !== "")
                s.scale = numberOf(f.scale)
            if (String(f.offset).trim() !== "")
                s.offset = numberOf(f.offset)
            if (String(f.unit) !== "")
                s.unit = String(f.unit)
            if (String(f.decimals).trim() !== "")
                s.decimals = numberOf(f.decimals)
        }
        if (f.kind === "enum")
            s.values = parseValues(f.values)
        if (f.group !== "")
            s.group = f.group
        return s
    }

    // The module as a file's ecus entry, its tx only when typed
    function moduleOf(f) {
        const m = { rx: String(f.rx).trim().toUpperCase() }
        if (String(f.tx).trim() !== "")
            m.tx = String(f.tx).trim().toUpperCase()
        return m
    }

    // A new vehicle's own file, named "<label> own" or name when given
    // (VehicleBook.ownName: one no other file's profile has)
    function emptyFile(label, name) {
        return { havne_signals: 1,
                 name: name !== undefined ? String(name) : qsTr("%1 own").arg(String(label).trim()),
                 source: { name: "Written in Havne" }, ecus: [], signals: [] }
    }

    // A kept copy's text as a document, or null
    function parseFile(text) {
        try {
            const d = JSON.parse(text)
            return d !== null && typeof d === "object" && Array.isArray(d.signals)
                    && Array.isArray(d.ecus) ? d : null
        } catch (e) {
            return null
        }
    }

    function copy(file) {
        return JSON.parse(JSON.stringify(file))
    }

    // The ECUs its signals read, in their order
    function trimEcus(file) {
        const used = {}
        for (const s of file.signals)
            used[s.ecu] = true
        file.ecus = file.ecus.filter(e => used[e.id])
        return file
    }

    // file with signal on module: the module joins the ecus once ("m" and
    // its rx), and a signal of the same key is replaced in its place, or
    // the one keyed replacing (an Edit that renamed it)
    function withSignal(file, signal, module, replacing) {
        const d = copy(file)
        let ecu = d.ecus.find(e => String(e.rx).toUpperCase() === module.rx)
        if (ecu === undefined) {
            ecu = { id: "m" + module.rx.toLowerCase(), rx: module.rx }
            d.ecus.push(ecu)
        }
        if (module.tx !== undefined)
            ecu.tx = module.tx
        else
            delete ecu.tx
        const s = copy(signal)
        s.ecu = ecu.id
        const old = replacing !== undefined && replacing !== "" ? replacing : s.key
        const at = d.signals.findIndex(x => x.key === old)
        if (at < 0)
            d.signals.push(s)
        else
            d.signals[at] = s
        return trimEcus(d)
    }

    function withoutSignal(file, key) {
        const d = copy(file)
        d.signals = d.signals.filter(s => s.key !== key)
        return trimEcus(d)
    }

    // A signal of the file back into the form's fields, for Edit
    function fieldsOf(file, signal) {
        const f = emptyFields()
        const ecu = file.ecus.find(e => e.id === signal.ecu)
        const field = signal.field || {}
        const text = v => v === undefined ? "" : String(v)
        f.name = signal.name
        f.rx = ecu !== undefined ? String(ecu.rx) : ""
        f.tx = ecu !== undefined ? text(ecu.tx) : ""
        f.service = signal.read.service
        f.number = signal.read.number
        f.byte = text(field.byte)
        f.bits = text(field.bits)
        f.bit = text(field.bit)
        f.order = field.order === "le" ? "le" : "be"
        f.signed = field.signed === true
        f.bytes = text(field.bytes)
        f.scale = text(signal.scale)
        f.offset = text(signal.offset)
        f.unit = text(signal.unit)
        f.decimals = text(signal.decimals)
        f.group = signal.group || ""
        f.kind = signal.kind || "number"
        f.values = valuesText(signal.values)
        return f
    }

    // "22 D100 on 7E8": what a signal asks, and of which module
    function describe(file, signal) {
        const ecu = file.ecus.find(e => e.id === signal.ecu)
        return qsTr("%1 %2 on %3").arg(signal.read.service, signal.read.number,
                                       ecu !== undefined ? String(ecu.rx) : "?")
    }
}
