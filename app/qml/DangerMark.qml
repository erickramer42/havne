// File/layer: app/qml/DangerMark.qml | Qt6 Quick + Shapes, UI layer
// Testing: QML: QmlTest.gauges, QmlTest.accessibility (test_qml)

import QtQuick
import QtQuick.Shapes

// The mark beside a readout once its value is past dangerFrom: a filled
// triangle with an exclamation point, so danger is told from warning by
// more than hue (the night palettes draw both in warm colors).
// The gauge shows it only while it applies.
Item {
    id: mark
    objectName: "dangerMark"

    property real size: 16

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    // Never a node of its own, which would be shown and hidden at each
    // crossing: the gauge's name says danger
    Accessible.ignored: true

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: Theme.gaugeDanger
            strokeWidth: Math.max(1, mark.size * 0.08)
            joinStyle: ShapePath.RoundJoin
            fillColor: Theme.gaugeDanger
            startX: mark.width / 2; startY: mark.size * 0.06
            PathLine { x: mark.width - mark.size * 0.04; y: mark.height - mark.size * 0.08 }
            PathLine { x: mark.size * 0.04; y: mark.height - mark.size * 0.08 }
            PathLine { x: mark.width / 2; y: mark.size * 0.06 }
        }
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: mark.size * 0.30
        text: "!"
        Accessible.ignored: true
        font.pixelSize: mark.size * 0.62
        font.weight: Font.Bold
        color: Theme.surface
    }
}
