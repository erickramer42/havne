// File/layer: app/qml/ShiftLightBar.qml | Qt6 Quick, UI layer
// Testing: manual smoke — DashboardPage under mock://demo; LEDs fill
//          green → amber → red through the band and flash at the shift
//          point (the 2nd/3rd-gear limiter holds show it for 5 s)

import QtQuick

pragma ComponentBehavior: Bound

// Motorsport shift-light strip driven by RPM. `from` is where the
// first LED lights, `to` the shift point; at or past `to` every LED
// flashes in Theme.info.
GaugeBase {
    id: gauge

    property int count: 15
    property real ledSpacing: 8

    smoothingMs: 60   // shift lights must not lag the engine

    readonly property bool shiftNow: hasValue && value >= to && !sweeping
    property bool flashOn: false
    Timer {
        id: flasher
        interval: 70
        repeat: true
        running: gauge.shiftNow
        onTriggered: gauge.flashOn = !gauge.flashOn
        onRunningChanged: gauge.flashOn = flasher.running
    }

    readonly property real led: Math.max(6, Math.min(height * 0.62,
        (width - (count - 1) * ledSpacing) / count))

    Row {
        anchors.centerIn: parent
        spacing: gauge.ledSpacing

        Repeater {
            model: gauge.count
            delegate: Item {
                id: slot
                required property int index
                readonly property real pos: (index + 1) / gauge.count
                readonly property color hue: pos <= 0.4 ? Theme.success
                                           : pos <= 0.74 ? Theme.warning
                                           : Theme.danger
                readonly property bool lit: gauge.shiftNow ? gauge.flashOn
                                                           : gauge.fraction >= pos - 1e-6
                readonly property color glow: gauge.shiftNow ? Theme.info : hue

                width: gauge.led; height: gauge.led

                Rectangle {   // halo
                    anchors.centerIn: parent
                    width: parent.width * 1.5; height: width; radius: width / 2
                    visible: slot.lit
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
                    color: Qt.alpha("white", slot.lit ? 0.45 : 0.07)
                }
            }
        }
    }
}
