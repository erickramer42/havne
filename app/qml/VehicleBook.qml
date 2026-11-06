// File/layer: app/qml/VehicleBook.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.vehiclebook, QmlTest.settings (test_qml)

pragma Singleton
import QtQml

// The vehicles (ADR-0027) as rules on one document,
// { salt, picked, list: [{ id, label, vinKey, uri, plotted, unplotted,
// signals, own, plugin }] }:
// plotted and unplotted are the rows ticked past Live's default and
// unticked from it (ADR-0039 section 8); a stored entry without
// plotted reads as none.
// signals are the vehicle's signal files (ADR-0051), each
// { sha256, name, profile }: the file kept by its content's SHA-256,
// its name, and the profile it registers (user.<slug>); own is the
// sha256 of the one the vehicle's form writes, "" for none, and one of
// signals. An entry stored before them has none. Two files making one
// profile are refused (profileProblem), so a profile names one file
// wherever it's chosen; the same file in two vehicles is one file.
// plugin is the profile of the plugin variant picked for the vehicle
// (plugin.<id>.<variant>, ADR-0052 section 5), "" for none; the
// plugins themselves are the app's, not a vehicle's.
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
    // entry is missing a field (dropped whole, ADR-0027 section 5)
    function parse(text) {
        let d
        try {
            d = JSON.parse(text)
        } catch (e) {
            return empty()
        }
        const isString = v => typeof v === "string"
        const isFile = f => f !== null && typeof f === "object" && isSha(f.sha256)
                         && isString(f.name) && f.name !== ""
                         && isString(f.profile) && f.profile !== ""
        if (d === null || typeof d !== "object" || Array.isArray(d)
                || !isString(d.salt) || !isString(d.picked) || !Array.isArray(d.list))
            return empty()
        const ids = {}
        for (const v of d.list) {
            if (v === null || typeof v !== "object" || !isString(v.id) || v.id === ""
                    || ids[v.id] || !isString(v.label) || v.label.trim() === ""
                    || !isString(v.vinKey) || !isString(v.uri)
                    || !Array.isArray(v.unplotted) || !v.unplotted.every(isString)
                    || (v.plotted !== undefined
                        && (!Array.isArray(v.plotted) || !v.plotted.every(isString)))
                    || (v.signals !== undefined
                        && (!Array.isArray(v.signals) || !v.signals.every(isFile)))
                    || (v.own !== undefined && !isString(v.own))
                    || (v.plugin !== undefined && !isString(v.plugin)))
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

    // A SHA-256 as the store names a file: 64 lower-case hex digits
    function isSha(s) {
        return typeof s === "string" && /^[0-9a-f]{64}$/.test(s)
    }

    // Every field a stored entry has, so no change drops one; an own
    // that names no file of the entry's is none
    function entry(v) {
        const files = v.signals === undefined ? []
                    : v.signals.map(f => ({ sha256: f.sha256, name: f.name, profile: f.profile }))
        const own = v.own !== undefined && files.some(f => f.sha256 === v.own) ? v.own : ""
        return { id: v.id, label: v.label, vinKey: v.vinKey, uri: v.uri,
                 plotted: v.plotted === undefined ? [] : v.plotted.slice(),
                 unplotted: v.unplotted.slice(), signals: files, own: own,
                 plugin: v.plugin === undefined ? "" : v.plugin }
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
        d.list.push({ id: id, label: String(label).trim(), vinKey: "", uri: "",
                      plotted: [], unplotted: [], signals: [], own: "", plugin: "" })
        d.picked = id
        return d
    }

    // The picked vehicle's connection, plotted signals and signal files
    // (the same files, so the same profiles), not its VIN key
    function duplicate(doc, label) {
        const from = picked(doc)
        if (from === null)
            return doc
        const d = add(doc, label)
        const v = picked(d)
        v.uri = from.uri
        v.plotted = from.plotted.slice()
        v.unplotted = from.unplotted.slice()
        v.signals = entry(from).signals
        v.own = from.own
        v.plugin = from.plugin
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

    // "" when file ({ sha256, name, profile }) may join the picked
    // vehicle, else why not: another file, in any vehicle, makes its
    // profile. exceptSha is a file it replaces (the form's own, saved
    // again), "" for none
    function profileProblem(doc, file, exceptSha) {
        for (const v of doc.list) {
            const other = (v.signals || []).find(f => f.profile === file.profile
                                         && f.sha256 !== file.sha256
                                         && !(v.id === doc.picked && f.sha256 === exceptSha))
            if (other)
                return qsTr("%1 is made by another file already (%2, in %3)")
                        .arg(file.profile, other.name, v.label)
        }
        return ""
    }

    // file into the picked vehicle's signal files; the document as it was
    // when profileProblem refuses it or the vehicle has it
    function addSignals(doc, file) {
        const v = picked(doc)
        if (v === null || profileProblem(doc, file, "") !== ""
                || (v.signals || []).some(f => f.sha256 === file.sha256))
            return doc
        return change(doc, e => e.signals.push({ sha256: file.sha256, name: file.name,
                                                 profile: file.profile }))
    }

    // The file by sha256 out of the picked vehicle's, own too if it was
    function removeSignals(doc, sha256) {
        return change(doc, v => {
            v.signals = v.signals.filter(f => f.sha256 !== sha256)
            if (v.own === sha256)
                v.own = ""
        })
    }

    // The picked vehicle's plugin variant, by its profile ("" for none)
    function setPlugin(doc, profile) {
        return change(doc, v => v.plugin = String(profile))
    }

    // Every vehicle's pick of one of profiles undone: a plugin removed
    function dropPlugin(doc, profiles) {
        const d = copy(doc)
        for (const v of d.list) {
            if (profiles.indexOf(v.plugin) >= 0)
                v.plugin = ""
        }
        return d
    }

    // The profile core makes of a signal file's name (signal_file.cpp's
    // profile_of): "user." and the name in lower case, every run of other
    // characters one '-', none at either end
    function profileOf(name) {
        let slug = ""
        let dash = false
        for (const c of String(name)) {
            const lower = c >= "a" && c <= "z" || c >= "0" && c <= "9"
            const upper = c >= "A" && c <= "Z"
            if (lower || upper) {
                if (dash && slug !== "")
                    slug += "-"
                dash = false
                slug += upper ? c.toLowerCase() : c
            } else {
                dash = true
            }
        }
        return slug === "" ? "" : "user." + slug
    }

    // A name for a vehicle's own file whose profile no file in any vehicle
    // makes: "<label> own", else "<label> own 2" and on. A profile is one
    // file's (profileProblem), and a label is used again once its vehicle
    // is renamed, or copied by Duplicate
    function ownName(doc, label) {
        const taken = {}
        for (const v of doc.list) {
            for (const f of (v.signals || []))
                taken[f.profile] = true
        }
        const base = qsTr("%1 own").arg(String(label).trim())
        for (let n = 1; n < 100; ++n) {
            const name = n === 1 ? base : base + " " + n
            if (!taken[profileOf(name)])
                return name
        }
        return base
    }

    // The form's file saved: file replaces the picked vehicle's own; the
    // document as it was when profileProblem refuses it
    function setOwn(doc, file) {
        const v = picked(doc)
        if (v === null || profileProblem(doc, file, v.own || "") !== "")
            return doc
        return change(doc, e => {
            e.signals = e.signals.filter(f => f.sha256 !== e.own && f.sha256 !== file.sha256)
            e.signals.push({ sha256: file.sha256, name: file.name, profile: file.profile })
            e.own = file.sha256
        })
    }

    // Every file some vehicle keeps, each once, sorted: the store keeps
    // these and may delete the rest
    function referencedFiles(doc) {
        const all = {}
        for (const v of doc.list) {
            for (const f of (v.signals || []))
                all[f.sha256] = true
        }
        return Object.keys(all).sort()
    }

    // A connect's URI, kept for an adapter session only (ADR-0027 section 1)
    function rememberUri(doc, uri) {
        if (!isAdapterUri(uri))
            return doc
        return change(doc, v => v.uri = uri)
    }

    function rememberPlotting(doc, plotted, unplotted) {
        return change(doc, v => {
            v.plotted = plotted.slice()
            v.unplotted = unplotted.slice()
        })
    }

    // ADR-0027 section 4: the VIN key settles which vehicle this is. Returns
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

    // Beside the vehicles, for a recognize() that picked another: the
    // pick is the car's, not the user's, so the section says so until
    // the user picks; "" for any other result
    function pickNote(r) {
        if (r.outcome !== "switched")
            return ""
        return r.previous === ""
                ? qsTr("The connected car's VIN picked %1").arg(r.label)
                : qsTr("The connected car's VIN picked %1 (%2 was picked)").arg(r.label, r.previous)
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
