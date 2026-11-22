// File/layer: app/qml/RecordingTexts.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings*, QmlTest.phone_* (test_qml)

import QtQuick
import App

pragma ComponentBehavior: Bound

// Display text for the Recordings page, a part of RecordingsPage

Item {
    id: part
    required property RecordingsPage host
    visible: false

    // Seconds to the ms with no trailing zeros, as core's replay takes them
    function secondsText(ms) {
        return String(Number((ms / 1000).toFixed(3)))
    }

    // A change, signed, in a signal's decimals
    function changeText(v, decimals, unit) {
        return v > 0 ? "+" + part.host.formatPart.valueText(v, decimals, unit) : part.host.formatPart.valueText(v, decimals, unit)
    }
    // A statistic of a row, or a dash with none
    function statText(v, s) {
        return isFinite(v) ? part.host.formatPart.valueText(v, s.decimals, s.unit) : qsTr("—")
    }
    function statChange(s) {
        return s && isFinite(s.change) ? part.changeText(s.change, s.decimals, s.unit) : qsTr("—")
    }
    function statRate(s) {
        return s && isFinite(s.rate)
               ? part.changeText(s.rate, s.decimals + 1, s.unit !== "" ? s.unit + "/s" : "/s") : qsTr("—")
    }
    // A value at one of the span's ends, as the rows show it
    function atText(at, s) {
        return !at || at.atMs < 0 ? qsTr("—")
               : s.isText ? at.text
               : at.label ? at.label
               : part.host.formatPart.valueText(Number(at.text), s.decimals, at.unit)
    }
    // The second line a row and a strip's cell show with a span
    function spanCaption(s) {
        const stat = part.host.spanStats[s.index]
        return s.isText ? qsTr("B %1").arg(part.atText(part.host.readoutTo[s.index], s))
                        : qsTr("B %1 · Δ %2").arg(part.atText(part.host.readoutTo[s.index], s)).arg(part.statChange(stat))
    }
    // What the row says for no pull: a recording with neither a throttle nor
    // an engine-load signal has nothing a pull is judged by, whatever the
    // levels, and the row says so. A phone's row must stay one line at any
    // font, so it keeps the plain words and the reason is the label's spoken
    // name (jumpStatusSpoken)
    readonly property string noPullWhy: part.host.recording.pullsLackSignals
        ? qsTr("No pull found (no throttle or load signal)") : qsTr("No pull found")
    readonly property string noPullText: Theme.compact ? qsTr("No pull found") : noPullWhy
    // The Jump row's line: where the last jump went while the cursor is
    // still at its start, else how many of each were found
    readonly property string jumpStatus: {
        if (part.host.stretchFailed)
            return qsTr("Its stretches couldn't be read")
        if (part.host.stretchList === null)
            return qsTr("Looking for stretches…")
        const list = part.host.jumpIndex >= 0 ? part.host.comparePart.stretchesOf(part.host.jumpKind) : []
        const at = part.host.jumpIndex >= 0 ? list[part.host.jumpIndex] : undefined
        if (at !== undefined && part.host.hasCursor && Math.abs(part.host.cursorMs - at.fromMs) < 0.5)
            return qsTr("%1 %2 of %3 · %4").arg(part.host.jumpKind === "pull" ? qsTr("Pull") : qsTr("Cruise"))
                       .arg(part.host.jumpIndex + 1).arg(list.length).arg(part.host.formatPart.timeText(at.fromMs, true))
        const pulls = part.host.comparePart.stretchesOf("pull").length
        const cruises = part.host.comparePart.stretchesOf("cruise").length
        return (pulls === 0 ? noPullText
                            : pulls === 1 ? qsTr("1 pull") : qsTr("%1 pulls").arg(pulls)) + " · "
               + (cruises === 0 ? qsTr("No cruise found")
                                : cruises === 1 ? qsTr("1 cruise") : qsTr("%1 cruises").arg(cruises))
    }
    // What a screen reader says for the status: the reason in full
    readonly property string jumpStatusSpoken: jumpStatus.replace(noPullText, noPullWhy)
}
