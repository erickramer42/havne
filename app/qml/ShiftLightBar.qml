// File/layer: app/qml/ShiftLightBar.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.gauges, QmlTest.kinds, QmlTest.accessibility;
//          manual smoke: DashboardPage under mock://demo; LEDs fill
//          green -> amber -> red through the band and flash at the shift
//          point (the 2nd/3rd-gear limiter holds show it for 5 s)

import QtQuick

pragma ComponentBehavior: Bound

// Motorsport shift-light strip driven by RPM. `from` is where the
// first LED lights, `to` the shift point; at or past `to` every LED
// flashes, each in its stage's color.
GaugeBase {
    id: gauge

    property int count: 15
    property real ledSpacing: 8

    smoothingMs: 60   // shift lights must not lag the engine

    readonly property bool shiftNow: hasValue && value >= to && !sweeping
    property bool flashOn: false
    Timer {
        id: flasher
        interval: 167   // a flash of 3 Hz at most
        repeat: true
        running: gauge.shiftNow
        onTriggered: gauge.flashOn = !gauge.flashOn
        onRunningChanged: gauge.flashOn = flasher.running
    }

    readonly property real led: Math.max(6, Math.min(height * 0.62,
        (width - (count - 1) * ledSpacing) / count))

    Row {
        id: lights
        objectName: "lights"
        anchors.centerIn: parent
        spacing: gauge.ledSpacing
        opacity: gauge.stale ? 0.45 : 1.0

        Repeater {
            model: gauge.count
            delegate: Item {
                id: slot
                objectName: "slot"
                required property int index
                readonly property real pos: (index + 1) / gauge.count
                readonly property color hue: pos <= 0.4 ? Theme.success
                                           : pos <= 0.74 ? Theme.gaugeWarn
                                           : Theme.gaugeDanger
                readonly property bool lit: gauge.shiftNow ? gauge.flashOn
                                                           : gauge.fraction >= pos - 1e-6
                // A lit light keeps its stage's color, flashing or not
                readonly property color glow: hue

                width: gauge.led; height: gauge.led

                Rectangle {   // halo
                    anchors.centerIn: parent
                    width: parent.width * 1.5; height: width; radius: width / 2
                    opacity: slot.lit ? 1 : 0
                    color: Qt.alpha(slot.glow, 0.16)
                }
                Rectangle {   // lens
                    anchors.fill: parent
                    radius: width / 2
                    color: slot.lit ? slot.glow : Qt.alpha(slot.hue, 0.10)
                    border.width: 1
                    border.color: slot.lit ? Qt.lighter(slot.glow, 1.35) : Qt.alpha(slot.hue, 0.28)
                }
                Rectangle {   // specular highlight
                    x: parent.width * 0.24; y: parent.height * 0.14
                    width: parent.width * 0.42; height: width * 0.55; radius: height / 2
                    color: Qt.alpha(Theme.gaugeTick, slot.lit ? 0.45 : 0.07)
                }
            }
        }
    }

    // A stale value's word, where the strip leaves room at its end
    Text {
        objectName: "gaugeNote"
        Accessible.ignored: true
        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        visible: gauge.hasValue && gauge.stale && implicitWidth + 8 <= (gauge.width - lights.width) / 2
        text: gauge.shownNote
        font.pixelSize: 11
        color: Qt.alpha(Theme.textDim, 0.8)
    }
}
