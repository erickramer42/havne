// File/layer: app/qml/SegmentGauge.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.gauges, QmlTest.accessibility, QmlTest.dashboard

import QtQuick

pragma ComponentBehavior: Bound

// A tachometer's cells (ADR-0057 section 1): `cells` Rectangles in a row
// along a rising curve, each taller than the one before, lit up to the
// value. A card too narrow for them draws as many as fit at a pitch of
// 3 px plus the gap (`drawn`), so a 64-cell option on a phone's narrow card
// shows fewer, wider cells and never a collapsed row. The cell's color is
// its zone's (warm, then danger past dangerFrom, the danger cells outlined
// like the bar's), the leading cell fades in as the value reaches it, and a
// held peak is a notch over its cell. Static items built for a size; per
// frame only the opacities change.
GaugeBase {
    id: gauge

    // How many cells (2 to 64), and the curve's steepness: 0 gives cells
    // of one height, 1 a straight rise, more a curve that climbs late
    property int cells: 24
    property real rise: 1.6

    readonly property int count: Math.max(2, Math.min(64, Math.round(cells)))
    readonly property real curve: Math.max(0, Math.min(4, rise))
    readonly property real cellGap: width > 240 ? 3 : 2
    // The cells that fit at a pitch of at least 3 px
    readonly property int drawn: Math.max(2, Math.min(count,
        Math.floor((field.width + cellGap) / (cellGap + 3))))
    // The room above the tallest cell, for the peak's notch
    readonly property real headroom: 7

    // A cell's height as a fraction of the cells' room: 0.2 for the first,
    // 1 for the last
    function heightOf(i) {
        return 0.2 + 0.8 * (curve === 0 ? 1 : Math.pow(i / (drawn - 1), curve))
    }
    function valueAt(i) { return from + (i + 0.5) / drawn * (to - from) }

    implicitHeight: 96

    Text {
        id: title
        Accessible.ignored: true
        anchors { left: parent.left; top: parent.top }
        text: gauge.label
        font.pixelSize: 12
        font.weight: Font.DemiBold
        color: Theme.textDim
    }
    Text {
        objectName: "gaugeNote"
        Accessible.ignored: true
        anchors { left: title.right; leftMargin: 10; right: readout.left; rightMargin: 8
                  baseline: title.baseline }
        visible: (!gauge.hasValue || gauge.stale) && width > 0
        elide: Text.ElideRight
        text: gauge.shownNote
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
            Accessible.ignored: true
            text: gauge.valueText
            font.pixelSize: 22
            font.weight: Font.Bold
            font.family: Theme.valueFamily
            font.features: Theme.valueFeatures
            color: gauge.hasValue ? gauge.zoneColor(gauge.value, Theme.gaugeReadout) : Theme.textDim
        }
        // Past dangerFrom, beside the value; its slot stays
        DangerMark {
            visible: !isNaN(gauge.dangerFrom)
            opacity: gauge.inDanger ? 1 : 0
            size: 18
            anchors.verticalCenter: valueLabel.verticalCenter
        }
        Text {
            objectName: "unit"
            Accessible.ignored: true
            visible: gauge.hasValue
            anchors.baseline: valueLabel.baseline
            text: gauge.unit
            font.pixelSize: 12
            color: Theme.textDim
        }
    }

    Item {
        id: field
        objectName: "cellField"
        anchors { left: parent.left; right: parent.right; top: readout.bottom; topMargin: 8
                  bottom: ends.top; bottomMargin: 4 }

        Repeater {
            model: gauge.drawn
            delegate: Rectangle {
                id: cell
                required property int index
                objectName: "cell"
                readonly property color lit: gauge.zoneColor(gauge.valueAt(index), gauge.accent)
                readonly property real fill: Math.max(0, Math.min(1,
                    gauge.fraction * gauge.drawn - index))
                readonly property bool inDangerZone: !isNaN(gauge.dangerFrom)
                                                     && gauge.valueAt(index) >= gauge.dangerFrom

                width: (field.width - (gauge.drawn - 1) * gauge.cellGap) / gauge.drawn
                height: (field.height - gauge.headroom) * gauge.heightOf(index)
                x: index * (width + gauge.cellGap)
                y: field.height - height
                radius: 2
                color: Qt.alpha(cell.lit, 0.09)
                border.width: cell.inDangerZone ? 2 : 0
                border.color: Qt.alpha(Theme.gaugeDanger, 0.55)

                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    opacity: cell.fill
                    color: cell.lit
                }
            }
        }

        // The held peak: a notch over the cell it reached
        Rectangle {
            objectName: "peakNotch"
            readonly property int at: isNaN(gauge.peakValue) ? 0
                : Math.max(0, Math.min(gauge.drawn - 1,
                      Math.floor(gauge.fractionOf(gauge.peakValue) * gauge.drawn)))
            visible: gauge.showPeak && !isNaN(gauge.peakValue)
            width: (field.width - (gauge.drawn - 1) * gauge.cellGap) / gauge.drawn
            height: 3
            radius: 1
            x: at * (width + gauge.cellGap)
            y: field.height - (field.height - gauge.headroom) * gauge.heightOf(at) - 6
            // The tick color, which no zone's cell shares
            color: Theme.gaugeTick
        }
    }

    Item {
        id: ends
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: scaleFrom.implicitHeight
        Text {
            id: scaleFrom
            Accessible.ignored: true
            anchors.left: parent.left
            text: gauge.formatScale(gauge.from)
            font.pixelSize: 10
            font.features: Theme.valueFeatures
            color: Qt.alpha(Theme.textDim, 0.8)
        }
        Text {
            Accessible.ignored: true
            anchors.right: parent.right
            text: gauge.formatScale(gauge.to)
            font.pixelSize: 10
            font.features: Theme.valueFeatures
            color: Qt.alpha(Theme.textDim, 0.8)
        }
    }
}
