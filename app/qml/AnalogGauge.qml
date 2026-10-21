// File/layer: app/qml/AnalogGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: manual smoke — DashboardPage under mock://demo at 160 px
//          (numerals shrink, then thin out) through 480 px; sweep on
//          connect; tap clears the peak marker

import QtQuick
import QtQuick.Shapes

pragma ComponentBehavior: Bound

// Needle dial: tach, speedo. Face, ticks, numerals and zones are
// static Rectangles, Text and Shape arcs. Per frame only the needle's
// rotation and, with showProgress, one arc's sweep change. No Canvas:
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
    property string peakLabel: qsTr("PEAK")
    property bool showProgress: false   // lit arc from `from` to the needle
    property bool showPeak: false       // tell-tale marker and readout line
    property color needleColor: "#ff5a5f"

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

    readonly property int majorCount: Math.round((to - from) / majorStep) + 1
    readonly property int minorCount: Math.round((to - from) / minorStep) + 1
    // Numerals shrink to fit the arc between them, down to 9 px; past
    // that every other one (or more) is dropped
    readonly property real labelGap: numRadius * sweepAngle * Math.PI / 180
                                     / Math.max(1, majorCount - 1)
    readonly property real labelEm: 0.62 * String(Math.round(to / labelDivisor)).length + 0.6
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
            GradientStop { position: 0.0; color: "#3a3a52" }
            GradientStop { position: 0.45; color: "#1b1b28" }
            GradientStop { position: 1.0; color: "#2c2c42" }
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
                GradientStop { position: 0.0; color: "#25253a" }
                GradientStop { position: 1.0; color: "#0e0e15" }
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
            strokeColor: Qt.alpha(Theme.text, 0.16)
            strokeWidth: Math.max(1, gauge.size * 0.004)
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy
            radius: gauge.tickOuter - gauge.majorLen / 2
            startAngle: gauge.angleOf(gauge.warnStart)
            sweepAngle: gauge.angleOf(gauge.dangerStart) - startAngle
            strokeColor: Qt.alpha(Theme.warning, 0.14)
            strokeWidth: gauge.majorLen
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy
            radius: gauge.tickOuter - gauge.majorLen / 2
            startAngle: gauge.angleOf(gauge.dangerStart)
            sweepAngle: gauge.angleOf(gauge.to) - startAngle
            strokeColor: Qt.alpha(Theme.danger, 0.18)
            strokeWidth: gauge.majorLen
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.tickOuter
            startAngle: gauge.angleOf(gauge.warnStart)
            sweepAngle: gauge.angleOf(gauge.dangerStart) - startAngle
            strokeColor: Theme.warning
            strokeWidth: Math.max(2, gauge.size * 0.009)
        }
        GaugeArc {
            centerX: gauge.cx; centerY: gauge.cy; radius: gauge.tickOuter
            startAngle: gauge.angleOf(gauge.dangerStart)
            sweepAngle: gauge.angleOf(gauge.to) - startAngle
            strokeColor: Theme.danger
            strokeWidth: Math.max(2, gauge.size * 0.009)
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
            strokeColor: Qt.alpha(Theme.text, 0.06)
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

    // ---- Ticks: full-size items rotated about the dial centre
    Repeater {
        model: gauge.minorCount
        delegate: Item {
            id: tick
            required property int index
            readonly property real v: gauge.from + index * gauge.minorStep
            readonly property real steps: (v - gauge.from) / gauge.majorStep
            readonly property bool major: Math.abs(steps - Math.round(steps)) < 1e-6

            width: gauge.width; height: gauge.height
            rotation: gauge.angleOf(v) + 90   // 0° rotation = 12 o'clock

            Rectangle {
                antialiasing: true
                width: tick.major ? Math.max(2, gauge.size * 0.009)
                                  : Math.max(1, gauge.size * 0.0045)
                height: tick.major ? gauge.majorLen : gauge.minorLen
                x: gauge.cx - width / 2
                y: gauge.cy - gauge.tickOuter
                color: gauge.zoneColor(tick.v, tick.major ? Theme.text
                                                          : Qt.alpha(Theme.text, 0.5))
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
            text: String(Math.round(v / gauge.labelDivisor))
            x: gauge.px(a, gauge.numRadius) - width / 2
            y: gauge.py(a, gauge.numRadius) - height / 2
            font.pixelSize: gauge.numSize
            font.weight: Font.DemiBold
            font.features: ({ "tnum": 1 })
            color: gauge.zoneColor(v, Theme.text)
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
                fillColor: Theme.warning
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
        font.letterSpacing: 1
        color: Theme.textDim
    }
    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: gauge.cy + gauge.r * 0.2
        opacity: gauge.stale ? 0.45 : 1.0

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: gauge.valueText
            // Sized to clear the end numerals beside it at full scale
            font.pixelSize: Math.max(16, gauge.size * 0.1)
            font.weight: Font.DemiBold
            font.features: ({ "tnum": 1 })
            color: gauge.hasValue ? gauge.zoneColor(gauge.value, Theme.text) : Theme.textDim
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: gauge.unit
            font.pixelSize: Math.max(10, gauge.size * 0.036)
            font.letterSpacing: 1.5
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
            font.letterSpacing: 1
            font.features: ({ "tnum": 1 })
            color: gauge.hasValue ? Theme.warning : Theme.textDim
        }
    }

    // ---- Needle: shadow, then body; both drawn pointing at 3 o'clock
    component Needle: Shape {
        id: needle
        property color tint: gauge.needleColor
        property bool shadow: false

        width: gauge.width; height: gauge.height
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
            startX: gauge.cx - body.tail; startY: gauge.cy - body.tailW
            PathLine { x: gauge.cx; y: gauge.cy - body.baseW }
            PathLine { x: gauge.cx + body.len; y: gauge.cy - body.tipW }
            PathLine { x: gauge.cx + body.len; y: gauge.cy + body.tipW }
            PathLine { x: gauge.cx; y: gauge.cy + body.baseW }
            PathLine { x: gauge.cx - body.tail; y: gauge.cy + body.tailW }
            PathLine { x: gauge.cx - body.tail; y: gauge.cy - body.tailW }
        }
        LinearGradient {
            id: grad
            x1: gauge.cx - gauge.r * 0.18; y1: 0
            x2: gauge.cx + gauge.r * 0.80; y2: 0
            GradientStop { position: 0.0; color: "#2a2a38" }
            GradientStop { position: 0.17; color: "#2a2a38" }
            GradientStop { position: 0.19; color: Qt.darker(needle.tint, 1.25) }
            GradientStop { position: 1.0; color: Qt.lighter(needle.tint, 1.2) }
        }
    }
    Needle {
        shadow: true
        x: gauge.size * 0.006; y: gauge.size * 0.014
    }
    Needle {}

    // ---- Hub cap
    Rectangle {
        anchors.centerIn: parent
        width: gauge.size * 0.11; height: width; radius: width / 2
        border.color: Qt.alpha(Theme.text, 0.10)
        border.width: 1
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#4b4b63" }
            GradientStop { position: 1.0; color: "#14141c" }
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 0.36; height: width; radius: width / 2
            color: Qt.darker(gauge.needleColor, 1.5)
        }
    }
}
