// File/layer: app/qml/GaugeBase.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.gauges, QmlTest.accessibility, QmlTest.dashboard,
//          QmlTest.dashedit

import QtQuick

// Value plumbing shared by the dashboard gauges: range clamping,
// easing between the bridge's 10 Hz batches, zone colors, the
// connect sweep and peak/low hold. Subtypes only draw.
Item {
    id: base

    property real value: 0
    // The batch's highest and lowest, which the holds take: a batch
    // covers a 100 ms window, and `value` is only its newest
    property real windowHigh: value
    property real windowLow: value
    property bool hasValue: false   // false: parks at `from`, readout "—"
    property bool stale: false      // dims readouts and says so (LiveView's 3 s rule)
    property real from: 0
    property real to: 100
    property string label: ""
    property string unit: ""
    property string note: ""        // why there is no value, e.g. "not in profile"
    // The card's note line: why there is no value, or that the one shown is old
    readonly property string shownNote: !hasValue ? note : stale ? qsTr("no new data") : ""
    // A note for a missing value shares the title's line while it fits whole
    // beside it; if not, it takes the line (two at most), led by the title
    TextMetrics {
        id: noteMetrics
        text: base.shownNote
        font.pixelSize: 11
    }
    function noteTakesLine(titleWidth, room) {
        return !hasValue && shownNote !== "" && room - titleWidth - 10 < noteMetrics.advanceWidth
    }
    readonly property string titledNote: shownNote === "" || label === "" || shownNote.indexOf(label) >= 0
                                         ? shownNote : qsTr("%1: %2").arg(label).arg(shownNote)
    property int decimals: 0

    // Zones in value units; NaN disables one
    property real coldBelow: NaN
    property real warnFrom: NaN
    property real dangerFrom: NaN
    property color accent: Theme.gaugeValue   // normal-zone color
    // The big value's color outside a zone: the look's, or the cell's picked one
    property color readoutColor: Theme.gaugeReadout
    // The hold shown: a kind with a scale marks its peak on it (the dial's
    // tell-tale, the bar's and the ring's mark, the cells' notch); off by
    // default. Reset peaks clears it
    property bool showPeak: false

    // A little over one batch interval, so motion never stalls between
    // samples. Shift lights want less.
    property int smoothingMs: 180

    // Drawn position 0..1: the eased value, or the sweep while it plays
    readonly property real fraction: Math.max(sweepPos, fractionOf(smoothed))
    readonly property bool sweeping: sweepAnim.running
    // Past dangerFrom: the readout gains a DangerMark and the zone's stroke
    // is heavier, so danger is more than a hue
    readonly property bool inDanger: hasValue && !isNaN(dangerFrom) && value >= dangerFrom
    // An enumeration's label, shown in place of the number
    property string valueLabel: ""
    readonly property string valueText: hasValue ? (valueLabel !== "" ? valueLabel : format(value))
                                                 : qsTr("—")

    property real peakValue: NaN
    property real lowValue: NaN

    signal peaksReset()

    function fractionOf(v) { return Math.max(0, Math.min(1, (v - from) / (to - from))) }
    function format(v) { return Number(v).toLocaleString(Qt.locale(), 'f', decimals) }
    // Scale labels carry only the decimals the value needs: none for a
    // whole number, up to two under 10 and one under 100, whatever the
    // readout's decimals
    function formatScale(v) {
        const n = Number(v)
        const most = Math.abs(n) < 10 ? 2 : Math.abs(n) < 100 ? 1 : 0
        let d = 0
        while (d < most && Math.abs(n - Number(n.toFixed(d))) > 1e-9) ++d
        return n.toLocaleString(Qt.locale(), 'f', d)
    }
    function zoneColor(v, normal) {
        if (!isNaN(dangerFrom) && v >= dangerFrom) return Theme.gaugeDanger
        if (!isNaN(warnFrom) && v >= warnFrom) return Theme.gaugeWarn
        if (!isNaN(coldBelow) && v < coldBelow) return Theme.info
        return normal
    }
    function sweep() { sweepAnim.restart() }
    function resetPeaks() {
        peakValue = NaN
        lowValue = NaN
        peaksReset()
    }

    property real smoothed: hasValue ? Math.max(from, Math.min(to, value)) : from
    Behavior on smoothed {
        NumberAnimation { duration: base.smoothingMs; easing.type: Easing.OutCubic }
    }

    property real sweepPos: 0
    SequentialAnimation {
        id: sweepAnim
        NumberAnimation {
            target: base; property: "sweepPos"
            from: 0; to: 1; duration: 650; easing.type: Easing.InOutSine
        }
        PauseAnimation { duration: 150 }
        NumberAnimation {
            target: base; property: "sweepPos"
            to: 0; duration: 750; easing.type: Easing.InOutSine
        }
    }

    // Only fresh samples count toward the holds
    onWindowHighChanged: track()
    onWindowLowChanged: track()
    onHasValueChanged: track()
    function track() {
        if (!hasValue || stale) return
        if (isNaN(peakValue) || windowHigh > peakValue) peakValue = windowHigh
        if (isNaN(lowValue) || windowLow < lowValue) lowValue = windowLow
    }

    // One line for a screen reader: each kind keeps its parts out of the tree
    // (Accessible.ignored) and changes them by opacity, never visibility
    Accessible.role: Accessible.Indicator
    Accessible.name: hasValue ? qsTr("%1: %2 %3").arg(label).arg(valueText).arg(unit).trim()
                                + (inDanger ? qsTr(", danger") : "") + (stale ? qsTr(", no new data") : "")
                              : note !== "" ? qsTr("%1: %2").arg(label).arg(note) : label
}
