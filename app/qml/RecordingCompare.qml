// File/layer: app/qml/RecordingCompare.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// Compare, alignment and stretch jumping for the Recordings page, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    // The compared recording's values at axis time t, by the first's
    // signal index, each signal's pair
    function compareValuesAt(t) {
        const out = {}
        if (!part.host.compareAligned || isNaN(t) || t - part.host.compareOffsetMs < 0)
            return out
        const mine = part.host.recording.signalList.filter(s => ComparePairs.pairOf(part.host.comparePairs, s.index) !== undefined)
        const got = part.host.compare.valuesAt(mine.map(s => part.host.comparePairs[s.index]), t - part.host.compareOffsetMs)
        for (let k = 0; k < got.length; ++k)
            out[mine[k].index] = got[k]
        return out
    }

    function compareWith(row) {
        part.host.compareOpen = true
        part.host.compareTitle = row.title
        part.host.compareAligned = false
        part.host.compareStatus = qsTr("Opening %1…").arg(row.title)
        part.host.compare.open(row.folder)
    }
    function stopComparing() {
        part.host.compareOpen = false
        part.host.compare.close()
        part.host.compareTitle = ""
        part.host.compareStatus = ""
        part.host.compareAligned = false
        part.host.compareReadout = ({})
        part.host.differenceShown = false
        part.host.seriesPart.forgetDifference()
        if (part.host.recording.state === RecordingView.Open)
            part.host.cursorsPart.showAll()
    }

    // The first recording's markers' labels, each once, and its number
    // signals: what the alignment can be on
    function markerLabels() {
        const out = []
        for (const m of part.host.recording.markers) {
            if (out.indexOf(m.label) < 0)
                out.push(m.label)
        }
        return out
    }
    // The alignment's signal, by its id, with its name for another decode
    function chooseAlignSignal(s) {
        part.host.alignSignal = s ? part.host.panesPart.idOf(s) : ""
        part.host.alignSignalName = s ? s.name : ""
    }
    // Asks each recording for its pulls, once: the answers align again
    function askPulls() {
        if (part.host.pullsHere === null && part.host.stretchAsk === 0)
            part.askStretches()
        if (part.host.pullsThere === null && part.host.pullsThereAsk === 0)
            part.host.pullsThereAsk = part.host.compare.stretches(true, false, part.forcedRules())
    }
    // One ask for both kinds: only the newest ask per view is answered, so
    // the Jump row, the bands and the alignment share it
    function askStretches() {
        if (part.host.recording.state !== RecordingView.Open)
            return
        part.host.rulesProblem = ""
        part.host.stretchUnits = Bridge.unitSystem
        let used = part.host.stretchRules
        let request = part.host.recording.stretches(true, true, used)
        if (request === 0 && part.host.recording.stretchesProblem !== "") {
            // A stored level that is none today: the defaults, with the
            // words under the panel's fields, never a refused ask
            part.host.rulesProblem = part.host.recording.stretchesProblem
            used = ({})
            request = part.host.recording.stretches(true, true, used)
        }
        part.host.stretchAsk = request
        if (request === 0) {
            part.host.stretchFailed = true
            part.host.pullsHereFailed = true
            part.host.pullsHere = []
        } else {
            part.host.askedRules = used
        }
    }
    // The levels in force for the compared recording's ask too, so a pull is
    // counted by one rule in both
    function forcedRules() {
        return part.host.askedRules
    }
    // Asks again with the levels in force, here and there: a change of unit
    // system reads the same typed numbers in the other unit
    function reaskStretches() {
        part.host.stretchList = null
        part.host.stretchFailed = false
        part.host.pullsHere = null
        part.host.pullsHereFailed = false
        part.host.jumpIndex = -1
        part.askStretches()
        if (part.host.compare.state === RecordingView.Open) {
            part.host.pullsThere = null
            part.host.alignPullThere = 0
            part.host.pullsThereAsk = part.host.compare.stretches(true, false, part.host.askedRules)
        }
    }
    // Asks again with the panel's levels. A refusal leaves the earlier answer
    // standing and its words under the fields
    function applyStretchRules(rules) {
        const request = part.host.recording.stretches(true, true, rules)
        if (request === 0) {
            part.host.rulesProblem = part.host.recording.stretchesProblem
            if (part.host.rulesProblem === "")
                part.host.stretchFailed = true
            return
        }
        part.host.rulesProblem = ""
        part.host.askedRules = rules
        part.host.stretchUnits = Bridge.unitSystem
        part.host.viewerSettings.stretchRules = JSON.stringify(rules)
        part.host.stretchAsk = request
        part.host.stretchList = null
        part.host.stretchFailed = false
        part.host.pullsHere = null
        part.host.jumpIndex = -1
        if (part.host.compare.state === RecordingView.Open) {
            part.host.pullsThere = null
            part.host.alignPullThere = 0
            part.host.pullsThereAsk = part.host.compare.stretches(true, false, rules)
        }
        // On a phone the panel stood where the chart is: back to it
        if (Theme.compact)
            part.host.stretchesOpen = false
    }
    function stretchesOf(kind) {
        return part.host.stretchList === null ? [] : part.host.stretchList.filter(s => s.kind === kind)
    }
    // Where a jump goes on from: the cursor, else the view's start
    function jumpBase() {
        return part.host.hasCursor ? part.host.cursorMs : part.host.viewT0
    }
    // The place in a kind's list of the first stretch after the base, or
    // of the last before it; -1 for none
    function nextStretch(kind) {
        const at = part.jumpBase()
        return part.stretchesOf(kind).findIndex(s => s.fromMs > at + 0.5)
    }
    function previousStretch(kind) {
        const at = part.jumpBase()
        const list = part.stretchesOf(kind)
        for (let i = list.length - 1; i >= 0; --i) {
            if (list[i].fromMs < at - 0.5)
                return i
        }
        return -1
    }
    // The stretch in view with a margin, the cursor at its start
    function jump(kind, direction) {
        const i = direction > 0 ? part.nextStretch(kind) : part.previousStretch(kind)
        if (i < 0)
            return
        const s = part.stretchesOf(kind)[i]
        const margin = Math.max(part.host.minSpanMs, (s.toMs - s.fromMs) / 4)
        part.host.cursorsPart.setView(s.fromMs - margin, s.toMs + margin)
        part.host.cursorsPart.setCursor(s.fromMs)
        part.host.jumpKind = kind
        part.host.jumpIndex = i
    }
    // A recording's pulls as the choices' rows: its number, start and length
    function pullRows(pulls, here) {
        return (pulls || []).map((p, i) =>
            (here ? qsTr("Pull %1 here · %2 · %3") : qsTr("Pull %1 there · %2 · %3"))
                .arg(i + 1).arg(part.host.formatPart.timeText(p.fromMs, true)).arg(part.host.formatPart.lengthText(p.toMs - p.fromMs)))
    }
    function setAlignOn(on) {
        if (on === "marker" && part.host.alignLabel === "")
            part.host.alignLabel = part.markerLabels()[0] || ""
        if ((on === "rise" || on === "fall") && part.host.alignSignal === "")
            part.chooseAlignSignal(ComparePairs.numbers(part.host.recording.signalList)[0])
        part.host.alignOn = on
    }
    // A number as typed, in the locale or plain; NaN for none
    function numberOf(text) {
        if (text.trim() === "")
            return NaN
        try {
            return Number.fromLocaleString(Qt.locale(), text.trim())
        } catch (e) {
            const v = Number(text.trim())
            return isFinite(v) ? v : NaN
        }
    }

    // Lines the compared recording up on the first: its offset, whether
    // it's drawn, and the status saying so
    function align() {
        part.host.compareAligned = false
        part.host.compareStatus = part.host.compare.state === RecordingView.Failed
                        ? qsTr("%1 can't be opened: %2").arg(part.host.compareTitle).arg(part.host.compare.error) : ""
        if (part.host.compare.state === RecordingView.Open && part.host.recording.state === RecordingView.Open)
            part.alignOpen()
        if (part.host.recording.state !== RecordingView.Open)
            return
        part.host.cursorsPart.showAll()
        if (part.host.hasCursor)
            part.host.cursorsPart.setCursor(part.host.cursorMs)
    }
    function alignOpen() {
        let here = 0
        let there = 0
        let what = qsTr("their starts")
        if (part.host.alignOn === "utc") {
            what = qsTr("their UTC starts")
            here = part.host.recording.info.utcStartMs > 0 ? part.host.recording.info.utcStartMs : -1
            there = part.host.compare.info.utcStartMs > 0 ? part.host.compare.info.utcStartMs : -1
        } else if (part.host.alignOn === "marker") {
            what = qsTr("the marker %1").arg(part.host.alignLabel)
            here = part.host.alignLabel !== "" ? part.host.recording.findMarker(part.host.alignLabel) : -1
            there = part.host.alignLabel !== "" ? part.host.compare.findMarker(part.host.alignLabel) : -1
        } else if (part.host.alignOn === "rise" || part.host.alignOn === "fall") {
            const s = part.host.recording.signalList.find(x => part.host.panesPart.idOf(x) === part.host.alignSignal)
            if (!s || isNaN(part.host.alignLevel)) {
                part.host.compareStatus = qsTr("Dashed: %1. Choose a signal and type a level to align on").arg(part.host.compareTitle)
                return
            }
            const verb = part.host.alignOn === "rise" ? qsTr("rising to %1") : qsTr("falling to %1")
            what = qsTr("%1 %2").arg(s.name).arg(verb.arg(part.host.formatPart.valueText(part.host.alignLevel, s.decimals, s.unit)))
            const j = ComparePairs.pairOf(part.host.comparePairs, s.index)
            here = part.host.recording.findThreshold(s.index, part.host.alignLevel, part.host.alignHysteresis, part.host.alignOn === "rise")
            there = j !== undefined ? part.host.compare.findThreshold(j, part.host.alignLevel, part.host.alignHysteresis, part.host.alignOn === "rise") : -1
        } else if (part.host.alignOn === "engine") {
            what = qsTr("their engine starts")
            here = part.host.recording.findEngineStart()
            there = part.host.compare.findEngineStart()
        } else if (part.host.alignOn === "pull") {
            if (part.host.pullsHere === null || part.host.pullsThere === null) {
                part.askPulls()
                part.host.compareStatus = qsTr("Dashed: %1. Finding each recording's pulls…").arg(part.host.compareTitle)
                return
            }
            if (part.host.pullsHereFailed || part.host.pullsThereFailed) {
                part.host.compareStatus = (part.host.pullsHereFailed && part.host.pullsThereFailed
                                 ? qsTr("%1 isn't drawn: neither recording's pulls could be read")
                                 : part.host.pullsHereFailed ? qsTr("%1 isn't drawn: this recording's pulls couldn't be read")
                                 : qsTr("%1 isn't drawn: its pulls couldn't be read")).arg(part.host.compareTitle)
                return
            }
            what = qsTr("pull %1 here and pull %2 there").arg(part.host.alignPullHere + 1).arg(part.host.alignPullThere + 1)
            here = part.host.alignPullHere < part.host.pullsHere.length ? part.host.pullsHere[part.host.alignPullHere].fromMs : -1
            there = part.host.alignPullThere < part.host.pullsThere.length ? part.host.pullsThere[part.host.alignPullThere].fromMs : -1
        }
        if (here < 0 || there < 0) {
            part.host.compareStatus = part.host.alignOn === "utc"
                ? (here < 0 && there < 0 ? qsTr("%1 isn't drawn: neither has a UTC start").arg(part.host.compareTitle)
                   : here < 0 ? qsTr("%1 isn't drawn: this recording has no UTC start").arg(part.host.compareTitle)
                   : qsTr("%1 isn't drawn: it has no UTC start").arg(part.host.compareTitle))
                : part.host.alignOn === "engine"
                ? (here < 0 && there < 0
                   ? qsTr("%1 isn't drawn: neither has an engine start (RPM reaching 500 after being below 100)")
                   : here < 0 ? qsTr("%1 isn't drawn: this recording has no engine start (RPM reaching 500 after being below 100)")
                   : qsTr("%1 isn't drawn: it has no engine start (RPM reaching 500 after being below 100)"))
                  .arg(part.host.compareTitle)
                : part.host.alignOn === "pull"
                ? (here < 0 && there < 0
                   ? qsTr("%1 isn't drawn: neither recording has a pull (full throttle for a second while it accelerates)")
                   : here < 0 ? qsTr("%1 isn't drawn: this recording has no pull (full throttle for a second while it accelerates)")
                   : qsTr("%1 isn't drawn: it has no pull (full throttle for a second while it accelerates)"))
                  .arg(part.host.compareTitle)
                : here < 0 && there < 0 ? qsTr("%1 isn't drawn: neither has %2").arg(part.host.compareTitle).arg(what)
                : here < 0 ? qsTr("%1 isn't drawn: this recording has no %2").arg(part.host.compareTitle).arg(what)
                : qsTr("%1 isn't drawn: it has no %2").arg(part.host.compareTitle).arg(what)
            return
        }
        part.host.compareOffsetMs = part.host.alignOn === "utc" ? there - here : here - there
        part.host.compareAligned = true
        part.host.compareStatus = part.host.alignOn === "start" ? qsTr("Dashed: %1, aligned on %2").arg(part.host.compareTitle).arg(what)
            : part.host.alignOn === "utc" ? qsTr("Dashed: %1, aligned on %2, %3 apart").arg(part.host.compareTitle).arg(what)
                                    .arg(part.host.formatPart.timeText(Math.abs(part.host.compareOffsetMs), true))
            : qsTr("Dashed: %1, aligned on %2, at %3 here and %4 there").arg(part.host.compareTitle).arg(what)
                  .arg(part.host.formatPart.timeText(here, true)).arg(part.host.formatPart.timeText(there, true))
    }
}
