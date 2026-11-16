// File/layer: app/qml/ConditionBook.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.alerts (test_qml)

pragma Singleton
import QtQml

// The condition file (ADR-0063 section 5) as rules on its parsed
// document, { havne_conditions: 1, name, checks, conditions: [{ key,
// name, enabled, when, for, clear_for, alert, record, mark }] }, for the
// Alerts page and a gauge's Alert. Core's loader is the only judge of a
// file: these rules only build documents, and every one is saved
// through Bridge.saveConditions, which keeps what core takes.
//
// A key the rules don't know (notes, x-...) is kept as read, in the file
// and in a condition, and written back. Every function takes the
// document and returns a new one, never changing what it was given.
QtObject {
    readonly property int format: 1
    readonly property int maxConditions: 64
    readonly property int maxTerms: 4
    readonly property int maxRefs: 8
    // In UTF-8 bytes, as core counts ECUIO_CONDITION_NAME_MAX
    readonly property int maxName: 40
    readonly property int maxKey: 31
    // The share of a gauge's range a level's clear lies inside it
    readonly property real clearShare: 0.02
    // Core's four link words (ADR-0063 section 4), in its order
    readonly property var linkWords: ["not_ok", "timeout", "request_failed", "hard_failure"]

    function empty() {
        return { havne_conditions: format, conditions: [] }
    }
    // The document of a file's text, "" an empty one; null for text that
    // isn't a condition file's JSON (core's words say why at a save)
    function parse(text) {
        if (text === "")
            return empty()
        let doc
        try {
            doc = JSON.parse(text)
        } catch (e) {
            return null
        }
        if (doc === null || typeof doc !== "object" || Array.isArray(doc)
                || !Array.isArray(doc.conditions))
            return null
        return doc
    }
    function text(doc) {
        return JSON.stringify(doc, null, 2) + "\n"
    }
    function copy(value) {
        return JSON.parse(JSON.stringify(value))
    }
    function conditions(doc) {
        return doc !== null && Array.isArray(doc.conditions) ? doc.conditions : []
    }
    function canAdd(doc) {
        return conditions(doc).length < maxConditions
    }

    // The document with condition c at i, or appended with i -1
    function withCondition(doc, i, c) {
        const next = copy(doc)
        if (i < 0 || i >= next.conditions.length)
            next.conditions.push(copy(c))
        else
            next.conditions[i] = copy(c)
        return next
    }
    function without(doc, i) {
        const next = copy(doc)
        if (i >= 0 && i < next.conditions.length)
            next.conditions.splice(i, 1)
        return next
    }
    // Enabled is the default, so on drops the key and off writes false
    function withEnabled(doc, i, on) {
        const next = copy(doc)
        const c = next.conditions[i]
        if (c === undefined)
            return next
        if (on)
            delete c.enabled
        else
            c.enabled = false
        return next
    }

    // A key from a name: lower case, a run of anything but a-z and 0-9 one
    // "_", at most maxKey, "alert" for none; taken keys get _2, _3...
    function newKey(name, taken) {
        let base = String(name).toLowerCase().replace(/[^a-z0-9]+/g, "_")
                       .replace(/^_+|_+$/g, "").slice(0, maxKey)
        if (base === "")
            base = "alert"
        if (taken.indexOf(base) < 0)
            return base
        for (let n = 2; ; ++n) {
            const tail = "_" + n
            const key = base.slice(0, maxKey - tail.length) + tail
            if (taken.indexOf(key) < 0)
                return key
        }
    }
    function keysOf(doc) {
        return conditions(doc).map(c => String(c.key))
    }
    function namesOf(doc) {
        return conditions(doc).map(c => String(c.name))
    }

    // A name's UTF-8 bytes, as core counts them
    function nameBytes(name) {
        let n = 0
        for (const ch of String(name)) {
            const code = ch.codePointAt(0)
            n += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4
        }
        return n
    }
    // The name's first whole characters that fit in bytes
    function cutName(name, bytes) {
        let out = ""
        let n = 0
        for (const ch of String(name)) {
            n += nameBytes(ch)
            if (n > bytes)
                break
            out += ch
        }
        return out
    }
    // A name no condition in taken has (core refuses one twice): the name
    // while it's free, else " 2", " 3"... within maxName bytes
    function newName(name, taken) {
        const base = cutName(String(name).trim(), maxName)
        if (taken.indexOf(base) < 0)
            return base
        for (let n = 2; ; ++n) {
            const tail = " " + n
            const named = cutName(base, maxName - tail.length).trim() + tail
            if (taken.indexOf(named) < 0)
                return named
        }
    }

    // A condition's terms, its join ("all", "any", "" for one term)
    function termsOf(c) {
        const w = c.when ?? ({})
        if (Array.isArray(w.all))
            return w.all
        if (Array.isArray(w.any))
            return w.any
        return [w]
    }
    function joinOf(c) {
        const w = c.when ?? ({})
        return Array.isArray(w.all) ? "all" : Array.isArray(w.any) ? "any" : ""
    }
    // when for terms: one term alone, more under join. A group rebuilt
    // from was keeps was's other keys (notes, x-), each in its place
    function whenOf(terms, join, was) {
        if (terms.length === 1)
            return copy(terms[0])
        const list = join === "any" ? "any" : "all"
        const old = was ?? ({})
        const keys = Array.isArray(old.all) || Array.isArray(old.any) ? Object.keys(old) : [list]
        const w = {}
        for (const k of keys) {
            if (k === "all" || k === "any")
                w[list] = copy(terms)
            else
                w[k] = copy(old[k])
        }
        return w
    }
    // "signal", "link", "check"
    function kindOf(term) {
        return term.link !== undefined ? "link" : term.check !== undefined ? "check" : "signal"
    }
    // A signal term's comparison: above, below, equals, differs, "" none
    function comparisonOf(term) {
        for (const k of ["above", "below", "equals", "differs"]) {
            if (term[k] !== undefined)
                return k
        }
        return ""
    }
    // The term with its comparison and level set (the old one's gone);
    // equals and differs take no clear
    function withComparison(term, comparison, level) {
        const t = copy(term)
        for (const k of ["above", "below", "equals", "differs"])
            delete t[k]
        t[comparison] = level
        if (comparison !== "above" && comparison !== "below")
            delete t.clear
        return t
    }

    // A signal term on refs in unit: the term a new one starts as
    function signalTerm(refs, unit, comparison, level, clear) {
        const t = { signal: refs.slice(0, maxRefs), unit: unit }
        t[comparison] = level
        if (clear !== undefined && clear !== null)
            t.clear = clear
        return t
    }
    // A condition on one term, its key new in taken, with an alert
    function alertCondition(name, term, level, taken) {
        const shown = cutName(String(name).trim(), maxName)
        return { key: newKey(shown, taken), name: shown, when: copy(term),
                 alert: { level: level, banner: true } }
    }

    // The alert a gauge's levels give (ADR-0063 section 9): its danger
    // from, else its warning from, as above; with neither, its danger
    // below, else its warning below, as below. The clear lies clearShare
    // of the range inside the level, and nothing is held. levels holds
    // dangerFrom, warnFrom, dangerBelow, warnBelow (any may be missing)
    // in unit; null with none
    function alertFromLevels(name, refs, unit, levels, from, to, taken) {
        const span = typeof from === "number" && typeof to === "number" && to > from
                     ? to - from : 0
        const margin = span * clearShare
        const pick = [["dangerFrom", "above", "danger"], ["warnFrom", "above", "warning"],
                      ["dangerBelow", "below", "danger"], ["warnBelow", "below", "warning"]]
        for (const [option, comparison, level] of pick) {
            const at = levels[option]
            if (typeof at !== "number" || !isFinite(at))
                continue
            const clear = span > 0 ? (comparison === "above" ? at - margin : at + margin) : undefined
            return alertCondition(name, signalTerm(refs, unit, comparison, at, clear), level, taken)
        }
        return null
    }
    // The levels a signal file's gauge gives, as alertFromLevels takes
    // them: its high warning as warnFrom, its low one as warnBelow
    function gaugeLevels(gauge) {
        const g = gauge ?? ({})
        const levels = {}
        if (typeof g.warnHigh === "number")
            levels.warnFrom = g.warnHigh
        if (typeof g.warnLow === "number")
            levels.warnBelow = g.warnLow
        return levels
    }
}
