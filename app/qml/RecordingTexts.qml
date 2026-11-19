// File/layer: app/qml/RecordingTexts.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.recordings, QmlTest.phone (test_qml)

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
}
