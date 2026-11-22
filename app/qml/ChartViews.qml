// File/layer: app/qml/ChartViews.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.chartlayout{,_panes} (test_qml)

// The Recordings viewer's saved pane arrangements as arithmetic on a
// parsed text (no Canvas, no Theme): ChartLayout's layout reads none of
// it.

pragma Singleton
import QtQml

QtObject {
    // The pane arrangements the viewer keeps for each recording, as the
    // settings hold them: { "<recording's folder name>": { used (ms since
    // the epoch, stamped at each write and each open of a kept one),
    // assign: { <signal id>: <pane key> } (the key, else the name an earlier
    // version kept), shares: { <pane count>:
    // [share per pane] } } }. A damaged or oversized text reads as none, a
    // damaged entry is left out and the rest kept; past viewsMax entries
    // the least recently used go first.
    readonly property int viewsMax: 100
    readonly property int viewsMaxSignals: 64
    readonly property int viewsMaxText: 262144
    readonly property int viewsMaxPanes: 8

    // Whether an assignment holding `assign` can take one more under `id`:
    // the bounds viewEntry reads back with, so a write is never one a later
    // read throws away whole
    function canKeep(assign, id) {
        return id.length > 0 && id.length <= 200
            && (assign[id] !== undefined || Object.keys(assign).length < viewsMaxSignals)
    }

    function viewEntry(e) {
        if (e === null || typeof e !== "object" || Array.isArray(e))
            return null
        const out = { used: isFinite(e.used) ? Number(e.used) : 0, assign: {}, shares: {} }
        if (e.assign !== undefined) {
            if (e.assign === null || typeof e.assign !== "object" || Array.isArray(e.assign))
                return null
            const names = Object.keys(e.assign)
            if (names.length > viewsMaxSignals)
                return null
            for (const n of names) {
                const k = e.assign[n]
                if (n.length === 0 || n.length > 200 || typeof k !== "string" || k.length === 0 || k.length > 40)
                    return null
                out.assign[n] = k
            }
        }
        if (e.shares !== undefined) {
            if (e.shares === null || typeof e.shares !== "object" || Array.isArray(e.shares))
                return null
            for (const c of Object.keys(e.shares)) {
                const n = Number(c)
                const v = e.shares[c]
                if (!Number.isInteger(n) || n < 2 || n > viewsMaxPanes || !Array.isArray(v) || v.length !== n
                        || !v.every(x => typeof x === "number" && isFinite(x) && x > 0))
                    return null
                out.shares[c] = v
            }
        }
        return out
    }

    function viewsParse(text) {
        if (typeof text !== "string" || text.length > viewsMaxText)
            return {}
        let doc
        try {
            doc = JSON.parse(text)
        } catch (e) {
            return {}
        }
        if (doc === null || typeof doc !== "object" || Array.isArray(doc))
            return {}
        const out = {}
        for (const name of Object.keys(doc)) {
            if (name.length === 0 || name.length > 300)
                continue
            const e = viewEntry(doc[name])
            if (e !== null)
                out[name] = e
        }
        return out
    }

    // The views with one recording's entry changed by patch ({ assign } and/or
    // { shares }; a field left out stays) and stamped used at `now`; an entry
    // left with nothing in it goes, and the least recently used past viewsMax
    function viewsWith(views, name, patch, now) {
        const next = Object.assign({}, views)
        const before = views[name] !== undefined ? views[name] : { assign: {}, shares: {} }
        const e = { used: now, assign: patch.assign !== undefined ? patch.assign : before.assign,
                    shares: patch.shares !== undefined ? patch.shares : before.shares }
        if (Object.keys(e.assign).length === 0 && Object.keys(e.shares).length === 0)
            delete next[name]
        else
            next[name] = e
        return viewsTrim(next)
    }

    // Stamped used at `now`, when the recording has an entry
    function viewsTouched(views, name, now) {
        if (views[name] === undefined)
            return views
        const next = Object.assign({}, views)
        next[name] = Object.assign({}, views[name], { used: now })
        return next
    }

    function viewsTrim(views) {
        const names = Object.keys(views)
        if (names.length <= viewsMax)
            return views
        names.sort((a, b) => views[b].used - views[a].used)
        const next = {}
        for (const n of names.slice(0, viewsMax))
            next[n] = views[n]
        return next
    }
}
