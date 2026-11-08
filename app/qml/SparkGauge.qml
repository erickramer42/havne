// File/layer: app/qml/SparkGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.gauges, QmlTest.dashboard (test_qml)

import QtQuick
import QtQuick.Shapes

// One trace over a window (ADR-0057 section 1): the newest value at its end,
// the window's low and high as a scale. DigitalGauge's ShapePath trace with
// a longer window: at 10 Hz at most 1,200 points. It samples only while it
// is on screen, so a page left and returned to starts its trace again; the
// trace is a glance, Live's chart is the record.
GaugeBase {
    id: gauge

    // The window in seconds (10 to 120)
    property int windowS: 30

    readonly property int span: Math.max(10, Math.min(120, Math.round(windowS)))
    readonly property int historyLength: span * 10
    property var history: []

    // One sample every 100 ms while a value is coming: a value that holds
    // still still advances the trace, so the x axis stays time
    Timer {
        interval: 100
        repeat: true
        running: gauge.hasValue && gauge.visible
        onTriggered: gauge.takeSample()
    }
    function takeSample() {
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

    function tracePoints(w, h) {
        const n = history.length
        if (n < 2) return []
        const step = w / (historyLength - 1)
        const range = extent.hi - extent.lo
        const pts = []
        for (let i = 0; i < n; ++i) {
            pts.push(Qt.point(w - (n - 1 - i) * step,
                              h - 2 - (history[i] - extent.lo) / range * (h - 4)))
        }
        return pts
    }

    implicitHeight: 96

    Text {
        id: title
        anchors { left: parent.left; top: parent.top }
        text: gauge.label
        font.pixelSize: 12
        font.weight: Font.DemiBold
        font.letterSpacing: 1.2
        font.capitalization: Font.AllUppercase
        color: Theme.textDim
    }
    Text {
        anchors { left: title.right; leftMargin: 10; baseline: title.baseline }
        visible: !gauge.hasValue
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
            font.weight: Font.DemiBold
            font.features: ({ "tnum": 1 })
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
        id: scale
        anchors { left: parent.left; top: trace.top; bottom: trace.bottom }
        width: Math.max(highText.implicitWidth, lowText.implicitWidth)
        Text {
            id: highText
            objectName: "scaleHigh"
            text: gauge.history.length > 0 ? gauge.format(gauge.extent.hi) : ""
            font.pixelSize: 10
            font.features: ({ "tnum": 1 })
            color: Qt.alpha(Theme.textDim, 0.8)
        }
        Item { width: 1; height: Math.max(0, scale.height - highText.height - lowText.height) }
        Text {
            id: lowText
            objectName: "scaleLow"
            text: gauge.history.length > 0 ? gauge.format(gauge.extent.lo) : ""
            font.pixelSize: 10
            font.features: ({ "tnum": 1 })
            color: Qt.alpha(Theme.textDim, 0.8)
        }
    }

    Shape {
        id: trace
        objectName: "trace"
        anchors { left: scale.right; leftMargin: 6; right: parent.right
                  top: readout.bottom; topMargin: 6; bottom: parent.bottom }
        preferredRendererType: Shape.CurveRenderer

        readonly property var points: gauge.tracePoints(width, height)
        readonly property var area: points.length < 2 ? []
            : [Qt.point(points[0].x, height)].concat(points, [Qt.point(width, height)])

        ShapePath {   // baseline
            strokeColor: Qt.alpha(Theme.gaugeTick, 0.06)
            strokeWidth: 1
            fillColor: "transparent"
            startX: 0; startY: trace.height - 0.5
            PathLine { x: trace.width; y: trace.height - 0.5 }
        }
        ShapePath {
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 0; y1: 0; x2: 0; y2: trace.height
                GradientStop { position: 0.0; color: Qt.alpha(gauge.accent, 0.30) }
                GradientStop { position: 1.0; color: Qt.alpha(gauge.accent, 0.0) }
            }
            PathPolyline { path: trace.area }
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
