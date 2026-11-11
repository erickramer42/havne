// File/layer: app/qml/RingGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.gauges, QmlTest.accessibility, QmlTest.dashboard

import QtQuick
import QtQuick.Shapes

// Needle-less arc gauge for slow signals (temperatures): a thick
// rounded arc colored by zone, a centered numeric readout, and a thin
// zone strip outside the arc so the colors read as a scale.
GaugeBase {
    id: gauge

    property real startAngle: 150   // degrees; 0 = 3 o'clock, clockwise
    property real sweepAngle: 240

    // Ring centerline radius. The drawing reaches ~1.19 rr above the
    // center (ring, zone strip) and ~0.83 rr below it (arc ends plus
    // the min/max labels), so it fits a box of 2.4 rr by 2.05 rr.
    readonly property real rr: Math.max(1, Math.min(width / 2.4, height / 2.1))
    readonly property real ringWidth: rr * 0.17
    readonly property real zoneRadius: rr + ringWidth / 2 + rr * 0.09
    readonly property real zoneWidth: Math.max(2, rr * 0.035)
    readonly property real cx: width / 2
    readonly property real cy: height / 2 + rr * 0.18
    readonly property real endAngle: startAngle + sweepAngle
    readonly property real valueAngle: startAngle + fraction * sweepAngle
    readonly property color fillColor: zoneColor(from + fraction * (to - from), accent)

    function angleOf(v) { return startAngle + fractionOf(v) * sweepAngle }
    function px(deg, radius) { return cx + radius * Math.cos(deg * Math.PI / 180) }
    function py(deg, radius) { return cy + radius * Math.sin(deg * Math.PI / 180) }

    // Zone boundaries, each collapsing to its neighbor when unset
    readonly property real coldEnd: isNaN(coldBelow) ? from : coldBelow
    readonly property real dangerStart: isNaN(dangerFrom) ? to : dangerFrom
    readonly property real warnStart: isNaN(warnFrom) ? dangerStart : warnFrom
    readonly property real gapDeg: 1.5   // hairline gap between zone segments

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        // Zone strip: cold, normal, warn, danger
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.zoneRadius
            startAngle: gauge.startAngle
            sweepAngle: Math.max(0, gauge.angleOf(gauge.coldEnd) - startAngle - gauge.gapDeg)
            strokeColor: Theme.info
            strokeWidth: gauge.zoneWidth
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.zoneRadius
            startAngle: gauge.angleOf(gauge.coldEnd)
            sweepAngle: Math.max(0, gauge.angleOf(gauge.warnStart) - startAngle - gauge.gapDeg)
            strokeColor: Qt.alpha(Theme.gaugeTick, 0.14)
            strokeWidth: gauge.zoneWidth
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.zoneRadius
            startAngle: gauge.angleOf(gauge.warnStart)
            sweepAngle: Math.max(0, gauge.angleOf(gauge.dangerStart) - startAngle - gauge.gapDeg)
            strokeColor: Theme.gaugeWarn
            strokeWidth: gauge.zoneWidth
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.zoneRadius
            startAngle: gauge.angleOf(gauge.dangerStart)
            sweepAngle: gauge.endAngle - startAngle
            strokeColor: Theme.gaugeDanger
            // Heavier than the warning strip: danger is more than a hue
            strokeWidth: gauge.zoneWidth * 2
        }

        // Track
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.rr
            startAngle: gauge.startAngle; sweepAngle: gauge.sweepAngle
            strokeColor: Qt.alpha(Theme.gaugeTick, 0.07)
            strokeWidth: gauge.ringWidth
            capStyle: ShapePath.RoundCap
        }
    }

    // Value arc in its own Shape: only this one changes per frame
    Shape {
        anchors.fill: parent
        visible: gauge.hasValue || gauge.sweeping
        preferredRendererType: Shape.CurveRenderer

        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.rr
            startAngle: gauge.startAngle; sweepAngle: gauge.fraction * gauge.sweepAngle
            strokeColor: Qt.alpha(gauge.fillColor, 0.16)
            strokeWidth: gauge.ringWidth * 1.9
            capStyle: ShapePath.RoundCap
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.rr
            startAngle: gauge.startAngle; sweepAngle: gauge.fraction * gauge.sweepAngle
            strokeColor: gauge.fillColor
            strokeWidth: gauge.ringWidth
            capStyle: ShapePath.RoundCap
        }
    }

    // The held peak: a small mark on the zone strip at its angle, only when
    // asked for
    Rectangle {
        objectName: "peakMark"
        visible: gauge.showPeak && !isNaN(gauge.peakValue)
        readonly property real a: gauge.angleOf(isNaN(gauge.peakValue) ? gauge.from : gauge.peakValue)
        width: gauge.zoneWidth * 2.6; height: width; radius: width / 2
        x: gauge.px(a, gauge.zoneRadius) - width / 2
        y: gauge.py(a, gauge.zoneRadius) - height / 2
        color: Theme.gaugeTick   // no zone's color: it sits on the strip
    }

    // Tip knob
    Rectangle {
        visible: gauge.hasValue || gauge.sweeping
        width: gauge.ringWidth * 0.46; height: width; radius: width / 2
        x: gauge.px(gauge.valueAngle, gauge.rr) - width / 2
        y: gauge.py(gauge.valueAngle, gauge.rr) - height / 2
        color: Qt.alpha(Theme.gaugeTick, 0.92)
    }

    // Range ends under the arc tips
    Text {
        Accessible.ignored: true
        text: gauge.formatScale(gauge.from)
        x: gauge.px(gauge.startAngle, gauge.rr) - width / 2
        y: gauge.py(gauge.startAngle, gauge.rr) + gauge.ringWidth * 0.75
        font.pixelSize: Math.max(9, gauge.rr * 0.14)
        font.features: Theme.valueFeatures
        color: Theme.textDim
    }
    Text {
        Accessible.ignored: true
        text: gauge.formatScale(gauge.to)
        x: gauge.px(gauge.endAngle, gauge.rr) - width / 2
        y: gauge.py(gauge.endAngle, gauge.rr) + gauge.ringWidth * 0.75
        font.pixelSize: Math.max(9, gauge.rr * 0.14)
        font.features: Theme.valueFeatures
        color: Theme.textDim
    }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: gauge.cy - height / 2
        opacity: gauge.stale ? 0.45 : 1.0

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: gauge.rr * 0.03
            Text {
                id: readout
                objectName: "readout"
                Accessible.ignored: true
                text: gauge.valueText
                font.pixelSize: Math.max(16, gauge.rr * 0.5)
                font.weight: Font.Bold
                font.family: Theme.valueFamily
                font.features: Theme.valueFeatures
                color: gauge.hasValue ? gauge.zoneColor(gauge.value, Theme.gaugeReadout) : Theme.textDim
            }
            // Past dangerFrom, beside the value
            DangerMark {
                // The slot stays once a gauge has a danger zone, so the readout
                // never moves as the mark appears
                visible: !isNaN(gauge.dangerFrom)
                opacity: gauge.inDanger ? 1 : 0
                size: Math.max(12, gauge.rr * 0.3)
                anchors.verticalCenter: readout.verticalCenter
            }
            Text {
                objectName: "unit"
                Accessible.ignored: true
                visible: gauge.hasValue
                anchors.baseline: readout.baseline
                text: gauge.unit
                font.pixelSize: Math.max(10, gauge.rr * 0.17)
                color: Theme.textDim
            }
        }
        Text {
            Accessible.ignored: true
            anchors.horizontalCenter: parent.horizontalCenter
            text: gauge.label
            font.pixelSize: Math.max(9, gauge.rr * 0.13)
            color: Theme.textDim
        }
        Text {
            Accessible.ignored: true
            anchors.horizontalCenter: parent.horizontalCenter
            objectName: "gaugeNote"
            visible: text !== ""
            text: gauge.shownNote
            font.pixelSize: Math.max(9, gauge.rr * 0.12)
            color: Qt.alpha(Theme.textDim, 0.7)
        }
    }
}
