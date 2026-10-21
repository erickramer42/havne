// File/layer: app/qml/GaugeArc.qml | Qt6 Quick Shapes, UI layer
// Testing: visual, through AnalogGauge and RingGauge

import QtQuick
import QtQuick.Shapes

// One stroked circular arc inside a gauge's Shape. Angles in degrees,
// 0 = 3 o'clock, clockwise: the PathAngleArc convention.
ShapePath {
    id: arc
    property real centerX: 0
    property real centerY: 0
    property real radius: 0
    property real startAngle: 0
    property real sweepAngle: 0

    fillColor: "transparent"
    capStyle: ShapePath.FlatCap

    PathAngleArc {
        centerX: arc.centerX
        centerY: arc.centerY
        radiusX: arc.radius
        radiusY: arc.radius
        startAngle: arc.startAngle
        sweepAngle: arc.sweepAngle
    }
}
