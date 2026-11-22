// File/layer: app/qml/SparkGauge.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.kinds, QmlTest.gauges, QmlTest.accessibility, QmlTest.dashboard{,_pages,_panel},
//          QmlTest.dashedit

import QtQuick
import QtQuick.Shapes

// One trace over a window (ADR-0057 section 1): the newest value at its end,
// the window's low and high as a scale. It draws the model's history of its
// signal (`traceSource.trace`, every window's lowest, highest and newest),
// sliced from time 0 at about one point per 1.5 px, so a spike keeps its
// height and place as the trace scrolls, and nothing is lost when its page
// is left: the ring is the model's. It reads only while its page is the one
// shown and the app is active (`shown`, from the dashboard: a SwipeView keeps
// its neighbors visible, so `visible` alone would not stop them). The trace
// is a glance, Live's chart is the record.
GaugeBase {
    id: gauge

    // The window in seconds (10 to 120)
    property int windowS: 30
    // Whether the dashboard's page holding this gauge is the one on screen
    // and the app is active
    property bool shown: true
    // What the trace reads (Bridge.signalModel, a test's stand-in) and the
    // def it reads; null or -1 reads nothing
    property var traceSource: null
    property int signalId: -1

    readonly property int span: Math.max(10, Math.min(120, Math.round(windowS)))
    readonly property int windowMs: span * 1000
    // [ts, val, ...] oldest first, as read last
    property var samples: []

    // A read every 100 ms (the bridge's tick) while shown: the window ends
    // at the newest sample, so the trace scrolls as values arrive
    Timer {
        objectName: "traceReader"
        interval: 100
        repeat: true
        running: gauge.visible && gauge.shown && gauge.traceSource !== null && gauge.signalId >= 0
        triggeredOnStart: true
        onTriggered: gauge.readTrace()
    }
    function readTrace() {
        if (traceSource === null || signalId < 0)
            return
        samples = traceSource.trace(signalId, windowMs, Math.max(1, Math.floor(trace.width / 3)))
    }
    onSignalIdChanged: samples = []

    readonly property var extent: traceExtent(samples)

    // The trace's pieces for a box
    function tracePoints(w, h) {
        return tracePieces(samples, windowMs, w, h, extent.lo, extent.hi)
    }

    implicitHeight: 96

    Text {
        textFormat: Text.PlainText
        id: title
        objectName: "gaugeTitle"
        Accessible.ignored: true
        anchors { left: parent.left; top: parent.top }
        text: gauge.label
        font.pixelSize: 12
        font.weight: Font.DemiBold
        color: gauge.titleColor
        elide: Text.ElideRight
        width: Math.max(0, Math.min(implicitWidth, gauge.width - readout.width - 8))
        visible: !gauge.noteAlone
    }
    readonly property bool noteAlone: noteTakesLine(title.implicitWidth, width - readout.width - 8)
    Text {
        textFormat: Text.PlainText
        objectName: "gaugeNote"
        Accessible.ignored: true
        anchors { left: gauge.noteAlone ? parent.left : title.right
                  leftMargin: gauge.noteAlone ? 0 : 10
                  right: readout.left; rightMargin: 8 }
        y: gauge.noteAlone ? 0 : title.y + title.baselineOffset - baselineOffset
        visible: (!gauge.hasValue || gauge.stale) && (gauge.noteAlone || width > 0)
        elide: Text.ElideRight
        wrapMode: gauge.noteAlone ? Text.WordWrap : Text.NoWrap
        maximumLineCount: gauge.noteAlone ? 2 : 1
        text: gauge.noteAlone ? gauge.titledNote : gauge.shownNote
        font.pixelSize: 11
        color: Qt.alpha(Theme.textDim, 0.7)
    }

    // The newest value, at the trace's end
    Row {
        id: readout
        anchors { right: parent.right; top: parent.top }
        spacing: 3
        opacity: gauge.stale ? 0.45 : 1.0
        Text {
            textFormat: Text.PlainText
            id: valueLabel
            objectName: "readout"
            Accessible.ignored: true
            text: gauge.valueText
            font.pixelSize: 22
            font.weight: Font.Bold
            font.family: Theme.valueFamily
            font.features: Theme.valueFeatures
            color: gauge.valueColor
        }
        DangerMark {
            visible: gauge.dangerSlot
            opacity: gauge.showsDanger ? 1 : 0
            size: 18
            anchors.verticalCenter: valueLabel.verticalCenter
        }
        Text {
            textFormat: Text.PlainText
            objectName: "unit"
            Accessible.ignored: true
            visible: gauge.hasValue
            anchors.baseline: valueLabel.baseline
            text: gauge.unit
            font.pixelSize: 12
            color: Theme.textDim
        }
    }

    // The scale: the window's high at the top, its low at the bottom
    Column {
        id: scaleBox
        objectName: "scaleBox"
        anchors { left: parent.left; top: trace.top; bottom: trace.bottom }
        // Sized from the scale's ends and a digit more, so the trace
        // doesn't move when a label gains a digit
        width: Math.max(highText.implicitWidth, lowText.implicitWidth,
                        sampleLow.implicitWidth, sampleHigh.implicitWidth)
        Text {
            textFormat: Text.PlainText
            id: highText
            objectName: "scaleHigh"
            Accessible.ignored: true
            text: gauge.samples.length > 0 ? gauge.format(gauge.extent.hi) : ""
            font.pixelSize: 10
            font.features: Theme.valueFeatures
            color: Qt.alpha(Theme.textDim, 0.8)
        }
        Item { width: 1; height: Math.max(0, scaleBox.height - highText.height - lowText.height) }
        Text {
            textFormat: Text.PlainText
            id: lowText
            objectName: "scaleLow"
            Accessible.ignored: true
            text: gauge.samples.length > 0 ? gauge.format(gauge.extent.lo) : ""
            font.pixelSize: 10
            font.features: Theme.valueFeatures
            color: Qt.alpha(Theme.textDim, 0.8)
        }
    }

    Text {
        textFormat: Text.PlainText
        id: sampleLow
        Accessible.ignored: true
        visible: false
        text: gauge.format(gauge.from) + "0"
        font.pixelSize: 10
        font.features: Theme.valueFeatures
    }
    Text {
        textFormat: Text.PlainText
        id: sampleHigh
        Accessible.ignored: true
        visible: false
        text: gauge.format(gauge.to) + "0"
        font.pixelSize: 10
        font.features: Theme.valueFeatures
    }

    Shape {
        id: trace
        objectName: "trace"
        anchors { left: scaleBox.right; leftMargin: 6; right: parent.right
                  top: readout.bottom; topMargin: 6; bottom: parent.bottom }
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
}
