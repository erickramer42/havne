// File/layer: app/qml/BarGauge.qml | Qt6 Quick, UI layer
// Testing: manual smoke: DashboardPage under mock://demo; segment
//          count follows width (phone ~30, desktop ~50); the leading
//          segment fades in, so motion reads smooth at any count

import QtQuick

pragma ComponentBehavior: Bound

// Segmented LED-style bar for percentages (throttle, load). Plain
// Rectangles: one opacity per segment changes as the value moves.
GaugeBase {
    id: gauge

    property real segmentWidth: 7     // minimum; segments stretch to fill
    property real segmentGap: 3
    property var scaleMarks: [0, 25, 50, 75, 100]   // value units; [] hides the scale

    readonly property int segments: Math.max(10, Math.floor((bar.width + segmentGap)
                                                            / (segmentWidth + segmentGap)))

    implicitHeight: 72

    Text {
        id: title
        anchors { left: parent.left; verticalCenter: readout.verticalCenter }
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
    Row {
        id: readout
        anchors { right: parent.right; top: parent.top }
        spacing: 3
        opacity: gauge.stale ? 0.45 : 1.0
        Text {
            id: valueLabel
            text: gauge.valueText
            font.pixelSize: 22
            font.weight: Font.DemiBold
            font.features: ({ "tnum": 1 })
            color: gauge.hasValue ? gauge.zoneColor(gauge.value, Theme.text) : Theme.textDim
        }
        Text {
            visible: gauge.hasValue
            anchors.baseline: valueLabel.baseline
            text: gauge.unit
            font.pixelSize: 12
            color: Theme.textDim
        }
    }

    Row {
        id: bar
        anchors { left: parent.left; right: parent.right; top: readout.bottom; topMargin: 8 }
        height: 18
        spacing: gauge.segmentGap

        Repeater {
            model: gauge.segments
            delegate: Rectangle {
                id: seg
                required property int index
                // Zone color at the segment's midpoint; fill fraction
                // 0..1 so the leading segment fades rather than snaps
                readonly property color lit: gauge.zoneColor(
                    gauge.from + (index + 0.5) / gauge.segments * (gauge.to - gauge.from),
                    gauge.accent)
                readonly property real fill: Math.max(0, Math.min(1,
                    gauge.fraction * gauge.segments - index))

                width: (bar.width - (gauge.segments - 1) * gauge.segmentGap) / gauge.segments
                height: bar.height
                radius: 2
                color: Qt.alpha(seg.lit, 0.09)

                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    opacity: seg.fill
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Qt.lighter(seg.lit, 1.25) }
                        GradientStop { position: 1.0; color: seg.lit }
                    }
                }
            }
        }
    }

    Item {
        anchors { left: bar.left; right: bar.right; top: bar.bottom; topMargin: 4 }
        visible: gauge.scaleMarks.length > 0
        Repeater {
            model: gauge.scaleMarks
            delegate: Text {
                required property real modelData
                readonly property real f: gauge.fractionOf(modelData)
                // End labels align to the bar's edges, the rest center
                x: f <= 0 ? 0 : f >= 1 ? parent.width - width : f * parent.width - width / 2
                text: gauge.formatScale(modelData)
                font.pixelSize: 10
                font.features: ({ "tnum": 1 })
                color: Qt.alpha(Theme.textDim, 0.8)
            }
        }
    }
}
