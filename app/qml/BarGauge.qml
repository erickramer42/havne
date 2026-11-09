// File/layer: app/qml/BarGauge.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.gauges, QmlTest.dashboard

import QtQuick

pragma ComponentBehavior: Bound

// Segmented LED-style bar for percentages (throttle, load). Plain
// Rectangles: one opacity per segment changes as the value moves.
GaugeBase {
    id: gauge

    property real segmentWidth: 7     // minimum; segments stretch to fill
    property real segmentGap: 3
    property var scaleMarks: [0, 25, 50, 75, 100]   // value units; [] hides the scale
    // Standing up (segments stacked from the bottom, the scale beside) and
    // the scale's ticks on or off (ADR-0057 section 1)
    property bool vertical: false
    property bool marks: true

    // The bar's length along its own direction, and its thickness across
    readonly property real barLength: vertical ? bar.height : bar.width
    readonly property real barThickness: vertical ? bar.width : bar.height
    // At least ten when ten fit; a short bar gets as many as fit (two at
    // least), never segments narrower than they are wide across
    readonly property int segments: Math.max(2, Math.floor((barLength + segmentGap)
                                                           / (segmentWidth + segmentGap)))

    implicitHeight: vertical ? 180 : 72

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
            objectName: "readout"
            text: gauge.valueText
            font.pixelSize: 22
            font.weight: Font.DemiBold
            font.features: ({ "tnum": 1 })
            color: gauge.hasValue ? gauge.zoneColor(gauge.value, Theme.gaugeReadout) : Theme.textDim
        }
        // Past dangerFrom, beside the value
        DangerMark {
            // The slot stays once a gauge has a danger zone, so the readout
            // never moves as the mark appears
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

    Item {
        id: bar
        objectName: "bar"
        x: 0
        y: readout.height + 8
        width: gauge.vertical ? Math.min(28, gauge.width * 0.4) : gauge.width
        height: gauge.vertical ? Math.max(0, gauge.height - y - 14) : 18

        Repeater {
            model: gauge.segments
            delegate: Rectangle {
                id: seg
                objectName: "segment"
                required property int index
                // Zone color at the segment's midpoint; fill fraction
                // 0..1 so the leading segment fades rather than snaps
                readonly property color lit: gauge.zoneColor(
                    gauge.from + (index + 0.5) / gauge.segments * (gauge.to - gauge.from),
                    gauge.accent)
                readonly property real fill: Math.max(0, Math.min(1,
                    gauge.fraction * gauge.segments - index))

                // The danger zone's segments carry an outline: heavier than
                // the others, so the zone reads apart by more than its hue
                readonly property bool inDangerZone: !isNaN(gauge.dangerFrom)
                    && gauge.from + (index + 0.5) / gauge.segments * (gauge.to - gauge.from)
                       >= gauge.dangerFrom

                readonly property real along: (gauge.barLength
                    - (gauge.segments - 1) * gauge.segmentGap) / gauge.segments
                // Standing up the first segment is the lowest
                width: gauge.vertical ? bar.width : along
                height: gauge.vertical ? along : bar.height
                x: gauge.vertical ? 0 : index * (along + gauge.segmentGap)
                y: gauge.vertical ? bar.height - (index + 1) * along - index * gauge.segmentGap : 0
                radius: 2
                color: Qt.alpha(seg.lit, 0.09)
                border.width: seg.inDangerZone ? 2 : 0
                border.color: Qt.alpha(Theme.gaugeDanger, 0.55)

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

    // The held peak: a mark across the bar at its place, only when asked for
    Rectangle {
        objectName: "peakMark"
        visible: gauge.showPeak && !isNaN(gauge.peakValue)
        readonly property real at: gauge.fractionOf(isNaN(gauge.peakValue) ? gauge.from : gauge.peakValue)
        color: Theme.gaugeTick   // no zone's color: it sits over lit segments
        radius: 1
        // Inside the track at either end
        x: gauge.vertical ? bar.x - 3 : bar.x + Math.max(0, Math.min(bar.width - width, at * bar.width - width / 2))
        y: gauge.vertical ? bar.y + Math.max(0, Math.min(bar.height - height, (1 - at) * bar.height - height / 2))
                          : bar.y - 3
        width: gauge.vertical ? bar.width + 6 : 3
        height: gauge.vertical ? 3 : bar.height + 6
    }

    Item {
        objectName: "scaleMarks"
        x: gauge.vertical ? bar.x + bar.width + 6 : bar.x
        y: gauge.vertical ? bar.y : bar.y + bar.height + 4
        width: gauge.vertical ? gauge.width - x : bar.width
        height: gauge.vertical ? bar.height : 12
        visible: gauge.marks && gauge.scaleMarks.length > 0
        Repeater {
            model: gauge.scaleMarks
            delegate: Text {
                required property real modelData
                readonly property real f: gauge.fractionOf(modelData)
                // End labels align to the bar's edges, the rest center; standing
                // up the low end is at the bottom
                x: gauge.vertical ? 0 : f <= 0 ? 0 : f >= 1 ? parent.width - width : f * parent.width - width / 2
                y: gauge.vertical ? (f <= 0 ? parent.height - height
                                    : f >= 1 ? 0 : (1 - f) * parent.height - height / 2) : 0
                text: gauge.formatScale(modelData)
                font.pixelSize: 10
                font.features: ({ "tnum": 1 })
                color: Qt.alpha(Theme.textDim, 0.8)
            }
        }
    }
}
