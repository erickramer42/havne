// File/layer: app/qml/GaugeBase.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.gauges, QmlTest.accessibility, QmlTest.dashboard{,_pages,_panel},
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

    // Zones in value units; NaN disables one. From is at or above, Below is
    // strictly below; danger beats warning beats cold, on either side
    property real coldBelow: NaN
    property real warnFrom: NaN
    property real dangerFrom: NaN
    property real warnBelow: NaN
    property real dangerBelow: NaN
    // Where the low bands end on the scale: danger from the start of it to
    // dangerBelow, warning from there to warnBelow (each clipped to the scale)
    readonly property real lowDangerEnd: isNaN(dangerBelow) ? from
                                         : Math.max(from, Math.min(to, dangerBelow))
    readonly property real lowWarnEnd: isNaN(warnBelow) ? lowDangerEnd
                                       : Math.max(lowDangerEnd, Math.min(to, warnBelow))
    readonly property bool hasDangerZone: !isNaN(dangerFrom) || !isNaN(dangerBelow)
    property color accent: Theme.gaugeValue   // normal-zone color
    // The big value's color outside a zone: the look's, or the cell's picked one
    property color readoutColor: Theme.gaugeReadout
    // The cell's picked color ("" for none): with no value it tints the title,
    // so a page's color grouping shows while it is built offline; a pick the
    // title can't be read in (under the text ratio) leaves it dim
    property string picked: ""
    readonly property color titleColor: !hasValue && picked !== "" && Theme.gaugeTextReadable(picked) ? picked : Theme.textDim
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
    // In the danger zone, past dangerFrom or under dangerBelow: the readout
    // gains a DangerMark and the zone's stroke is heavier, so danger is more
    // than a hue
    readonly property bool inDanger: hasValue && inDangerAt(value)
    // The highest alert ON that reads this signal (1 warning, 2 danger): the
    // value's text takes its role over the zone's, the zones' drawing kept
    property int alertLevel: 0
    readonly property bool showsDanger: inDanger || (hasValue && alertLevel === 2)
    // The mark's slot: kept by a danger zone, opened by a danger alert on a
    // gauge with none, so danger is never a hue alone
    readonly property bool dangerSlot: hasDangerZone || alertLevel === 2
    readonly property color valueColor: valueColorWith(readoutColor)
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
    function inDangerAt(v) {
        return (!isNaN(dangerFrom) && v >= dangerFrom) || (!isNaN(dangerBelow) && v < dangerBelow)
    }
    // The value's own color, its normal one given: an alert's, else its zone's
    function valueColorWith(normal) {
        if (!hasValue) return Theme.textDim
        if (alertLevel === 2) return Theme.alertDangerFill
        if (alertLevel === 1) return Theme.alertWarnFill
        return zoneColor(value, normal)
    }
    function zoneColor(v, normal) {
        if (inDangerAt(v)) return Theme.gaugeDanger
        if ((!isNaN(warnFrom) && v >= warnFrom) || (!isNaN(warnBelow) && v < warnBelow))
            return Theme.gaugeWarn
        if (!isNaN(coldBelow) && v < coldBelow) return Theme.info
        return normal
    }
    function sweep() { sweepAnim.restart() }
    function resetPeaks() {
        peakValue = NaN
        lowValue = NaN
        peaksReset()
    }

    // A trace's [ts, val, ...] (Bridge.signalModel.trace) spans its lowest and
    // highest number, padded to 4 % of full scale so idle noise doesn't fill it
    function traceExtent(samples) {
        let lo = Infinity, hi = -Infinity
        for (let i = 1; i < samples.length; i += 2) {
            const v = samples[i]
            if (!isFinite(v)) continue
            if (v < lo) lo = v
            if (v > hi) hi = v
        }
        if (lo > hi)
            return { lo: from, hi: to }
        const minSpan = (to - from) * 0.04
        if (hi - lo < minSpan) {
            const mid = (hi + lo) / 2
            lo = mid - minSpan / 2
            hi = mid + minSpan / 2
        }
        return { lo: lo, hi: hi }
    }
    // Its pieces for a w x h box, by time (the newest at the right edge): only a
    // break (a point that isn't a number) ends one, and a lone point is a dot
    function tracePieces(samples, windowMs, w, h, lo, hi) {
        const n = samples.length
        if (n < 2 || w <= 0 || windowMs <= 0)
            return []
        const newest = samples[n - 2]
        const range = hi - lo
        const pieces = []
        let piece = []
        const end = () => {
            if (piece.length === 1)
                piece.push(Qt.point(piece[0].x + (piece[0].x > w - 1 ? -1 : 1), piece[0].y))
            if (piece.length > 0)
                pieces.push(piece)
            piece = []
        }
        for (let i = 0; i < n; i += 2) {
            const v = samples[i + 1]
            if (!isFinite(v)) {
                end()
                continue
            }
            const y = range > 0 && isFinite(range) ? h - 2 - (v - lo) / range * (h - 4) : h / 2
            piece.push(Qt.point(w - (newest - samples[i]) / windowMs * w, y))
        }
        end()
        return pieces
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
                                + (inDanger ? qsTr(", danger") : "")
                                + (alertLevel === 2 ? qsTr(", danger alert") : alertLevel === 1 ? qsTr(", warning alert") : "")
                                + (stale ? qsTr(", no new data") : "")
                              : note !== "" ? qsTr("%1: %2").arg(label).arg(note) : label
}
