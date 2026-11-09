// File/layer: app/qml/AnalogGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: manual smoke: DashboardPage under mock://demo at 160 px
//          (numerals shrink, then thin out) through 480 px; sweep on
//          connect; tap clears the peak marker
//          QML: QmlTest.dashboard (no negative tick count while a
//          reused cell rebinds)

import QtQuick
import QtQuick.Shapes

pragma ComponentBehavior: Bound

// Needle dial: tach, speedo. Face, ticks, numerals and zones are
// static Rectangles, Text and Shape arcs. Per frame only the needle
// (shadow and body) rotates and, with showProgress, the three progress
// arcs (two glow, one core) change their sweep. No Canvas:
// its repaint is a CPU raster of the whole dial, which a 3x phone
// screen would pay at animation rate.
GaugeBase {
    id: gauge

    property real majorStep: 10
    property real minorStep: 5
    property real labelDivisor: 1       // tach: 1000, numerals read 0..8
    property real startAngle: 135       // degrees; 0 = 3 o'clock, clockwise
    property real sweepAngle: 270
    property string caption: ""         // above the hub, e.g. "×1000 r/min"
    property string peakLabel: qsTr("Peak")
    property bool showProgress: false   // lit arc from `from` to the needle
    property color needleColor: Theme.gaugeNeedle

    // Geometry, all from the square the dial fits in
    readonly property real size: Math.min(width, height)
    readonly property real r: size / 2
    readonly property real cx: width / 2
    readonly property real cy: height / 2
    readonly property real tickOuter: r * 0.84
    readonly property real majorLen: size * 0.055
    readonly property real minorLen: size * 0.028
    readonly property real ringRadius: r * 0.89
    readonly property real ringWidth: Math.max(2.5, size * 0.013)
    readonly property real numRadius: r * 0.63

    // None while a reused cell rebinds its ends one at a time
    readonly property int majorCount: Math.max(0, Math.floor((to - from) / majorStep + 1e-9) + 1)
    readonly property int minorCount: Math.max(0, Math.floor((to - from) / minorStep + 1e-9) + 1)
    // Numerals shrink to fit the arc between them, down to 9 px; past
    // that every other one (or more) is dropped
    readonly property real labelGap: numRadius * sweepAngle * Math.PI / 180
                                     / Math.max(1, majorCount - 1)
    readonly property real labelEm: 0.62 * formatScale(to / labelDivisor).length + 0.6
    readonly property real numSize: Math.max(9, Math.min(size * 0.07, labelGap / labelEm))
    readonly property int labelEvery: Math.max(1, Math.ceil(numSize * labelEm / labelGap))

    readonly property real warnStart: isNaN(warnFrom) ? dangerStart : warnFrom
    readonly property real dangerStart: isNaN(dangerFrom) ? to : dangerFrom
    readonly property real needleAngle: startAngle + fraction * sweepAngle
    readonly property color progressColor: zoneColor(from + fraction * (to - from), accent)

    function angleOf(v) { return startAngle + fractionOf(v) * sweepAngle }
    function px(deg, radius) { return cx + radius * Math.cos(deg * Math.PI / 180) }
    function py(deg, radius) { return cy + radius * Math.sin(deg * Math.PI / 180) }

    TapHandler {
        enabled: gauge.showPeak
        onTapped: gauge.resetPeaks()
    }

    // ---- Bezel and face
    Rectangle {
        anchors.centerIn: parent
        width: gauge.size; height: width; radius: width / 2
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.gaugeShade.bezelHi }
            GradientStop { position: 0.45; color: Theme.gaugeFace }
            GradientStop { position: 1.0; color: Theme.gaugeShade.bezelLo }
        }
    }
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {   // lit from above
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: gauge.cx; centerY: gauge.cy - gauge.r * 0.35
                centerRadius: gauge.r * 1.25
                focalX: gauge.cx; focalY: gauge.cy - gauge.r * 0.35
                GradientStop { position: 0.0; color: Theme.gaugeShade.faceHi }
                GradientStop { position: 1.0; color: Theme.gaugeShade.faceLo }
            }
            PathAngleArc {
                centerX: gauge.cx; centerY: gauge.cy
                radiusX: gauge.r * 0.95; radiusY: gauge.r * 0.95
                startAngle: 0; sweepAngle: 360
            }
        }
    }

    // ---- Chapter ring and zone washes behind the ticks
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.tickOuter
            startAngle: gauge.startAngle; sweepAngle: gauge.sweepAngle
            strokeColor: Qt.alpha(Theme.gaugeTick, 0.16)
            strokeWidth: Math.max(1, gauge.size * 0.004)
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy
            radius: gauge.tickOuter - gauge.majorLen / 2
            startAngle: gauge.angleOf(gauge.warnStart)
            sweepAngle: gauge.angleOf(gauge.dangerStart) - startAngle
            strokeColor: Qt.alpha(Theme.gaugeWarn, 0.14)
            strokeWidth: gauge.majorLen
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy
            radius: gauge.tickOuter - gauge.majorLen / 2
            startAngle: gauge.angleOf(gauge.dangerStart)
            sweepAngle: gauge.angleOf(gauge.to) - startAngle
            strokeColor: Qt.alpha(Theme.gaugeDanger, 0.30)
            strokeWidth: gauge.majorLen
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.tickOuter
            startAngle: gauge.angleOf(gauge.warnStart)
            sweepAngle: gauge.angleOf(gauge.dangerStart) - startAngle
            strokeColor: Theme.gaugeWarn
            strokeWidth: Math.max(2, gauge.size * 0.009)
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.tickOuter
            startAngle: gauge.angleOf(gauge.dangerStart)
            sweepAngle: gauge.angleOf(gauge.to) - startAngle
            strokeColor: Theme.gaugeDanger
            // Heavier than the warning stroke: danger is more than a hue
            strokeWidth: Math.max(4, gauge.size * 0.02)
        }
    }

    // ---- Progress ring: track, two-step glow, core
    Shape {
        anchors.fill: parent
        visible: gauge.showProgress
        preferredRendererType: Shape.CurveRenderer
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.ringRadius
            startAngle: gauge.startAngle; sweepAngle: gauge.sweepAngle
            strokeColor: Qt.alpha(Theme.gaugeTick, 0.06)
            strokeWidth: gauge.ringWidth
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.ringRadius
            startAngle: gauge.startAngle; sweepAngle: gauge.fraction * gauge.sweepAngle
            strokeColor: Qt.alpha(gauge.progressColor, 0.07)
            strokeWidth: gauge.ringWidth * 4
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.ringRadius
            startAngle: gauge.startAngle; sweepAngle: gauge.fraction * gauge.sweepAngle
            strokeColor: Qt.alpha(gauge.progressColor, 0.18)
            strokeWidth: gauge.ringWidth * 2.2
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.ringRadius
            startAngle: gauge.startAngle; sweepAngle: gauge.fraction * gauge.sweepAngle
            strokeColor: gauge.progressColor
            strokeWidth: gauge.ringWidth
        }
    }

    // ---- Ticks: full-size items rotated about the dial center
    Repeater {
        model: gauge.minorCount
        delegate: Item {
            id: tick
            required property int index
            readonly property real v: gauge.from + index * gauge.minorStep
            readonly property real steps: (v - gauge.from) / gauge.majorStep
            readonly property bool major: Math.abs(steps - Math.round(steps)) < 1e-6

            width: gauge.width; height: gauge.height
            rotation: gauge.angleOf(v) + 90   // 0 degrees rotation = 12 o'clock

            Rectangle {
                antialiasing: true
                width: tick.major ? Math.max(2, gauge.size * 0.009)
                                  : Math.max(1, gauge.size * 0.0045)
                height: tick.major ? gauge.majorLen : gauge.minorLen
                x: gauge.cx - width / 2
                y: gauge.cy - gauge.tickOuter
                color: gauge.zoneColor(tick.v, tick.major ? Theme.gaugeTick
                                                          : Qt.alpha(Theme.gaugeTick, 0.5))
            }
        }
    }

    Repeater {
        model: gauge.majorCount
        delegate: Text {
            required property int index
            readonly property real v: gauge.from + index * gauge.majorStep
            readonly property real a: gauge.angleOf(v)

            visible: index % gauge.labelEvery === 0
            text: gauge.formatScale(v / gauge.labelDivisor)
            x: gauge.px(a, gauge.numRadius) - width / 2
            y: gauge.py(a, gauge.numRadius) - height / 2
            font.pixelSize: gauge.numSize
            font.weight: Font.DemiBold
            font.features: Theme.valueFeatures
            color: gauge.zoneColor(v, Theme.gaugeTick)
        }
    }

    // ---- Peak tell-tale: an amber wedge between ring and rim
    Item {
        width: gauge.width; height: gauge.height
        visible: gauge.showPeak && !isNaN(gauge.peakValue)
        rotation: gauge.angleOf(isNaN(gauge.peakValue) ? gauge.from : gauge.peakValue)
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                id: wedge
                readonly property real m: gauge.size * 0.02
                readonly property real tip: gauge.cx + gauge.ringRadius - gauge.ringWidth
                strokeColor: "transparent"
                fillColor: Theme.gaugeWarn
                startX: wedge.tip; startY: gauge.cy
                PathLine { x: wedge.tip + wedge.m * 1.6; y: gauge.cy - wedge.m }
                PathLine { x: wedge.tip + wedge.m * 1.6; y: gauge.cy + wedge.m }
                PathLine { x: wedge.tip; y: gauge.cy }
            }
        }
    }

    // ---- Readout: caption above the hub, value below it
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: gauge.cy - gauge.r * 0.36 - height / 2
        text: gauge.caption
        font.pixelSize: Math.max(10, gauge.size * 0.038)
        color: Theme.textDim
    }
    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: gauge.cy + gauge.r * 0.2
        opacity: gauge.stale ? 0.45 : 1.0

        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: valueLabel.width
            height: valueLabel.height
            Text {
                id: valueLabel
                objectName: "readout"
                text: gauge.valueText
                // Sized to clear the end numerals beside it at full scale
                font.pixelSize: Math.max(16, gauge.size * 0.1)
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
                size: valueLabel.font.pixelSize * 0.8
                anchors { left: valueLabel.right; leftMargin: 4; verticalCenter: valueLabel.verticalCenter }
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: gauge.unit
            font.pixelSize: Math.max(10, gauge.size * 0.036)
            color: Theme.textDim
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            topPadding: gauge.size * 0.012
            text: !gauge.hasValue ? gauge.note
                 : gauge.showPeak && !isNaN(gauge.peakValue)
                   ? "%1 %2".arg(gauge.peakLabel).arg(gauge.format(gauge.peakValue))
                   : ""
            font.pixelSize: Math.max(9, gauge.size * 0.032)
            font.features: Theme.valueFeatures
            color: gauge.hasValue ? Theme.gaugeWarn : Theme.textDim
        }
    }

    // ---- Needle: shadow, then body; both drawn pointing at 3 o'clock
    component Needle: Shape {
        id: needle
        property color tint: gauge.needleColor
        property bool shadow: false
        property real shiftX: 0     // the shadow's offset
        property real shiftY: 0

        // The box is the square whose corners lie as far from the center
        // as the needle's tip, so turning it never carries a corner past
        // the dial (a dial-sized box would, by up to 41%); the path
        // draws beyond the box, which a Shape doesn't clip. Centered on
        // the dial, so the box's center is the rotation's
        width: body.len * Math.SQRT2; height: width
        x: gauge.cx - width / 2 + shiftX
        y: gauge.cy - height / 2 + shiftY
        rotation: gauge.needleAngle
        opacity: gauge.hasValue || gauge.sweeping ? 1.0 : 0.4
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            id: body
            readonly property real len: gauge.r * 0.80
            readonly property real tail: gauge.r * 0.18
            readonly property real baseW: gauge.size * 0.013
            readonly property real tipW: gauge.size * 0.0035
            readonly property real tailW: gauge.size * 0.010

            strokeColor: "transparent"
            fillColor: needle.shadow ? Qt.rgba(0, 0, 0, 0.45) : "transparent"
            fillGradient: needle.shadow ? null : grad
            startX: needle.width / 2 - body.tail; startY: needle.height / 2 - body.tailW
            PathLine { x: needle.width / 2; y: needle.height / 2 - body.baseW }
            PathLine { x: needle.width / 2 + body.len; y: needle.height / 2 - body.tipW }
            PathLine { x: needle.width / 2 + body.len; y: needle.height / 2 + body.tipW }
            PathLine { x: needle.width / 2; y: needle.height / 2 + body.baseW }
            PathLine { x: needle.width / 2 - body.tail; y: needle.height / 2 + body.tailW }
            PathLine { x: needle.width / 2 - body.tail; y: needle.height / 2 - body.tailW }
        }
        LinearGradient {
            id: grad
            x1: needle.width / 2 - gauge.r * 0.18; y1: 0
            x2: needle.width / 2 + gauge.r * 0.80; y2: 0
            GradientStop { position: 0.0; color: Theme.gaugeShade.needleBase }
            GradientStop { position: 0.17; color: Theme.gaugeShade.needleBase }
            GradientStop { position: 0.19; color: Qt.darker(needle.tint, 1.25) }
            GradientStop { position: 1.0; color: Qt.lighter(needle.tint, 1.2) }
        }
    }
    Needle {
        shadow: true
        shiftX: gauge.size * 0.006; shiftY: gauge.size * 0.014
    }
    Needle {}

    // ---- Hub cap
    Rectangle {
        anchors.centerIn: parent
        width: gauge.size * 0.11; height: width; radius: width / 2
        border.color: Qt.alpha(Theme.gaugeTick, 0.10)
        border.width: 1
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.gaugeShade.hubHi }
            GradientStop { position: 1.0; color: Theme.bg }
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 0.36; height: width; radius: width / 2
            color: Qt.darker(gauge.needleColor, 1.5)
        }
    }
}
