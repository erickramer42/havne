// File/layer: app/qml/RingGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: manual smoke — DashboardPage under mock://demo; coolant
//          crosses cold → normal during warm-up; legible at 140 px

import QtQuick
import QtQuick.Shapes

// Needle-less arc gauge for slow signals (temperatures): a thick
// rounded arc coloured by zone, a centred numeric readout, and a thin
// zone strip outside the arc so the colours read as a scale.
GaugeBase {
    id: gauge

    property real startAngle: 150   // degrees; 0 = 3 o'clock, clockwise
    property real sweepAngle: 240

    // Ring centreline radius. The drawing reaches ~1.19 rr above the
    // centre (ring, zone strip) and ~0.83 rr below it (arc ends plus
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

    // Zone boundaries, each collapsing to its neighbour when unset
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
            strokeColor: Qt.alpha(Theme.text, 0.14)
            strokeWidth: gauge.zoneWidth
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.zoneRadius
            startAngle: gauge.angleOf(gauge.warnStart)
            sweepAngle: Math.max(0, gauge.angleOf(gauge.dangerStart) - startAngle - gauge.gapDeg)
            strokeColor: Theme.warning
            strokeWidth: gauge.zoneWidth
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.zoneRadius
            startAngle: gauge.angleOf(gauge.dangerStart)
            sweepAngle: gauge.endAngle - startAngle
            strokeColor: Theme.danger
            strokeWidth: gauge.zoneWidth
        }

        // Track
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.rr
            startAngle: gauge.startAngle; sweepAngle: gauge.sweepAngle
            strokeColor: Qt.alpha(Theme.text, 0.07)
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

    // Tip knob
    Rectangle {
        visible: gauge.hasValue || gauge.sweeping
        width: gauge.ringWidth * 0.46; height: width; radius: width / 2
        x: gauge.px(gauge.valueAngle, gauge.rr) - width / 2
        y: gauge.py(gauge.valueAngle, gauge.rr) - height / 2
        color: Qt.alpha("white", 0.92)
    }

    // Range ends under the arc tips
    Text {
        text: gauge.format(gauge.from)
        x: gauge.px(gauge.startAngle, gauge.rr) - width / 2
        y: gauge.py(gauge.startAngle, gauge.rr) + gauge.ringWidth * 0.75
        font.pixelSize: Math.max(9, gauge.rr * 0.14)
        font.features: ({ "tnum": 1 })
        color: Theme.textDim
    }
    Text {
        text: gauge.format(gauge.to)
        x: gauge.px(gauge.endAngle, gauge.rr) - width / 2
        y: gauge.py(gauge.endAngle, gauge.rr) + gauge.ringWidth * 0.75
        font.pixelSize: Math.max(9, gauge.rr * 0.14)
        font.features: ({ "tnum": 1 })
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
                text: gauge.valueText
                font.pixelSize: Math.max(16, gauge.rr * 0.5)
                font.weight: Font.DemiBold
                font.features: ({ "tnum": 1 })
                color: gauge.hasValue ? gauge.zoneColor(gauge.value, Theme.text) : Theme.textDim
            }
            Text {
                visible: gauge.hasValue
                anchors.baseline: readout.baseline
                text: gauge.unit
                font.pixelSize: Math.max(10, gauge.rr * 0.17)
                color: Theme.textDim
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: gauge.label
            font.pixelSize: Math.max(9, gauge.rr * 0.13)
            font.letterSpacing: 1.5
            font.capitalization: Font.AllUppercase
            color: Theme.textDim
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !gauge.hasValue && text !== ""
            text: gauge.note
            font.pixelSize: Math.max(9, gauge.rr * 0.12)
            color: Qt.alpha(Theme.textDim, 0.7)
        }
    }
}
