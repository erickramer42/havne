// File/layer: app/qml/VehicleBook.qml | Qt6 Quick, UI layer
// Testing: test_qml (tests/qml/tst_vehiclebook.qml): a stored document
//          read back or dropped whole, the label's rule and its
//          defaults, New, Duplicate, Rename, Forget the VIN, Forget and
//          Forget all, a connection and plotted signals remembered for
//          adapter sessions only, and the VIN key settling the vehicle
//          (each row of ADR-0027 §4's table, with its log line).
//          SettingsPage.qml wires it (tests/qml/tst_settings.qml).

pragma Singleton
import QtQml

// The vehicles (ADR-0027) as rules on one document,
// { salt, picked, list: [{ id, label, vinKey, uri, unplotted }] }:
// every function takes the document and returns a new one, never
// changing what it was given, so SettingsPage stores what comes back
// and the rules test without a page.
QtObject {
    readonly property int maxLabel: 40

    function empty() {
        return { salt: "", picked: "", list: [] }
    }

    function isAdapterUri(uri) {
        const u = String(uri).toLowerCase()
        return u.startsWith("j2534://") || u.startsWith("elm327://")
    }

    // The stored text, or an empty document when it doesn't parse or any
    // entry is missing a field (dropped whole, ADR-0027 §5)
    function parse(text) {
        let d
        try {
            d = JSON.parse(text)
        } catch (e) {
            return empty()
        }
        const isString = v => typeof v === "string"
        if (d === null || typeof d !== "object" || Array.isArray(d)
                || !isString(d.salt) || !isString(d.picked) || !Array.isArray(d.list))
            return empty()
        const ids = {}
        for (const v of d.list) {
            if (v === null || typeof v !== "object" || !isString(v.id) || v.id === ""
                    || ids[v.id] || !isString(v.label) || v.label.trim() === ""
                    || !isString(v.vinKey) || !isString(v.uri)
                    || !Array.isArray(v.unplotted) || !v.unplotted.every(isString))
                return empty()
            ids[v.id] = true
        }
        return {
            salt: d.salt,
            picked: ids[d.picked] ? d.picked : "",
            list: d.list.map(v => entry(v))
        }
    }

    function serialize(doc) {
        return JSON.stringify(doc)
    }

    function entry(v) {
        return { id: v.id, label: v.label, vinKey: v.vinKey, uri: v.uri,
                 unplotted: v.unplotted.slice() }
    }

    function copy(doc) {
        return { salt: doc.salt, picked: doc.picked, list: doc.list.map(v => entry(v)) }
    }

    function find(doc, id) {
        return doc.list.find(v => v.id === id) || null
    }

    function picked(doc) {
        return find(doc, doc.picked)
    }

    // "" when the label may be used (by vehicle exceptId, if renaming),
    // else why not
    function labelProblem(doc, label, exceptId) {
        const t = String(label).trim()
        if (t === "")
            return qsTr("A label needs at least one character")
        if (t.length > maxLabel)
            return qsTr("At most %1 characters").arg(maxLabel)
        const lower = t.toLowerCase()
        if (doc.list.some(v => v.id !== exceptId && v.label.trim().toLowerCase() === lower))
            return qsTr("Another vehicle is called that")
        return ""
    }

    function taken(doc, label) {
        const lower = label.toLowerCase()
        return doc.list.some(v => v.label.trim().toLowerCase() === lower)
    }

    // "Vehicle N", the lowest N no label holds
    function defaultNewLabel(doc) {
        for (let n = 1; ; ++n) {
            const label = qsTr("Vehicle %1").arg(n)
            if (!taken(doc, label))
                return label
        }
    }

    // "<label> (copy)", then "(copy 2)" and on, the label cut to fit; a
    // copy's copy continues the count rather than adding a second tail
    function defaultCopyLabel(doc, label) {
        const base = label.replace(/ \(copy( \d+)?\)$/, "")
        for (let n = 1; ; ++n) {
            const tail = n === 1 ? qsTr(" (copy)") : qsTr(" (copy %1)").arg(n)
            const candidate = base.substring(0, maxLabel - tail.length).trim() + tail
            if (!taken(doc, candidate))
                return candidate
        }
    }

    function nextId(doc) {
        let n = 0
        for (const v of doc.list) {
            const m = /^v(\d+)$/.exec(v.id)
            if (m)
                n = Math.max(n, Number(m[1]))
        }
        return "v" + (n + 1)
    }

    function pick(doc, id) {
        const d = copy(doc)
        d.picked = find(d, id) ? id : ""
        return d
    }

    function add(doc, label) {
        const d = copy(doc)
        const id = nextId(d)
        d.list.push({ id: id, label: String(label).trim(), vinKey: "", uri: "", unplotted: [] })
        d.picked = id
        return d
    }

    // The picked vehicle's connection and plotted signals, not its VIN key
    function duplicate(doc, label) {
        const from = picked(doc)
        if (from === null)
            return doc
        const d = add(doc, label)
        const v = picked(d)
        v.uri = from.uri
        v.unplotted = from.unplotted.slice()
        return d
    }

    function change(doc, apply) {
        const d = copy(doc)
        const v = picked(d)
        if (v !== null)
            apply(v)
        return d
    }

    function rename(doc, label) {
        return change(doc, v => v.label = String(label).trim())
    }

    function forgetVin(doc) {
        return change(doc, v => v.vinKey = "")
    }

    function forget(doc) {
        const d = copy(doc)
        d.list = d.list.filter(v => v.id !== d.picked)
        d.picked = ""
        return d
    }

    // Every vehicle and the salt, as before the first was saved
    function forgetAll(doc) {
        return empty()
    }

    // A connect's URI, kept for an adapter session only (ADR-0027 §1)
    function rememberUri(doc, uri) {
        if (!isAdapterUri(uri))
            return doc
        return change(doc, v => v.uri = uri)
    }

    function rememberUnplotted(doc, names) {
        return change(doc, v => v.unplotted = names.slice())
    }

    // ADR-0027 §4: the VIN key settles which vehicle this is. Returns
    // { doc, outcome, label, previous }: outcome "same", "learned",
    // "switched" or "new"; label the vehicle now picked; previous the
    // one picked before, "" for none
    function recognize(doc, key, uri) {
        const before = picked(doc)
        const previous = before === null ? "" : before.label
        if (before !== null && before.vinKey === key)
            return { doc: doc, outcome: "same", label: previous, previous: previous }
        const other = doc.list.find(v => v.vinKey === key)
        if (other)
            return { doc: pick(doc, other.id), outcome: "switched", label: other.label,
                     previous: previous }
        if (before !== null && before.vinKey === "")
            return { doc: change(doc, v => v.vinKey = key), outcome: "learned",
                     label: previous, previous: previous }
        const label = defaultNewLabel(doc)
        const d = add(doc, label)
        picked(d).vinKey = key
        if (isAdapterUri(uri))
            picked(d).uri = uri
        return { doc: d, outcome: "new", label: label, previous: previous }
    }

    // The activity log's line for a recognize() result
    function logLine(r) {
        switch (r.outcome) {
        case "same":
            return qsTr("Vehicle: %1").arg(r.label)
        case "learned":
            return qsTr("Vehicle: %1 (recognized from now on)").arg(r.label)
        case "switched":
            return r.previous === "" ? qsTr("Vehicle: %1 (picked)").arg(r.label)
                                     : qsTr("Vehicle: %1 (picked; %2 was)").arg(r.label, r.previous)
        default:
            return r.previous === ""
                    ? qsTr("Vehicle: saved as %1").arg(r.label)
                    : qsTr("Vehicle: not %1 (its VIN differs); saved as %2").arg(r.previous, r.label)
        }
    }
}
