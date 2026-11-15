// File/layer: app/qml/DigitalGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.gauges, QmlTest.accessibility,
//          QmlTest.dashboard, QmlTest.dashedit;
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
    readonly property int windowMs: 8000
    // Whether the page holding this tile is on screen: a tile on another
    // page reads nothing, and its trace is whole when it shows again
    property bool shown: true
    // What the trace reads (Bridge.signalModel, a test's stand-in) and the
    // def it reads, as SparkGauge's
    property var traceSource: null
    property int signalId: -1
    // [ts, val, ...] oldest first, as read last
    property var samples: []

    // A read every 100 ms (the bridge's tick) while shown
    Timer {
        objectName: "traceReader"
        interval: 100
        repeat: true
        running: gauge.visible && gauge.shown && !gauge.brief && gauge.traceSource !== null
                 && gauge.signalId >= 0
        triggeredOnStart: true
        onTriggered: gauge.readTrace()
    }
    function readTrace() {
        if (traceSource === null || signalId < 0)
            return
        samples = traceSource.trace(signalId, windowMs, Math.max(1, Math.floor(trace.width / 3)))
    }
    onSignalIdChanged: samples = []

    readonly property bool roomy: height >= 160
    // Too short for the trace and the footer: the label and the value alone
    readonly property bool brief: height < 84
    readonly property color levelColor: gauge.hasValue
                                        ? gauge.zoneColor(gauge.value, gauge.readoutColor)
                                        : Theme.textDim

    // The trace's pieces for a box, newest sample on the right edge, scaled
    // to the window's own low and high
    function tracePoints(w, h) {
        const e = traceExtent(samples)
        return tracePieces(samples, windowMs, w, h, e.lo, e.hi)
    }

    Column {
        id: heading
        anchors { left: parent.left; right: parent.right; top: parent.top }
        Text {
            objectName: "gaugeTitle"
            Accessible.ignored: true
            width: parent.width
            text: gauge.label
            elide: Text.ElideRight
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: gauge.titleColor
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
            visible: gauge.hasDangerZone
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
            PathMultiline { paths: trace.points }
        }
    }
    // Beside the value on a card too short for a trace; else in the trace's
    // place, or after the holds while a stale value keeps its trace
    Text {
        objectName: "gaugeNote"
        Accessible.ignored: true
        readonly property bool atHolds: !gauge.brief && gauge.hasValue
        anchors.left: gauge.brief ? valueRow.right : atHolds ? footer.right : parent.left
        anchors.leftMargin: gauge.brief || atHolds ? 8 : 0
        anchors.right: parent.right
        anchors.verticalCenter: gauge.brief ? valueBand.verticalCenter
                              : atHolds ? footer.verticalCenter : trace.verticalCenter
        horizontalAlignment: gauge.brief ? Text.AlignLeft : atHolds ? Text.AlignRight : Text.AlignHCenter
        elide: Text.ElideRight
        visible: text !== ""
        text: gauge.shownNote
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
