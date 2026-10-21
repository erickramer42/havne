// File/layer: app/qml/GaugeBase.qml | Qt6 Quick, UI layer
// Testing: manual smoke via DashboardPage under mock://demo — needles
//          ease between batches, sweep once per session, peaks reset

import QtQuick

// Value plumbing shared by the dashboard gauges: range clamping,
// easing between the bridge's 10 Hz batches, zone colours, the
// connect sweep and peak/low hold. Subtypes only draw.
Item {
    id: base

    property real value: 0
    property bool hasValue: false   // false: parks at `from`, readout "—"
    property bool stale: false      // dims readouts (LiveView's 3 s rule)
    property real from: 0
    property real to: 100
    property string label: ""
    property string unit: ""
    property string note: ""        // why there is no value, e.g. "not in profile"
    property int decimals: 0

    // Zones in value units; NaN disables one
    property real coldBelow: NaN
    property real warnFrom: NaN
    property real dangerFrom: NaN
    property color accent: Qt.lighter(Theme.primary, 1.3)   // normal-zone colour

    // A little over one batch interval, so motion never stalls between
    // samples. Shift lights want less.
    property int smoothingMs: 180

    // Drawn position 0..1: the eased value, or the sweep while it plays
    readonly property real fraction: Math.max(sweepPos, fractionOf(smoothed))
    readonly property bool sweeping: sweepAnim.running
    readonly property string valueText: hasValue ? format(value) : qsTr("—")

    property real peakValue: NaN
    property real lowValue: NaN

    signal peaksReset()

    function fractionOf(v) { return Math.max(0, Math.min(1, (v - from) / (to - from))) }
    function format(v) { return Number(v).toLocaleString(Qt.locale(), 'f', decimals) }
    function zoneColor(v, normal) {
        if (!isNaN(dangerFrom) && v >= dangerFrom) return Theme.danger
        if (!isNaN(warnFrom) && v >= warnFrom) return Theme.warning
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
    onValueChanged: track()
    onHasValueChanged: track()
    function track() {
        if (!hasValue || stale) return
        if (isNaN(peakValue) || value > peakValue) peakValue = value
        if (isNaN(lowValue) || value < lowValue) lowValue = value
    }

    Accessible.role: Accessible.Indicator
    Accessible.name: label
    Accessible.description: hasValue ? valueText + " " + unit : note
}
