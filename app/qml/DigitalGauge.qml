// File/layer: app/qml/DigitalGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: manual smoke — DashboardPage under mock://demo; sparkline
//          scrolls at 10 Hz, flat-line guard holds at idle; "not in
//          profile" note for signals the active set lacks

import QtQuick
import QtQuick.Shapes

pragma ComponentBehavior: Bound

// Numeric tile: label, big value, an ~8 s sparkline and the session's
// min/max. For signals read as numbers (airflow, intake temp) rather
// than positions. The value is never eased: digits show the sample.
GaugeBase {
    id: gauge

    property string description: ""   // second line under the label
    property int historyLength: 80     // samples at the 10 Hz clock below
    property var history: []

    // Clear with the peaks, so a new session draws a fresh trace
    onPeaksReset: history = []

    // A fixed sample clock, not onValueChanged: an unchanged value
    // still advances the trace, so the x axis stays time
    Timer {
        interval: 100
        repeat: true
        running: gauge.hasValue && gauge.visible
        onTriggered: {
            const h = gauge.history.slice(1 - gauge.historyLength)
            h.push(gauge.value)
            gauge.history = h
        }
    }

    readonly property bool roomy: height >= 160
    readonly property color levelColor: gauge.hasValue
                                        ? gauge.zoneColor(gauge.value, Theme.text)
                                        : Theme.textDim

    // Polyline for the sparkline, newest sample on the right edge.
    // Range is the visible window, padded to at least 4 % of full scale
    // so idle noise doesn't fill the box.
    function tracePoints(w, h) {
        const n = history.length
        if (n < 2) return []
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
        const step = w / (historyLength - 1)
        const pts = []
        for (let i = 0; i < n; ++i) {
            pts.push(Qt.point(w - (n - 1 - i) * step,
                              h - 2 - (history[i] - lo) / (hi - lo) * (h - 4)))
        }
        return pts
    }

    Column {
        id: heading
        anchors { left: parent.left; right: status.left; top: parent.top }
        Text {
            width: parent.width
            text: gauge.label
            elide: Text.ElideRight
            font.pixelSize: 12
            font.weight: Font.DemiBold
            font.letterSpacing: 1.2
            font.capitalization: Font.AllUppercase
            color: Theme.textDim
        }
        Text {
            width: parent.width
            visible: gauge.roomy && text !== ""
            text: gauge.description
            elide: Text.ElideRight
            font.pixelSize: 11
            color: Qt.alpha(Theme.textDim, 0.7)
        }
    }

    // Live dot: zone colour while fresh, hollow without data
    Rectangle {
        id: status
        anchors { right: parent.right; top: parent.top; topMargin: 3 }
        width: 8; height: 8; radius: 4
        color: gauge.hasValue && !gauge.stale ? gauge.zoneColor(gauge.value, Theme.success)
                                              : "transparent"
        border.width: 1
        border.color: gauge.hasValue && !gauge.stale ? "transparent" : Theme.textDim
    }

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: valueBand.verticalCenter
        spacing: 4
        opacity: gauge.stale ? 0.45 : 1.0
        Text {
            id: readout
            text: gauge.valueText
            font.pixelSize: Math.max(18, Math.min(valueBand.height * 0.9, gauge.width * 0.24))
            font.weight: Font.DemiBold
            font.features: ({ "tnum": 1 })
            color: gauge.levelColor
        }
        Text {
            visible: gauge.hasValue
            anchors.baseline: readout.baseline
            text: gauge.unit
            font.pixelSize: Math.max(11, readout.font.pixelSize * 0.36)
            color: Theme.textDim
        }
    }
    // Vertical slot the readout centres in
    Item {
        id: valueBand
        anchors { left: parent.left; right: parent.right; top: heading.bottom; bottom: trace.top }
    }

    Shape {
        id: trace
        anchors { left: parent.left; right: parent.right; bottom: footer.top; bottomMargin: 6 }
        height: Math.max(20, gauge.height * 0.24)
        preferredRendererType: Shape.CurveRenderer

        readonly property var points: gauge.tracePoints(width, height)
        readonly property var area: points.length < 2 ? []
            : [Qt.point(points[0].x, height)].concat(points,
                                                      [Qt.point(width, height)])

        ShapePath {   // baseline
            strokeColor: Qt.alpha(Theme.text, 0.06)
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
    Text {
        anchors.centerIn: trace
        visible: !gauge.hasValue && text !== ""
        text: gauge.note
        font.pixelSize: 11
        color: Qt.alpha(Theme.textDim, 0.8)
    }

    Row {
        id: footer
        anchors { left: parent.left; bottom: parent.bottom }
        spacing: 14
        Repeater {
            model: [
                { tag: qsTr("MIN"), v: gauge.lowValue },
                { tag: qsTr("MAX"), v: gauge.peakValue }
            ]
            delegate: Text {
                required property var modelData
                text: "%1  %2".arg(modelData.tag)
                              .arg(isNaN(modelData.v) ? qsTr("—") : gauge.format(modelData.v))
                font.pixelSize: 11
                font.letterSpacing: 0.8
                font.features: ({ "tnum": 1 })
                color: Theme.textDim
            }
        }
    }
}
