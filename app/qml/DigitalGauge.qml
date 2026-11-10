// File/layer: app/qml/DigitalGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.kinds (the brief tile), QmlTest.gauges, QmlTest.accessibility,
//          QmlTest.dashboard;
//          manual smoke: DashboardPage under mock://demo

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
    // Whether the page holding this tile is on screen: a tile on another
    // page takes no samples (its trace picks up where it stopped)
    property bool shown: true

    // Clear with the peaks, so a new session draws a fresh trace
    onPeaksReset: history = []

    // A fixed sample clock, not onValueChanged: an unchanged value
    // still advances the trace, so the x axis stays time
    Timer {
        interval: 100
        repeat: true
        objectName: "sampler"
        running: gauge.hasValue && gauge.visible && gauge.shown
        // A value that isn't a number is never kept, as SparkGauge's
        onTriggered: {
            if (!isFinite(gauge.value))
                return
            const h = gauge.history.slice(1 - gauge.historyLength)
            h.push(gauge.value)
            gauge.history = h
        }
    }

    readonly property bool roomy: height >= 160
    // Too short for the trace and the footer: the label and the value alone
    readonly property bool brief: height < 84
    readonly property color levelColor: gauge.hasValue
                                        ? gauge.zoneColor(gauge.value, Theme.gaugeReadout)
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
            Accessible.ignored: true
            width: parent.width
            text: gauge.label
            elide: Text.ElideRight
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: Theme.textDim
        }
        Text {
            Accessible.ignored: true
            width: parent.width
            visible: gauge.roomy && text !== ""
            text: gauge.description
            elide: Text.ElideRight
            font.pixelSize: 11
            color: Qt.alpha(Theme.textDim, 0.7)
        }
    }

    // Live dot: zone color while fresh, hollow without data
    Rectangle {
        id: status
        objectName: "statusDot"
        anchors { right: parent.right; top: parent.top; topMargin: 3 }
        width: 8; height: 8; radius: 4
        color: gauge.hasValue && !gauge.stale ? gauge.zoneColor(gauge.value, Theme.success)
                                              : "transparent"
        border.width: 1
        border.color: gauge.hasValue && !gauge.stale ? "transparent" : Theme.textDim
    }

    Row {
        id: valueRow
        anchors.left: parent.left
        anchors.verticalCenter: valueBand.verticalCenter
        spacing: 4
        opacity: gauge.stale ? 0.45 : 1.0
        Text {
            id: readout
            objectName: "readout"
            Accessible.ignored: true
            text: gauge.valueText
            font.pixelSize: Math.max(18, Math.min(valueBand.height * 0.9, gauge.width * 0.24))
            font.weight: Font.Bold
            font.family: Theme.valueFamily
            font.features: Theme.valueFeatures
            color: gauge.levelColor
        }
        // Past dangerFrom, beside the value
        DangerMark {
            // The slot stays once a gauge has a danger zone, so the readout
            // never moves as the mark appears
            visible: !isNaN(gauge.dangerFrom)
            opacity: gauge.inDanger ? 1 : 0
            size: Math.max(14, readout.font.pixelSize * 0.55)
            anchors.verticalCenter: readout.verticalCenter
        }
        Text {
            objectName: "unit"
            Accessible.ignored: true
            visible: gauge.hasValue
            anchors.baseline: readout.baseline
            text: gauge.unit
            font.pixelSize: Math.max(11, readout.font.pixelSize * 0.36)
            color: Theme.textDim
        }
    }
    // Vertical slot the readout centers in
    Item {
        id: valueBand
        anchors { left: parent.left; right: parent.right; top: heading.bottom
                  bottom: gauge.brief ? parent.bottom : trace.top }
    }

    Shape {
        id: trace
        objectName: "trace"
        visible: !gauge.brief
        anchors { left: parent.left; right: parent.right; bottom: footer.top; bottomMargin: 6 }
        height: Math.max(20, gauge.height * 0.24)
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
    // In the trace's place, or beside the value on a card too short for one
    Text {
        objectName: "gaugeNote"
        Accessible.ignored: true
        anchors.left: gauge.brief ? valueRow.right : parent.left
        anchors.leftMargin: gauge.brief ? 8 : 0
        anchors.right: parent.right
        anchors.verticalCenter: gauge.brief ? valueBand.verticalCenter : trace.verticalCenter
        horizontalAlignment: gauge.brief ? Text.AlignLeft : Text.AlignHCenter
        elide: Text.ElideRight
        visible: !gauge.hasValue && text !== ""
        text: gauge.note
        font.pixelSize: 11
        color: Qt.alpha(Theme.textDim, 0.8)
    }

    Row {
        id: footer
        objectName: "footer"
        visible: !gauge.brief
        anchors { left: parent.left; bottom: parent.bottom }
        spacing: 14
        // Two fixed items: a Repeater over the holds would rebuild both at
        // each new low or peak
        component Hold: Text {
            required property string tag
            required property real held
            text: "%1  %2".arg(tag).arg(isNaN(held) ? qsTr("—") : gauge.format(held))
            font.pixelSize: 11
            font.features: Theme.valueFeatures
            color: Theme.textDim
            Accessible.ignored: true
        }
        Hold { objectName: "lowHold"; tag: qsTr("MIN"); held: gauge.lowValue }
        Hold { objectName: "peakHold"; tag: qsTr("MAX"); held: gauge.peakValue }
    }
}
