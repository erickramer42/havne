// File/layer: app/qml/SparkGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.gauges, QmlTest.dashboard

import QtQuick
import QtQuick.Shapes

// One trace over a window (ADR-0057 section 1): the newest value at its end,
// the window's low and high as a scale. DigitalGauge's ShapePath trace with
// a longer window: at 10 Hz at most 1,200 samples, drawn as about one point
// per 1.5 px of width. It samples only while its page is the one shown and
// the app is active (`shown`, from the dashboard: a SwipeView keeps its
// neighbors visible, so `visible` alone would not stop them), and it starts
// its trace again when it stops being shown, on Reset peaks (a connect, a
// change of units) and never keeps a value that is not a number. The trace
// is a glance, Live's chart is the record.
GaugeBase {
    id: gauge

    // The window in seconds (10 to 120)
    property int windowS: 30
    // Whether the dashboard's page holding this gauge is the one on screen
    // and the app is active
    property bool shown: true

    readonly property int span: Math.max(10, Math.min(120, Math.round(windowS)))
    readonly property int historyLength: span * 10
    property var history: []

    onPeaksReset: history = []
    onShownChanged: if (!shown) history = []
    onVisibleChanged: if (!visible) history = []

    // One sample every 100 ms while a value is coming: a value that holds
    // still still advances the trace, so the x axis stays time
    Timer {
        interval: 100
        repeat: true
        running: gauge.hasValue && gauge.visible && gauge.shown
        onTriggered: gauge.takeSample()
    }
    function takeSample() {
        if (!isFinite(value))
            return
        const h = history.slice(1 - historyLength)
        h.push(value)
        history = h
    }

    // The window's own low and high, padded to at least 4 % of full scale
    // so idle noise doesn't fill the box
    readonly property var extent: {
        if (history.length < 1)
            return { lo: from, hi: to }
        let lo = Infinity, hi = -Infinity
        for (const v of history) {
            if (v < lo) lo = v
            if (v > hi) hi = v
        }
        const minSpan = (to - from) * 0.04
        if (hi - lo < minSpan) {
            const mid = (hi + lo) / 2
            lo = mid - minSpan / 2
            hi = mid + minSpan / 2
        }
        return { lo: lo, hi: hi }
    }

    // The trace's points for a box, newest at the right edge, one point per
    // 1.5 px at most (the newest kept). A flat range draws a midline
    function tracePoints(w, h) {
        const n = history.length
        if (n < 2 || w <= 0)
            return []
        const step = w / (historyLength - 1)
        const stride = Math.max(1, Math.ceil(n / (w / 1.5 + 1)))
        const range = extent.hi - extent.lo
        const points = []
        for (let i = n - 1; i >= 0; i -= stride) {
            const y = range > 0 && isFinite(range)
                ? h - 2 - (history[i] - extent.lo) / range * (h - 4) : h / 2
            points.push(Qt.point(w - (n - 1 - i) * step, y))
        }
        points.reverse()
        return points
    }

    implicitHeight: 96

    Text {
        id: title
        anchors { left: parent.left; top: parent.top }
        text: gauge.label
        font.pixelSize: 12
        font.weight: Font.DemiBold
        color: Theme.textDim
    }
    Text {
        objectName: "gaugeNote"
        anchors { left: title.right; leftMargin: 10; right: readout.left; rightMargin: 8
                  baseline: title.baseline }
        visible: !gauge.hasValue && width > 0
        elide: Text.ElideRight
        text: gauge.note
        font.pixelSize: 11
        color: Qt.alpha(Theme.textDim, 0.7)
    }

    // The newest value, at the trace's end
    Row {
        id: readout
        anchors { right: parent.right; top: parent.top }
        spacing: 3
        opacity: gauge.stale ? 0.45 : 1.0
        Text {
            id: valueLabel
            objectName: "readout"
            text: gauge.valueText
            font.pixelSize: 22
            font.weight: Font.Bold
            font.family: Theme.valueFamily
            font.features: Theme.valueFeatures
            color: gauge.hasValue ? gauge.zoneColor(gauge.value, Theme.gaugeReadout) : Theme.textDim
        }
        DangerMark {
            visible: !isNaN(gauge.dangerFrom)
            opacity: gauge.inDanger ? 1 : 0
            size: 18
            anchors.verticalCenter: valueLabel.verticalCenter
        }
        Text {
            objectName: "unit"
            visible: gauge.hasValue
            anchors.baseline: valueLabel.baseline
            text: gauge.unit
            font.pixelSize: 12
            color: Theme.textDim
        }
    }

    // The scale: the window's high at the top, its low at the bottom
    Column {
        id: scaleBox
        objectName: "scaleBox"
        anchors { left: parent.left; top: trace.top; bottom: trace.bottom }
        // Sized from the scale's ends and a digit more, so the trace
        // doesn't move when a label gains a digit
        width: Math.max(highText.implicitWidth, lowText.implicitWidth,
                        sampleLow.implicitWidth, sampleHigh.implicitWidth)
        Text {
            id: highText
            objectName: "scaleHigh"
            text: gauge.history.length > 0 ? gauge.format(gauge.extent.hi) : ""
            font.pixelSize: 10
            font.features: Theme.valueFeatures
            color: Qt.alpha(Theme.textDim, 0.8)
        }
        Item { width: 1; height: Math.max(0, scaleBox.height - highText.height - lowText.height) }
        Text {
            id: lowText
            objectName: "scaleLow"
            text: gauge.history.length > 0 ? gauge.format(gauge.extent.lo) : ""
            font.pixelSize: 10
            font.features: Theme.valueFeatures
            color: Qt.alpha(Theme.textDim, 0.8)
        }
    }

    Text {
        id: sampleLow
        visible: false
        text: gauge.format(gauge.from) + "0"
        font.pixelSize: 10
        font.features: Theme.valueFeatures
    }
    Text {
        id: sampleHigh
        visible: false
        text: gauge.format(gauge.to) + "0"
        font.pixelSize: 10
        font.features: Theme.valueFeatures
    }

    Shape {
        id: trace
        objectName: "trace"
        anchors { left: scaleBox.right; leftMargin: 6; right: parent.right
                  top: readout.bottom; topMargin: 6; bottom: parent.bottom }
        preferredRendererType: Shape.CurveRenderer

        readonly property var points: gauge.tracePoints(width, height)

        ShapePath {   // baseline
            strokeColor: Qt.alpha(Theme.gaugeTick, 0.06)
            strokeWidth: 1
            fillColor: "transparent"
            startX: 0; startY: trace.height - 0.5
            PathLine { x: trace.width; y: trace.height - 0.5 }
        }
        ShapePath {
            strokeColor: gauge.stale ? Theme.textDim : gauge.accent
            strokeWidth: 1.5
            fillColor: "transparent"
            joinStyle: ShapePath.RoundJoin
            capStyle: ShapePath.RoundCap
            PathPolyline { path: trace.points }
        }
    }
}
