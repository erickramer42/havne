// File/layer: app/qml/VehicleBook.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.vehiclebook, QmlTest.settings (test_qml)

pragma Singleton
import QtQml

// The vehicles (ADR-0027) as rules on one document, format 2 (ADR-0064),
// { format, salt, picked, list: [{ id, label, vinKey, uri, plotted,
// unplotted, picks, own, vinMatches, baseline, unmigrated }], unmigrated }:
// plotted and unplotted are the rows ticked past Live's default and
// unticked from it (ADR-0039 section 8), each by its signal's key
// (ADR-0065 section 9); a list kept by name is read as it stands and
// written by key at the next tick (SignalListModel::setPlotChoices).
// picks are the plugin variants picked for the vehicle, by their profiles
// (plugin.<id>.<variant>), in the order picked; the plugins themselves are
// the app's, not a vehicle's. own is the profile of the variant the
// vehicle's form writes, "" for none.
// vinMatches are the variants' profiles the car's VIN matched when it was
// last read, so the list marks them with no session. Never the VIN
// (ADR-0027 section 3).
// baseline is the folder of the vehicle's baseline recording (ADR-0049's
// appendix of 2026-11-06), as the Recordings page names it, "" for none.
// unmigrated, on an entry or on the book, are the signal files the move
// into the plugin store left behind, each { sha256, name, words } with
// kept false when its bytes are only in the earlier version's folder; the
// key is there only while the list holds any.
// A key this version doesn't know is carried through every change, on an
// entry and on the book.
// every function takes the document and returns a new one, never
// changing what it was given, so SettingsPage stores what comes back
// and the rules test without a page.
QtObject {
    readonly property int maxLabel: 40
    readonly property int format: 2

    readonly property var bookKeys: ["format", "salt", "picked", "list", "unmigrated"]
    readonly property var entryKeys: ["id", "label", "vinKey", "uri", "plotted", "unplotted",
                                      "picks", "own", "vinMatches", "baseline", "unmigrated"]

    function empty() {
        return { format: format, salt: "", picked: "", list: [] }
    }

    function isAdapterUri(uri) {
        const u = String(uri).toLowerCase()
        return u.startsWith("j2534://") || u.startsWith("elm327://")
    }

    function isString(v) {
        return typeof v === "string"
    }

    function isStrings(v) {
        return Array.isArray(v) && v.every(isString)
    }

    // A SHA-256 as the stores name a file: 64 lower-case hex digits
    function isSha(s) {
        return typeof s === "string" && /^[0-9a-f]{64}$/.test(s)
    }

    // The keys of o this version doesn't know, copied whole
    function carried(o, known) {
        const out = {}
        for (const key of Object.keys(o)) {
            if (known.indexOf(key) < 0)
                out[key] = JSON.parse(JSON.stringify(o[key]))
        }
        return out
    }

    // The records of a list of files that didn't move; what isn't one is
    // left out
    function records(list) {
        if (!Array.isArray(list))
            return []
        return list.filter(r => r !== null && typeof r === "object" && isSha(r.sha256)
                                && isString(r.name) && isString(r.words))
                   .map(r => r.kept === false
                             ? { sha256: r.sha256, name: r.name, words: r.words, kept: false }
                             : { sha256: r.sha256, name: r.name, words: r.words })
    }

    // Whether v is an entry: an id and a label, and each other field it
    // has of its own type
    function entryOk(v) {
        const optional = (value, ok) => value === undefined || ok(value)
        return v !== null && typeof v === "object" && !Array.isArray(v)
            && isString(v.id) && v.id !== "" && isString(v.label) && v.label.trim() !== ""
            && optional(v.vinKey, isString) && optional(v.uri, isString)
            && optional(v.plotted, isStrings) && optional(v.unplotted, isStrings)
            && optional(v.picks, isStrings) && optional(v.own, isString)
            && optional(v.vinMatches, isStrings) && optional(v.baseline, isString)
    }

    // The stored text. Each entry is judged by itself: one that isn't an
    // entry, or whose id an earlier one has, is left out and the rest
    // stay, and a list that is missing or isn't one reads as none, the
    // book's other fields kept. Text that isn't a book at all gives an
    // empty one. A book of a later format, or of one that isn't a number,
    // reads as empty with that format (isLater), so the page never
    // writes over it
    function parse(text) {
        let d
        try {
            d = JSON.parse(text)
        } catch (e) {
            return empty()
        }
        if (d === null || typeof d !== "object" || Array.isArray(d))
            return empty()
        if (d.format !== undefined && (typeof d.format !== "number" || d.format > format)) {
            const later = empty()
            later.format = d.format
            return later
        }
        const doc = carried(d, bookKeys)
        doc.format = format
        doc.salt = isString(d.salt) ? d.salt : ""
        doc.list = []
        const ids = {}
        for (const v of Array.isArray(d.list) ? d.list : []) {
            if (!entryOk(v) || ids[v.id])
                continue
            ids[v.id] = true
            doc.list.push(entry(v))
        }
        doc.picked = isString(d.picked) && ids[d.picked] ? d.picked : ""
        const left = records(d.unmigrated)
        if (left.length > 0)
            doc.unmigrated = left
        return doc
    }

    function isLater(doc) {
        return typeof doc.format !== "number" || doc.format > format
    }

    function serialize(doc) {
        return JSON.stringify(doc)
    }

    // Every field a stored entry has, so no change drops one
    function entry(v) {
        const e = carried(v, entryKeys)
        e.id = v.id
        e.label = v.label
        e.vinKey = v.vinKey === undefined ? "" : v.vinKey
        e.uri = v.uri === undefined ? "" : v.uri
        e.plotted = v.plotted === undefined ? [] : v.plotted.slice()
        e.unplotted = v.unplotted === undefined ? [] : v.unplotted.slice()
        e.picks = v.picks === undefined ? [] : v.picks.slice()
        e.own = v.own === undefined ? "" : v.own
        e.vinMatches = v.vinMatches === undefined ? [] : v.vinMatches.slice()
        e.baseline = v.baseline === undefined ? "" : v.baseline
        const left = records(v.unmigrated)
        if (left.length > 0)
            e.unmigrated = left
        return e
    }

    function copy(doc) {
        const d = carried(doc, bookKeys)
        d.format = doc.format
        d.salt = doc.salt
        d.picked = doc.picked
        d.list = doc.list.map(v => entry(v))
        const left = records(doc.unmigrated)
        if (left.length > 0)
            d.unmigrated = left
        return d
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
        d.list.push(entry({ id: id, label: String(label).trim() }))
        d.picked = id
        return d
    }

    // The picked vehicle's connection, plotted signals and picks, not its
    // own variant (the form writes one vehicle's), its VIN key and
    // matches, its baseline (a recording of that car) or what didn't move
    function duplicate(doc, label) {
        const from = picked(doc)
        if (from === null)
            return doc
        const d = add(doc, label)
        const v = picked(d)
        v.uri = from.uri
        v.plotted = from.plotted.slice()
        v.unplotted = from.unplotted.slice()
        v.picks = from.picks.slice()
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

    // The matches came from the VIN, so they go with its key
    function forgetVin(doc) {
        return change(doc, v => {
            v.vinKey = ""
            v.vinMatches = []
        })
    }

    // The picked vehicle out of the list. Its files that didn't move
    // become the book's own: one leaves only through Save or Remove
    function forget(doc) {
        const d = copy(doc)
        const gone = d.list.find(v => v.id === d.picked)
        d.list = d.list.filter(v => v.id !== d.picked)
        d.picked = ""
        keepRecords(d, gone === undefined ? [] : records(gone.unmigrated))
        return d
    }

    // Every vehicle and the salt, as before the first was saved; the
    // files that didn't move, the book's own and each vehicle's, stay
    // the book's until each is removed
    function forgetAll(doc) {
        const d = empty()
        keepRecords(d, doc.list.reduce((all, v) => all.concat(records(v.unmigrated)),
                                       records(doc.unmigrated)))
        return d
    }

    // more added to the book's own files that didn't move, each once
    function keepRecords(d, more) {
        const all = records(d.unmigrated)
        for (const r of more) {
            if (!all.some(a => a.sha256 === r.sha256))
                all.push(r)
        }
        if (all.length > 0)
            d.unmigrated = all
    }

    // The picked vehicle's picks, in their order
    function setPicks(doc, picks) {
        return change(doc, v => v.picks = picks.map(p => String(p)))
    }

    // The profile of the variant the picked vehicle's form writes ("" for
    // none)
    function setOwn(doc, profile) {
        return change(doc, v => v.own = String(profile))
    }

    // The variants the car's VIN matched, by their profiles, for the
    // picked vehicle (what the session read replaces what was kept)
    function setVinMatches(doc, profiles) {
        return change(doc, v => v.vinMatches = profiles.map(p => String(p)))
    }

    // Every vehicle's matches of one of profiles forgotten, its picks
    // kept: a plugin replaced
    function dropVinMatches(doc, profiles) {
        const d = copy(doc)
        for (const v of d.list)
            v.vinMatches = v.vinMatches.filter(p => profiles.indexOf(p) < 0)
        return d
    }

    // Every vehicle's picks, own variant and matches of one of profiles
    // undone: a plugin or a variant removed
    function dropProfiles(doc, profiles) {
        const d = dropVinMatches(doc, profiles)
        for (const v of d.list) {
            v.picks = v.picks.filter(p => profiles.indexOf(p) < 0)
            if (profiles.indexOf(v.own) >= 0)
                v.own = ""
        }
        return d
    }

    // Every vehicle's pick and own variant of one of profiles named to
    // instead (once in a vehicle's picks), its matches of them forgotten:
    // an import that took another's place
    function movePicks(doc, profiles, to) {
        const d = dropVinMatches(doc, profiles)
        for (const v of d.list) {
            const moved = v.picks.map(p => profiles.indexOf(p) >= 0 ? to : p)
            v.picks = moved.filter((p, i) => moved.indexOf(p) === i)
            if (profiles.indexOf(v.own) >= 0)
                v.own = to
        }
        return d
    }

    // Whether any vehicle picks one of profiles
    function pickedBy(doc, profiles) {
        return doc.list.some(v => v.picks.some(p => profiles.indexOf(p) >= 0))
    }

    // A file that didn't move, by its hash, out of the picked vehicle's
    // list, and out of the book's own
    function dropUnmigrated(doc, sha256) {
        return change(doc, v => {
            const left = records(v.unmigrated).filter(r => r.sha256 !== sha256)
            if (left.length > 0)
                v.unmigrated = left
            else
                delete v.unmigrated
        })
    }

    function dropLooseUnmigrated(doc, sha256) {
        const d = copy(doc)
        const left = records(d.unmigrated).filter(r => r.sha256 !== sha256)
        if (left.length > 0)
            d.unmigrated = left
        else
            delete d.unmigrated
        return d
    }

    // Whether a vehicle or the book lists a file that didn't move with
    // this hash: its kept bytes go only once none does
    function hashListed(doc, sha256) {
        return records(doc.unmigrated).some(r => r.sha256 === sha256)
            || doc.list.some(v => records(v.unmigrated).some(r => r.sha256 === sha256))
    }

    // The picked vehicle's baseline recording, by its folder ("" for none)
    function setBaseline(doc, folder) {
        return change(doc, v => v.baseline = String(folder))
    }

    function clearBaseline(doc) {
        return setBaseline(doc, "")
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
    // "switched" or "new"; label the vehicle the VIN names; previous the
    // one picked, "" for none. The pick never changes: the session reads
    // the picked vehicle's profile, so a vehicle the VIN names is offered
    // (the pick at the next connect), and a new one is added unpicked
    function recognize(doc, key, uri) {
        const before = picked(doc)
        const previous = before === null ? "" : before.label
        if (before !== null && before.vinKey === key)
            return { doc: doc, outcome: "same", label: previous, previous: previous }
        const other = doc.list.find(v => v.vinKey === key)
        if (other)
            return { doc: doc, outcome: "switched", label: other.label,
                     previous: previous }
        if (before !== null && before.vinKey === "")
            return { doc: change(doc, v => v.vinKey = key), outcome: "learned",
                     label: previous, previous: previous }
        const label = defaultNewLabel(doc)
        const d = add(doc, label)
        const v = picked(d)
        v.vinKey = key
        if (isAdapterUri(uri))
            v.uri = withoutFileProfile(uri)
        d.picked = before === null ? "" : before.id
        return { doc: d, outcome: "new", label: label, previous: previous }
    }

    // uri without a profile= that names a signal file or a plugin
    // (user.*, plugin.*): a vehicle a VIN adds picks none, so its
    // connection reads with Default, never with the last vehicle's signals
    function withoutFileProfile(uri) {
        const s = String(uri)
        const q = s.indexOf("?")
        if (q < 0)
            return s
        const kept = s.slice(q + 1).split("&").filter(p => {
            const v = p.startsWith("profile=") ? p.slice(8) : ""
            return !v.startsWith("user.") && !v.startsWith("plugin.")
        })
        return kept.length === 0 ? s.slice(0, q) : s.slice(0, q + 1) + kept.join("&")
    }

    // Whether uri's query has a profile key, with a value or not: an
    // address that names its profile is read as it is
    function namesProfile(uri) {
        const s = String(uri)
        const q = s.indexOf("?")
        return q >= 0 && s.slice(q + 1).split("&")
                          .some(p => p === "profile" || p.startsWith("profile="))
    }

    // uri reading profile: profile= added last, every other key where it
    // was; an address that names a profile already is left as it is
    function withProfile(uri, profile) {
        const s = String(uri)
        if (profile === "" || namesProfile(s))
            return s
        const q = s.indexOf("?")
        const sep = q < 0 ? "?" : s.endsWith("?") || s.endsWith("&") ? "" : "&"
        return s + sep + "profile=" + profile
    }

    // Beside the vehicles, for a recognize() that named another saved
    // vehicle: the session keeps the pick, so the section offers the
    // named one; "" for any other result
    function pickNote(r) {
        if (r.outcome !== "switched")
            return ""
        return r.previous === ""
                ? qsTr("The connected car's VIN is %1's: pick it to read its signals at the next connect").arg(r.label)
                : qsTr("The connected car's VIN is %1's, but this session reads %2: pick %1 for the next connect")
                      .arg(r.label, r.previous)
    }

    // The activity log's line for a recognize() result
    function logLine(r) {
        switch (r.outcome) {
        case "same":
            return qsTr("Vehicle: %1").arg(r.label)
        case "learned":
            return qsTr("Vehicle: %1 (recognized from now on)").arg(r.label)
        case "switched":
            return r.previous === ""
                    ? qsTr("Vehicle: %1 (recognized, not picked; pick it for the next connect)").arg(r.label)
                    : qsTr("Vehicle: %1 (recognized; this session reads %2, pick %1 for the next connect)")
                          .arg(r.label, r.previous)
        default:
            return r.previous === ""
                    ? qsTr("Vehicle: saved as %1 (not picked)").arg(r.label)
                    : qsTr("Vehicle: not %1 (its VIN differs); saved as %2, not picked for this session")
                          .arg(r.previous, r.label)
        }
    }
}
